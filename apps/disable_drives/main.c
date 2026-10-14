/*
 * 走邮箱 SDO 给 0x6040 下发 CiA 402 下行序列（0x06 → 0x07 → 0x00）。
 * 主站的禁用走过程数据，只在 OP 下有效；邮箱在 INIT 以上都可用。
 *
 * **本工具会让电机立刻失去保持转矩。带重力负载、或抱闸未抱时不要跑。**
 *
 * 用法:
 *   emaster-disable <接口>              写：走 0x06/0x07/0x00
 *   emaster-disable <接口> --read       只读：不写任何对象
 */

#define _POSIX_C_SOURCE 200809L

#include "soem/soem.h"

#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* CiA 402 下行序列（厂商手册）。 */
static const uint16_t CIA_CONTROLWORD_SEQUENCE[] = {0x0006U, 0x0007U, 0x0000U};

/* 0x6041 的位。 */
#define CIA_STATUS_FAULT 0x0008U
#define CIA_STATUS_OPERATION_ENABLED 0x0004U
#define CIA_STATUS_VOLTAGE_ENABLED 0x0010U
#define CIA_STATUS_SWITCH_ON_DISABLED 0x0040U

static bool read_u16(ecx_contextt *context, uint16_t slave, uint16_t index, uint16_t subindex,
                     uint16_t *out)
{
    uint16_t value = 0U;
    int size = (int)sizeof(value);

    if (ecx_SDOread(context, slave, index, (uint8_t)subindex, FALSE, &size, &value,
                    EC_TIMEOUTRXM) <= 0 ||
        size != (int)sizeof(value))
    {
        return false;
    }
    *out = value;
    return true;
}

static bool read_i16(ecx_contextt *context, uint16_t slave, uint16_t index, uint16_t subindex,
                     int16_t *out)
{
    int16_t value = 0;
    int size = (int)sizeof(value);

    if (ecx_SDOread(context, slave, index, (uint8_t)subindex, FALSE, &size, &value,
                    EC_TIMEOUTRXM) <= 0 ||
        size != (int)sizeof(value))
    {
        return false;
    }
    *out = value;
    return true;
}

static bool read_u32(ecx_contextt *context, uint16_t slave, uint16_t index, uint16_t subindex,
                     uint32_t *out)
{
    uint32_t value = 0U;
    int size = (int)sizeof(value);

    if (ecx_SDOread(context, slave, index, (uint8_t)subindex, FALSE, &size, &value,
                    EC_TIMEOUTRXM) <= 0 ||
        size != (int)sizeof(value))
    {
        return false;
    }
    *out = value;
    return true;
}

/* 读不到写 na，不带标记。 */
static void format_tagged(char *out, size_t size, bool ok, uint32_t value, const char *tag)
{
    if (ok)
    {
        (void)snprintf(out, size, "%lu%s", (unsigned long)value, tag);
    }
    else
    {
        (void)snprintf(out, size, "na");
    }
}

/* 一律 says- 前缀：读数，不是事实。 */
static const char *describe(uint16_t statusword, bool read_ok)
{
    if (!read_ok)
    {
        return "na";
    }
    if ((statusword & CIA_STATUS_FAULT) != 0U)
    {
        return "says-fault";
    }
    if ((statusword & CIA_STATUS_OPERATION_ENABLED) != 0U)
    {
        return (statusword & CIA_STATUS_VOLTAGE_ENABLED) != 0U ? "says-enabled+powered"
                                                               : "says-enabled";
    }
    if ((statusword & CIA_STATUS_SWITCH_ON_DISABLED) != 0U)
    {
        return "says-switch-on-disabled";
    }
    return "says-not-enabled";
}

/* 倒出 SOEM 攒的 SDO 报错，中止码是判据。 */
static void drain_sdo_errors(ecx_contextt *context)
{
    ec_errort error;

    while (ecx_poperror(context, &error))
    {
        printf("      SDO错误 slave=%u index=0x%04X sub=%u abort=0x%08X\n",
               (unsigned)error.Slave, (unsigned)error.Index, (unsigned)error.SubIdx,
               (unsigned)error.AbortCode);
    }
}

