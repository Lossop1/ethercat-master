/*
 * 总线扫描：一次 ecx_config_init 拿到环上的全部从站，再逐个读它们的身份与换算参数。
 *
 * 存在的理由：主站那些工具都要求"先有一份声明了轴数与身份的部署档"，于是最前面的
 * 那个问题——线接好了没有、到底扫到几台、每一台是什么——反而没有工具能回答。接错
 * 一根线时主站报的是"身份或 PDO 映射与部署配置不一致"，而"环上到底有几台、不一致的
 * 是哪一台"它不说；换算参数（减速比、编码器分辨率）对不上时主站也只说"位置换算无效"。
 *
 * 所以本工具回答三件事，全部按实测印出来：
 *   1. 环上有几台；
 *   2. 每一台的身份三件套（对部署档的 identity）；
 *   3. 每一台的 0x6091（减速比）与 0x608F（编码器分辨率）——运动方案里的
 *      expected_position_scale 就该照这两个数写。
 *
 * 它不读部署档、不发过程数据、不进 OP：读完把全体请求回 INIT 再关闭。
 * 用法:
 *   emaster-scan <接口>        例: emaster-scan enp49s0
 *   不带参数时只列出可用的网络适配器。
 */

#define _POSIX_C_SOURCE 200809L

/* 这个 fork 的伞头文件是 soem/soem.h，没有上游那个 soem/ethercat.h；
 * 而且它只提供 context 版 API（ecx_*），没有 ec_init／ec_slave 那一套。 */
#include "soem/soem.h"

#include "emaster/cli.h"

#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* 读一个 u32 对象。ecx_SDOread 返回的是工作计数器：>0 才算这一条真的回来了；
 * 宽度也必须相符，"读回来 2 个字节"不等于"读到了一个 u32"。 */
static bool read_u32(ecx_contextt *context, uint16_t slave, uint16_t index, uint8_t subindex,
                     uint32_t *out)
{
    uint32_t value = 0U;
    int size = (int)sizeof(value);

    if (ecx_SDOread(context, slave, index, subindex, FALSE, &size, &value, EC_TIMEOUTRXM) <= 0 ||
        size != (int)sizeof(value))
    {
        return false;
    }
    *out = value;
    return true;
}

/*
 * 一个 u16 对象。CiA 402 的状态字与故障码都是 u16。
 *
 * 与 read_u32 同样的理由：读不到返回 false，由调用方印 na，不印 0 —— 0 在状态字里
 * 是有含义的值（"没准备"），与"没读回来"必须分得开。
 */
static bool read_u16(ecx_contextt *context, uint16_t slave, uint16_t index, uint8_t subindex,
                     uint16_t *out)
{
    uint16_t value = 0U;
    int size = (int)sizeof(value);

    if (ecx_SDOread(context, slave, index, subindex, FALSE, &size, &value, EC_TIMEOUTRXM) <= 0 ||
        size != (int)sizeof(value))
    {
        return false;
    }
    *out = value;
    return true;
}

/*
 * 一个 u32 对象印成十进制，读不到印 na —— 0 在减速比／分辨率里是非法值，
 * 印 0 会被当成"驱动器报的就是 0"，而它其实是"这一条没读回来"。 */
static void format_u32(char *out, size_t size, bool read, uint32_t value)
{
    if (read)
    {
        (void)snprintf(out, size, "%u", (unsigned)value);
    }
    else
    {
        (void)snprintf(out, size, "na");
    }
}

/* 用法里带上网卡清单：这是唯一一处能列出本机可用网卡的地方，出错时也要看得到。 */
static void print_usage(FILE *out, const char *program)
{
    ec_adaptert *adapter = ec_find_adapters();

    fprintf(out, "用法: %s <接口>\n", program);
    fprintf(out, "  --help: 打印本说明后退出\n");
    fprintf(out, "  --version: 打印版本号后退出\n");
    fprintf(out, "\n可用的网络适配器:\n");
    for (ec_adaptert *entry = adapter; entry != NULL; entry = entry->next)
    {
        fprintf(out, "  %s\t%s\n", entry->name, entry->desc);
    }
    ec_free_adapters(adapter);
}

