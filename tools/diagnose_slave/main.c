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

#include <stdbool.h>
#include <stdio.h>

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

/* 一个 u32 对象印成十进制，读不到印 na —— 0 在减速比／分辨率里是非法值，
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

int main(int argc, char *argv[])
{
    ecx_contextt context;

    if (argc < 2)
    {
        ec_adaptert *adapter = ec_find_adapters();

        fprintf(stderr, "用法: %s <接口>\n\n可用的网络适配器:\n", argv[0]);
        for (ec_adaptert *entry = adapter; entry != NULL; entry = entry->next)
        {
            fprintf(stderr, "  %s\t%s\n", entry->name, entry->desc);
        }
        ec_free_adapters(adapter);
        return 1;
    }

    printf("interface=%s\n", argv[1]);
    if (!ecx_init(&context, argv[1]))
    {
        fprintf(stderr, "错误：ecx_init 失败，接口 %s 打不开（网卡名对不对？网卡 up 了吗？）\n",
                argv[1]);
        return 1;
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
    const bool preop_reached = ecx_statecheck(&context, 0U, EC_STATE_PRE_OP,
                                              EC_TIMEOUTSTATE * 4) == EC_STATE_PRE_OP;
    if (!preop_reached)
    {
        fprintf(stderr, "警告：全体未在超时内到达 PRE-OP，换算参数一律印 na\n");
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

        format_u32(gear_motor_text, sizeof(gear_motor_text), gear_read, gear_motor);
        format_u32(gear_shaft_text, sizeof(gear_shaft_text), gear_read, gear_shaft);
        format_u32(encoder_increments_text, sizeof(encoder_increments_text), encoder_read,
                   encoder_increments);
        format_u32(encoder_revolutions_text, sizeof(encoder_revolutions_text), encoder_read,
                   encoder_revolutions);

        /*
         * state 是这一拍读回来的 AL 状态：1=INIT、2=PRE-OP、4=SAFE-OP、8=OP。
         * 读到 PRE-OP 之后正常值是 2；出现别的值说明那一台有自己的问题，
         * 而主站启动日志里"AL state="那一栏印的就是它。
         */
        printf("slave=%d name=%s vendor=0x%08X product=0x%08X revision=0x%08X "
               "state=%u al_status=0x%04X dc=%d coe=%d mbx_rl=%u gear=%s/%s enc=%s/%s\n",
               index, slave->name, (unsigned)slave->eep_man, (unsigned)slave->eep_id,
               (unsigned)slave->eep_rev, (unsigned)slave->state,
               (unsigned)slave->ALstatuscode, slave->hasdc != 0U ? 1 : 0,
               (slave->mbx_proto & ECT_MBXPROT_COE) != 0U ? 1 : 0, (unsigned)slave->mbx_rl,
               gear_motor_text, gear_shaft_text, encoder_increments_text,
               encoder_revolutions_text);
    }

    /* 看完就把全体请求回 INIT：本工具不参与运行，不该把从站留在 PRE-OP。 */
    context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context, 0U);
    (void)ecx_statecheck(&context, 0U, EC_STATE_INIT, EC_TIMEOUTSTATE);
    ecx_close(&context);
    return 0;
}