int main(int argc, char *argv[])
{
    ecx_contextt context;
    bool read_only = false;

    memset(&context, 0, sizeof(context));
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--read") != 0))
    {
        ec_adaptert *adapter = ec_find_adapters();

        printf("用法: %s <接口> [--read]   例: %s enp49s0 --read\n", argv[0], argv[0]);
        printf("可用适配器:\n");
        for (ec_adaptert *item = adapter; item != NULL; item = item->next)
        {
            printf("  %s  %s\n", item->name, item->desc);
        }
        ec_free_adapters(adapter);
        return 2;
    }
    read_only = (argc == 3);

    printf("interface=%s mode=%s\n", argv[1], read_only ? "read" : "disable");
    /*
     * 非 root 与网卡名错是两件事，处置也不同，先把它们分开说。否则权限不足会被一路
     * 读成"网卡打不开"，顺着这条线索查下去永远查不到。两个判据都只在本工具里问。
     */
    if (geteuid() != 0)
    {
        fprintf(stderr, "错误：需要 root —— 访问总线要开原始套接字，用 sudo 重跑\n");
        return 1;
    }
    if (if_nametoindex(argv[1]) == 0U)
    {
        fprintf(stderr, "错误：网卡 %s 不存在（不带参数运行可列出可用的网卡）\n", argv[1]);
        return 1;
    }
    if (!ecx_init(&context, argv[1]))
    {
        fprintf(stderr, "错误：ecx_init 失败，接口 %s 打不开（网卡 up 了吗？）\n", argv[1]);
        return 1;
    }

    const int slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        fprintf(stderr, "错误：一台从站都没扫到（slavecount=%d）\n", context.slavecount);
        ecx_close(&context);
        return 1;
    }

    /* 邮箱访问前必须确认到达 PRE-OP，否则 SDO 会不可复现地超时。 */
    const bool preop = ecx_statecheck(&context, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 4) ==
                       EC_STATE_PRE_OP;
    if (!preop)
    {
        /* 广播读：任一台不在 PRE-OP 这一项就不成立。 */
        fprintf(stderr, "注意：并非全体都在超时内到达 PRE-OP，下面逐台看谁读不到\n");
    }
    ecx_readstate(&context);

    printf("slaves=%d\n", slave_count);
    printf("标记：says- = 状态字读值（真使能时 6041 也读到 0x1250）；"
           "[上次OP] = 出 OP 后不再更新的缓存值。\n");
    const size_t step_count = sizeof(CIA_CONTROLWORD_SEQUENCE) / sizeof(CIA_CONTROLWORD_SEQUENCE[0]);
    int says_off = 0;
    int says_on = 0;
    int unverified = 0;
    for (int index = 1; index <= context.slavecount; ++index)
    {
        const ec_slavet *slave = &context.slavelist[index];
        const uint16_t slave_id = (uint16_t)index;
        uint16_t before = 0U;
        const bool before_ok = read_u16(&context, slave_id, UINT16_C(0x6041), 0U, &before);

        int16_t current = 0;
        const bool current_ok = read_i16(&context, slave_id, UINT16_C(0x6078), 0U, &current);
        uint32_t bus = 0U;
        const bool bus_ok = read_u32(&context, slave_id, UINT16_C(0x6079), 0U, &bus);
        int16_t mosfet = 0;
        const bool mosfet_ok = read_i16(&context, slave_id, UINT16_C(0x200B), 1U, &mosfet);

        char current_text[24];
        char bus_text[24];
        char mosfet_text[24];
        format_tagged(current_text, sizeof(current_text), current_ok, (uint32_t)(uint16_t)current,
                      "");
        format_tagged(bus_text, sizeof(bus_text), bus_ok, bus, "[上次OP]");
        format_tagged(mosfet_text, sizeof(mosfet_text), mosfet_ok, (uint32_t)(uint16_t)mosfet,
                      "[上次OP]");

        printf("slave=%2d state=%2u eep=%08lX/%08lX/%08lX 6041=0x%04X(%s) current_permille=%-6s "
               "bus_mV=%-16s mosfet_dC=%s\n",
               index, (unsigned)slave->state, (unsigned long)slave->eep_man,
               (unsigned long)slave->eep_id, (unsigned long)slave->eep_rev, (unsigned)before,
               describe(before, before_ok), current_text, bus_text, mosfet_text);

        if (read_only)
        {
            if (!before_ok)
            {
                ++unverified;
            }
            else if ((before & CIA_STATUS_OPERATION_ENABLED) != 0U)
            {
                ++says_on;
            }
            else
            {
                ++says_off;
            }
            continue;
        }

        uint16_t after = 0U;
        bool after_ok = false;
        for (size_t step = 0U; step < step_count; ++step)
        {
            const uint16_t controlword = CIA_CONTROLWORD_SEQUENCE[step];
            const int wkc = ecx_SDOwrite(&context, slave_id, UINT16_C(0x6040), 0U, FALSE,
                                         (int)sizeof(controlword), &controlword, EC_TIMEOUTRXM);
            if (wkc <= 0)
            {
                drain_sdo_errors(&context);
            }

            uint16_t readback = 0U;
            const bool readback_ok = read_u16(&context, slave_id, UINT16_C(0x6040), 0U, &readback);
            after_ok = read_u16(&context, slave_id, UINT16_C(0x6041), 0U, &after);
            if (!after_ok)
            {
                after = 0U;
            }

            printf("    写 0x6040=0x%04X wkc=%d  读回 0x6040=0x%04X(%s)  6041=0x%04X(%s)\n",
                   (unsigned)controlword, wkc, (unsigned)readback, readback_ok ? "ok" : "na",
                   (unsigned)after, describe(after, after_ok));
        }

        /* 只记录 after 读到什么，不下使能结论。 */
        const bool now_enabled = after_ok && (after & CIA_STATUS_OPERATION_ENABLED) != 0U;
        if (!after_ok)
        {
            ++unverified;
        }
        else if (now_enabled)
        {
            ++says_on;
        }
        else
        {
            ++says_off;
        }
    }

    if (read_only)
    {
        printf("read_only statusword_says_on=%d says_off=%d unreadable=%d\n", says_on, says_off,
               unverified);
    }
    else
    {
        printf("写后状态字 says_on=%d says_off=%d unreadable=%d\n", says_on, says_off, unverified);
        printf("says_off 不是“已停用”：真使能时同样读到这个值。\n");
        /* 收尾回 INIT。 */
        context.slavelist[0].state = EC_STATE_INIT;
        (void)ecx_writestate(&context, 0U);
    }
    ecx_close(&context);
    return (unverified == 0 && says_on == 0) ? 0 : 1;
}