int main(int argc, char *argv[])
{
    ecx_contextt context;

    switch (emaster_cli_info(argc, argv))
    {
    case EMASTER_CLI_HELP:
        print_usage(stdout, argv[0]);
        return 0;
    case EMASTER_CLI_VERSION:
        emaster_cli_print_version("emaster-scan");
        return 0;
    default:
        break;
    }
    if (argc < 2)
    {
        /* 没有接口名就做不了任何事，所以无参数出帮助并退出 0。 */
        print_usage(stdout, argv[0]);
        return 0;
    }
    /*
     * 本工具只收一个位置参数：网卡名。以 `-` 开头的一律当选项。
     *
     * 先前它不查这个，`--bogus` 会被当成网卡名一路走到 ecx_init 失败，再被误报成
     * "需要 root"——操作员照提示去 sudo，还是同一个失败，查不到真正的错。用法错误
     * 就用用法错误的退出码。`--` 之后的那一个才算网卡名。
     */
    if (strcmp(argv[1], "--") == 0)
    {
        if (argc < 3)
        {
            print_usage(stderr, argv[0]);
            return 2;
        }
        argv[1] = argv[2];
    }
    else if (argv[1][0] == '-')
    {
        fprintf(stderr, "错误：不认识的选项 '%s'\n", argv[1]);
        print_usage(stderr, argv[0]);
        return 2;
    }

    printf("interface=%s\n", argv[1]);
    /*
     * 先把 ecx_init 失败的两种常见原因分开说。它自己只会回一句失败，而这两件事的
     * 处置完全不同：非 root 是"换个方式重来"，网卡名错是"名字打错了"。
     * 不提权限时，权限不足会一直被读成"网卡名或 up 状态有问题"，照着这条线索查下去
     * 永远查不到。两个判据都只在本工具里问，不引新依赖。
     */
    if (geteuid() != 0)
    {
        fprintf(stderr, "错误：需要 root —— 扫描要开原始套接字，用 sudo 重跑\n");
        return 1;
    }
    if (if_nametoindex(argv[1]) == 0U)
    {
        fprintf(stderr, "错误：网卡 %s 不存在（用不带参数的 %s 列出可用的网卡）\n",
                argv[1], argv[0]);
        return 1;
    }
    if (!ecx_init(&context, argv[1]))
    {
        fprintf(stderr, "错误：ecx_init 失败，接口 %s 打不开（网卡 up 了吗？）\n", argv[1]);
        return 1;
    }

    /*
     * **到达时的状态，必须在 ecx_config_init 之前读。**
     *
     * ecx_config_init() 自己会发出 INIT -> PRE-OP 请求（见下面那段注释），所以它之后
     * 读到的状态是**本工具造出来的**，不是从站原来的。2026-10-08 台架上正是这一点造成
     * 了分歧：操作员摸到电机在使能保持（OP），而本工具报 PRE-OP —— 因为工具先把它们
     * 拉下来了。**仪器改变了被观测对象，然后报告了自己造成的那个状态。**
     *
     * 这里读的是全体从站 AL 状态（0x0130）的按位或，形状与 SOEM 自己的 ecx_readstate
     * 一致：一条 2 字节广播数据报，**只读不改**。或值足以回答"有没有从站在 OP"：
     * OP(8) 的第 3 位一旦置起，或值里就看得见。它答不了"哪几台在 OP"，但那正是
     * 后面逐站读要回答的事 —— 而逐站读必须在改状态之后才能做，这是无法回避的顺序。
     */
    uint16_t al_state_or_at_arrival = 0U;
    bool al_state_or_valid = false;
    {
        uint16_t raw = 0U;
        const int wkc = ecx_BRD(&context.port, 0x0000, ECT_REG_ALSTAT, sizeof(raw), &raw,
                                EC_TIMEOUTRET);
        if (wkc > 0)
        {
            al_state_or_at_arrival = (uint16_t)(etohs(raw) & 0x000fU);
            al_state_or_valid = true;
        }
    }
    if (al_state_or_valid)
    {
        printf("al_state_at_arrival_or=%u\n", (unsigned)al_state_or_at_arrival);
        /* 第 3 位是 OP。把它单独说清楚：这是全工具里唯一一条"没有被自己改过"的状态读数。 */
        printf("al_state_at_arrival_has_op=%s\n",
               (al_state_or_at_arrival & 0x08U) != 0U ? "yes" : "no");
    }
    else
    {
        /* 读不到就写 na，不写 0：0 不是合法的 AL 状态，写成 0 会被当成"没读到"以外的意思。 */
        printf("al_state_at_arrival_or=na\n");
    }

    const int slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        fprintf(stderr, "错误：一台从站都没扫到（slavecount=%d）\n", context.slavecount);
        ecx_close(&context);
        return 1;
    }

    /*
     * ecx_config_init() 只发出 INIT -> PRE-OP 请求，不保证转换已在返回前完成。
     * 邮箱访问前必须显式等待并确认全体从站到达 PRE-OP，否则快速设备偶然成功、
     * 慢设备全部 SDO 超时，形成不可复现的竞态。
     */
    bool preop_reached = ecx_statecheck(&context, 0U, EC_STATE_PRE_OP,
                                        EC_TIMEOUTSTATE * 4) == EC_STATE_PRE_OP;
    if (!preop_reached)
    {
        /*
         * **重试一次。**
         *
         * ecx_config_init 里那次 PRE-OP 请求（ec_config.c:459）返回值被丢弃、也不重试，
         * 而实测（2026-10-08）它有一次不生效：从站在 config_init 之后仍停在 INIT，
         * 而在它返回之后**再写一次同样的 PRE-OP|ACK，从站就进了 PRE-OP**
         * （alctl_wkc=1、state_after=2、errbit=0）。
         *
         * 所以"进不去 PRE-OP"不一定是从站的问题 —— 值得先重试一次再下结论。
         */
        fprintf(stderr, "提示：config_init 之后未到 PRE-OP，逐台重写一次请求\n");
        for (int index = 1; index <= context.slavecount; ++index)
        {
            uint16_t request = htoes((uint16_t)(EC_STATE_PRE_OP | EC_STATE_ACK));
            (void)ecx_FPWR(&context.port, context.slavelist[index].configadr, ECT_REG_ALCTL,
                           sizeof(request), &request, EC_TIMEOUTRET3);
        }
        {
            const struct timespec settle = {0, 50000000L};   /* 50 ms 给全体走完转移 */
            (void)nanosleep(&settle, NULL);
        }
        preop_reached = ecx_statecheck(&context, 0U, EC_STATE_PRE_OP,
                                       EC_TIMEOUTSTATE * 4) == EC_STATE_PRE_OP;
        if (!preop_reached)
        {
            fprintf(stderr, "警告：重试后仍未到达 PRE-OP，换算参数一律印 na\n");
        }
    }
    /* 重新读一次各站状态：上面那次 statecheck 读的是地址 0 的广播结果。 */
    ecx_readstate(&context);

    printf("slaves=%d\n", slave_count);
    for (int index = 1; index <= context.slavecount; ++index)
    {
        const ec_slavet *slave = &context.slavelist[index];
        uint32_t gear_motor = 0U;
        uint32_t gear_shaft = 0U;
        uint32_t encoder_increments = 0U;
        uint32_t encoder_revolutions = 0U;
        char gear_motor_text[24];
        char gear_shaft_text[24];
        char encoder_increments_text[24];
        char encoder_revolutions_text[24];
        bool gear_read = false;
        bool encoder_read = false;
        uint32_t max_motor_speed = 0U;
        uint32_t profile_velocity = 0U;
        uint32_t profile_acceleration = 0U;
        uint32_t profile_deceleration = 0U;
        char max_motor_speed_text[24];
        char profile_velocity_text[24];
        char profile_acceleration_text[24];
        char profile_deceleration_text[24];
        bool max_motor_speed_read = false;
        bool profile_velocity_read = false;
        bool profile_acceleration_read = false;
        bool profile_deceleration_read = false;

        if (preop_reached && (slave->mbx_proto & ECT_MBXPROT_COE) != 0U)
        {
            /* 0x6091 减速比：子索引 1=电机转数、2=输出轴转数。运动方案里的
             * gear_motor_revolutions／gear_shaft_revolutions 就是这两个数。 */
            gear_read = read_u32(&context, (uint16_t)index, UINT16_C(0x6091), UINT8_C(0x01),
                                 &gear_motor) &&
                        read_u32(&context, (uint16_t)index, UINT16_C(0x6091), UINT8_C(0x02),
                                 &gear_shaft);
            /* 0x608F 编码器分辨率：子索引 1=增量数、2=电机转数。 */
            encoder_read = read_u32(&context, (uint16_t)index, UINT16_C(0x608F), UINT8_C(0x01),
                                    &encoder_increments) &&
                           read_u32(&context, (uint16_t)index, UINT16_C(0x608F), UINT8_C(0x02),
                                    &encoder_revolutions);
        }

        /*
         * CiA 402 层的两个读数，走邮箱（SDO）。
         *
         * **这才是"驱动器在不在使劲"的通道。** AL 状态是 ESC 那一层：它一旦掉到 INIT，
         * 邮箱就关了，主站从此问不到驱动器自己的状态 —— 而操作员在现场摸得到电机还在
         * 保持。2026-10-08 台架上这件事重复出现：主站报 AL=INIT（如实），电机仍使能保持
         * （也如实），两者说的是两层不同的东西，而报告里一直只有一层。
         *
         * 必须在 PRE-OP（邮箱活着）时读。本工具读 0x6091/0x608F 时正好在这个窗口里，
         * 所以顺手读掉，不额外增加过程数据静默。
         */
        uint16_t statusword = 0U;
        uint16_t error_code = 0U;
        bool statusword_read = false;
        bool error_code_read = false;
        uint32_t vendor_fault = 0U;
        uint32_t vendor_ext_fault = 0U;
        bool vendor_fault_read = false;
        bool vendor_ext_fault_read = false;
        if (preop_reached && (slave->mbx_proto & ECT_MBXPROT_COE) != 0U)
        {
            statusword_read = read_u16(&context, (uint16_t)index, UINT16_C(0x6041),
                                       UINT8_C(0x00), &statusword);
            error_code_read = read_u16(&context, (uint16_t)index, UINT16_C(0x603F),
                                       UINT8_C(0x00), &error_code);
            /*
             * **厂商故障码。这台驱动器的真实故障码在这里，不在标准的 603F 里。**
             *
             * 203Eh「扩展伺服故障码」与 203Fh「伺服故障码」是 Uint32 的 TPDO 对象，
             * 来自厂商手册的对象组 2000h 一览。手册不在仓库里 —— 厂商资料随电机走，
             * 由客户自己持有，所以这里只记对象号与它的来处。
             * 2026-10-08 台架上 12 台驱动器的标准 603F 全是 0，而它们同时停在 CiA 402
             * 的「未就绪」——只读 603F 会把"有故障"读成"没故障"。
             *
             * 这台驱动器是 ODrive 派生固件，CiA 402 是厂商在其上做的一层（手册
             * 2830 行把 6040h 与 odrv0.axis0.requested_state 直接对上）。所以
             * "为什么未就绪"的答案常常是底层的一个故障位，而它只在这两个对象里。
             */
            vendor_fault_read = read_u32(&context, (uint16_t)index, UINT16_C(0x203F),
                                         UINT8_C(0x00), &vendor_fault);
            vendor_ext_fault_read = read_u32(&context, (uint16_t)index, UINT16_C(0x203E),
                                             UINT8_C(0x00), &vendor_ext_fault);
            /*
             * 速度与加速度的限制配置：0x6080 最大电机转速、0x6081 轮廓速度、
             * 0x6083 轮廓加速度、0x6084 轮廓减速度。
             *
             * 加这四条是因为"轴只走到指令的一部分，限制在哪"在报告里没有依据：主站从不
             * 读它们，报告里也不会出现。手册把 0x6081 的换算写死了 ——
             *   POSPLAN_MAXSPD(RPM) = ProfileVelocity / (Resolution × GearRatio) × 60
             * ——但它是不是也管 CSP，手册没说。值本身读出来之前，谈不了。
             *
             * 放在**这一组**（与 6041/603F 同批），不放在上面读 6091/608F 那一组：
             * 那组在 PRE-OP 刚落地时就发，2026-10-14 实测整组返回 na——同一轮里
             * 晚几十个邮箱往返的 6041/603F 却读得回来。先按"邮箱还没热"处理，
             * 换组重试；若这里仍是 na，那就是驱动器真的不做这几条。
             */
            max_motor_speed_read = read_u32(&context, (uint16_t)index, UINT16_C(0x6080),
                                            UINT8_C(0x00), &max_motor_speed);
            profile_velocity_read = read_u32(&context, (uint16_t)index, UINT16_C(0x6081),
                                             UINT8_C(0x00), &profile_velocity);
            profile_acceleration_read = read_u32(&context, (uint16_t)index, UINT16_C(0x6083),
                                                 UINT8_C(0x00), &profile_acceleration);
            profile_deceleration_read = read_u32(&context, (uint16_t)index, UINT16_C(0x6084),
                                                 UINT8_C(0x00), &profile_deceleration);
        }
        char statusword_text[16];
        char error_code_text[16];
        char vendor_fault_text[16];
        char vendor_ext_text[16];
        (void)snprintf(vendor_fault_text, sizeof(vendor_fault_text),
                       vendor_fault_read ? "0x%08X" : "na", (unsigned)vendor_fault);
        (void)snprintf(vendor_ext_text, sizeof(vendor_ext_text),
                       vendor_ext_fault_read ? "0x%08X" : "na", (unsigned)vendor_ext_fault);
        if (statusword_read)
        {
            (void)snprintf(statusword_text, sizeof(statusword_text), "0x%04X",
                           (unsigned)statusword);
        }
        else
        {
            (void)snprintf(statusword_text, sizeof(statusword_text), "na");
        }
        if (error_code_read)
        {
            (void)snprintf(error_code_text, sizeof(error_code_text), "0x%04X",
                           (unsigned)error_code);
        }
        else
        {
            (void)snprintf(error_code_text, sizeof(error_code_text), "na");
        }
        /*
         * 把状态字译成一句话。人要看的是"在不在使劲"，不是十六进制。
         * 判据按 CiA 402 状态机的位定义：0x0008=Fault、0x0004=Operation Enabled、
         * 0x0040=Switch On Disabled、0x0021=Ready to Switch On。
         */
        const char *cia402_state = "na";
        if (statusword_read)
        {
            if ((statusword & 0x0008U) != 0U)          { cia402_state = "fault"; }
            else if ((statusword & 0x0004U) != 0U)     { cia402_state = "operation-enabled"; }
            else if ((statusword & 0x0040U) != 0U)     { cia402_state = "switch-on-disabled"; }
            else if ((statusword & 0x0021U) == 0x0021U) { cia402_state = "ready-to-switch-on"; }
            else if ((statusword & 0x0001U) == 0U)     { cia402_state = "not-ready"; }
            else                                        { cia402_state = "switched-on"; }
        }

        format_u32(gear_motor_text, sizeof(gear_motor_text), gear_read, gear_motor);
        format_u32(gear_shaft_text, sizeof(gear_shaft_text), gear_read, gear_shaft);
        format_u32(encoder_increments_text, sizeof(encoder_increments_text), encoder_read,
                   encoder_increments);
        format_u32(encoder_revolutions_text, sizeof(encoder_revolutions_text), encoder_read,
                   encoder_revolutions);
        format_u32(max_motor_speed_text, sizeof(max_motor_speed_text), max_motor_speed_read,
                   max_motor_speed);
        format_u32(profile_velocity_text, sizeof(profile_velocity_text), profile_velocity_read,
                   profile_velocity);
        format_u32(profile_acceleration_text, sizeof(profile_acceleration_text),
                   profile_acceleration_read, profile_acceleration);
        format_u32(profile_deceleration_text, sizeof(profile_deceleration_text),
                   profile_deceleration_read, profile_deceleration);

        /*
         * state 是这一拍读回来的 AL 状态：1=INIT、2=PRE-OP、4=SAFE-OP、8=OP。
         * 读到 PRE-OP 之后正常值是 2；出现别的值说明那一台有自己的问题，
         * 而主站启动日志里"AL state="那一栏印的就是它。
         */
        /*
         * 直接探一次这一台的 AL 状态寄存器，并把**工作计数器**印出来。
         *
         * WKC 是"这一帧有没有从站处理过"的唯一凭据：=1 说明寻址正常、帧到了它；
         * =0 说明这一台根本没收到。2026-10-08 台架上 12 台全部停在 INIT、进不去
         * PRE-OP、且 AL 状态里没有 error 位 —— 没有 error 位意味着从站不认为它拒绝过，
         * 那么"请求的帧到底到没到"就是分界，而它只能靠 WKC 回答。
         */
        uint16_t al_probe = 0U;
        const int al_probe_wkc = ecx_FPRD(&context.port, slave->configadr,
                                          ECT_REG_ALSTAT, sizeof(al_probe), &al_probe,
                                          EC_TIMEOUTRET);
        /*
         * **自己写一次 AL Control = PRE-OP | ACK，并看工作计数器。**
         *
         * ecx_config_init 里那次写（ec_config.c:459）返回值被丢弃，所以"请求有没有发到
         * 从站"从来没有依据。这里补一条会看结果的写，把三种情况分开：
         *   WKC = 0            -> 帧没到（寻址或链路）
         *   WKC >= 1 且变为 2  -> 写是有效的，说明 config_init 里那一次没成
         *   WKC >= 1 且仍为 1  -> 收到了、没执行
         * 仅当 AL 状态的 error 位置起时，AL 状态码（0x0134）才有意义，所以只在那种情况下读它。
         */
        uint16_t alctl_request = htoes((uint16_t)(EC_STATE_PRE_OP | EC_STATE_ACK));
        const int alctl_wkc = ecx_FPWR(&context.port, slave->configadr, ECT_REG_ALCTL,
                                       sizeof(alctl_request), &alctl_request, EC_TIMEOUTRET3);
        /* 给从站一点时间走转移再读回。用 nanosleep 而不是 usleep：本文件定义的是
         * _POSIX_C_SOURCE 200809L，usleep 已从该标准移除，用它过不了 -Werror。 */
        {
            const struct timespec settle = {0, 20000000L};
            (void)nanosleep(&settle, NULL);
        }
        uint16_t al_after = 0U;
        const int al_after_wkc = ecx_FPRD(&context.port, slave->configadr, ECT_REG_ALSTAT,
                                          sizeof(al_after), &al_after, EC_TIMEOUTRET);
        uint16_t al_code = 0U;
        const bool al_error_bit = (etohs(al_after) & EC_STATE_ERROR) != 0U;
        if (al_error_bit)
        {
            (void)ecx_FPRD(&context.port, slave->configadr, ECT_REG_ALSTATCODE,
                           sizeof(al_code), &al_code, EC_TIMEOUTRET);
        }

        printf("slave=%d name=%s vendor=0x%08X product=0x%08X revision=0x%08X "
               "state=%u al_status=0x%04X dc=%d coe=%d mbx_rl=%u cfgadr=%u al_wkc=%d "
               "alctl_wkc=%d state_after=%u ack_wkc=%d al_code=0x%04X errbit=%d "
               "gear=%s/%s enc=%s/%s "
               "maxspeed_6080=%s pvel_6081=%s pacc_6083=%s pdec_6084=%s "
               "cia402_6041=%s cia402=%s cia402_603f=%s fault_203f=%s fault_203e=%s\n",
               index, slave->name, (unsigned)slave->eep_man, (unsigned)slave->eep_id,
               (unsigned)slave->eep_rev, (unsigned)slave->state,
               (unsigned)slave->ALstatuscode, slave->hasdc != 0U ? 1 : 0,
               (slave->mbx_proto & ECT_MBXPROT_COE) != 0U ? 1 : 0, (unsigned)slave->mbx_rl,
               (unsigned)slave->configadr, al_probe_wkc,
               alctl_wkc, (unsigned)(etohs(al_after) & 0x0fU), al_after_wkc,
               (unsigned)al_code, al_error_bit ? 1 : 0,
               gear_motor_text, gear_shaft_text, encoder_increments_text,
               encoder_revolutions_text, max_motor_speed_text, profile_velocity_text,
               profile_acceleration_text, profile_deceleration_text, statusword_text,
               cia402_state, error_code_text, vendor_fault_text, vendor_ext_text);
    }

    /* 看完就把全体请求回 INIT：本工具不参与运行，不该把从站留在 PRE-OP。 */
    context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context, 0U);
    (void)ecx_statecheck(&context, 0U, EC_STATE_INIT, EC_TIMEOUTSTATE);
    ecx_close(&context);
    return 0;
}
