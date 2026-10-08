/*
 * 让驱动器真正失能：通过**邮箱 SDO** 往控制字 0x6040 写 Disable Voltage。
 *
 * 存在的理由。主站的"禁用"只走 PDO（session_shutdown.c 的安全停机走的是周期交换），
 * 而 PDO 只在 OP 下有效。**从站一旦掉出 OP，那条"禁用"就到不了它** —— 驱动器保持
 * 着最后一次收到的"使能"，功率级继续带电，操作员在现场摸得到电机仍在保持。
 * 2026-10-08 实测：ESC 的 AL 状态已经是 INIT，而 12 台驱动器的 0x6041 全是 0x1237
 * （Operation enabled + Voltage enabled）。报告里主站如实写着"退出 OP"，现场如实
 * 感觉得到电机在使劲，两者说的是两层不同的东西。
 *
 * 邮箱在 PRE-OP 下仍然可用（只有 INIT 才关闭），所以这条路不依赖 OP。
 *
 * **本工具会让电机立刻失去保持转矩。** 带重力负载、或抱闸未抱时不要跑 —— 那是掉负载，
 * 不是掉使能。它不做任何确认，因为"问一句再动"在自动化里等于没做；该由调用方决定。
 *
 * 用法:
 *   emaster-disable <接口>              例: emaster-disable enp49s0
 *   不带参数时只列出可用的网络适配器。
 *
 * 输出每台一行，印的是**改之前**与**改之后**的 0x6041，以及状态字译出的一句话：
 * 写没写进去、驱动器认没认，看的是 after 那一列实际变成了什么，不是看写调用的返回值。
 */

#define _POSIX_C_SOURCE 200809L

#include "soem/soem.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* CiA 402 控制字（0x6040）。只列本工具用得到的两个。 */
#define CIA_CONTROLWORD_DISABLE_VOLTAGE 0x0000U

/* CiA 402 状态字（0x6041）里判状态的位。 */
#define CIA_STATUS_FAULT 0x0008U
#define CIA_STATUS_OPERATION_ENABLED 0x0004U
#define CIA_STATUS_VOLTAGE_ENABLED 0x0010U
#define CIA_STATUS_SWITCH_ON_DISABLED 0x0040U

static bool read_u16(ecx_contextt *context, uint16_t slave, uint16_t index, uint16_t *out)
{
    uint16_t value = 0U;
    int size = (int)sizeof(value);

    if (ecx_SDOread(context, slave, index, 0U, FALSE, &size, &value, EC_TIMEOUTRXM) <= 0 ||
        size != (int)sizeof(value))
    {
        return false;
    }
    *out = value;
    return true;
}

/* 把状态字译成一句话。人要看的是"还在不在使劲"，不是十六进制。 */
static const char *describe(uint16_t statusword, bool read_ok)
{
    if (!read_ok)
    {
        return "na";
    }
    if ((statusword & CIA_STATUS_FAULT) != 0U)
    {
        return "fault";
    }
    if ((statusword & CIA_STATUS_OPERATION_ENABLED) != 0U)
    {
        return (statusword & CIA_STATUS_VOLTAGE_ENABLED) != 0U ? "enabled+powered" : "enabled";
    }
    if ((statusword & CIA_STATUS_SWITCH_ON_DISABLED) != 0U)
    {
        return "switch-on-disabled";
    }
    return "not-enabled";
}

int main(int argc, char *argv[])
{
    ecx_contextt context;

    memset(&context, 0, sizeof(context));
    if (argc != 2)
    {
        ec_adaptert *adapter = ec_find_adapters();

        printf("用法: %s <接口>   例: %s enp49s0\n", argv[0], argv[0]);
        printf("可用适配器:\n");
        for (ec_adaptert *item = adapter; item != NULL; item = item->next)
        {
            printf("  %s  %s\n", item->name, item->desc);
        }
        ec_free_adapters(adapter);
        return 2;
    }

    printf("interface=%s\n", argv[1]);
    if (!ecx_init(&context, argv[1]))
    {
        fprintf(stderr, "错误：ecx_init 失败，接口 %s 打不开\n", argv[1]);
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
        fprintf(stderr, "警告：全体未在超时内到达 PRE-OP，邮箱可能不可用，逐台的写会失败\n");
    }
    ecx_readstate(&context);

    printf("slaves=%d\n", slave_count);
    int disabled = 0;
    int still_enabled = 0;
    int write_failed = 0;
    for (int index = 1; index <= context.slavecount; ++index)
    {
        const ec_slavet *slave = &context.slavelist[index];
        uint16_t before = 0U;
        uint16_t after = 0U;
        const bool before_ok = read_u16(&context, (uint16_t)index, UINT16_C(0x6041), &before);
        uint16_t controlword = CIA_CONTROLWORD_DISABLE_VOLTAGE;
        const int wkc = ecx_SDOwrite(&context, (uint16_t)index, UINT16_C(0x6040), 0U, FALSE,
                                     (int)sizeof(controlword), &controlword, EC_TIMEOUTRXM);
        const bool after_ok = read_u16(&context, (uint16_t)index, UINT16_C(0x6041), &after);

        /*
         * 判定看的是 after 这一列**实际变成了什么**，不是写调用的返回值：写成功而驱动器
         * 因为别的原因仍保持使能，是这里真正要抓的形状。
         */
        const bool now_enabled = after_ok &&
                                 (after & CIA_STATUS_OPERATION_ENABLED) != 0U;
        if (wkc <= 0)
        {
            ++write_failed;
        }
        else if (now_enabled)
        {
            ++still_enabled;
        }
        else
        {
            ++disabled;
        }

        printf("slave=%d state=%u 6041_before=0x%04X(%s) 6041_after=0x%04X(%s) write_wkc=%d%s\n",
               index, (unsigned)slave->state, (unsigned)before, describe(before, before_ok),
               (unsigned)after, describe(after, after_ok), wkc,
               wkc <= 0 ? " 写失败" : (now_enabled ? " 仍使能" : ""));
    }

    printf("disabled=%d still_enabled=%d write_failed=%d\n", disabled, still_enabled,
           write_failed);

    /* 禁用之后再请求全体回 INIT：功率级已断，此时回到 INIT 是安全的。 */
    context.slavelist[0].state = EC_STATE_INIT;
    (void)ecx_writestate(&context, 0U);
    ecx_close(&context);
    return (write_failed == 0 && still_enabled == 0) ? 0 : 1;
}
