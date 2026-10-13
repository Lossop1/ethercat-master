/* CiA 402 状态字解码的回归基线。
 *
 * 这个测试不是在断言"解码是对的"——它断言的是**当前已知的行为是什么**，这样
 * 谁改了掩码或映射就得先看到自己动了什么，而不是悄悄改掉。
 *
 * 要钉住的那条反常识事实：掩码是 0x006F，而 0x1250 & 0x006F == 0x40
 * （switch-on-disabled）。所以一台**真使能时报 0x1250** 的驱动器会被解码成
 * "未使能"，停机判据里也就会被判成"已安全"。台架上实测到过这样的驱动器
 * （见 docs/test-plan.md §2.1）。
 *
 * 这**不是**说解码错了：标准里 0x1250 的高位是厂商/模式位，位定义之外的东西
 * 主站看不见。所以本轮的处理是把它固化成基线，而不是改状态机——改状态机需要先
 * 在台架上一轮运动过程中连续采 0x6041 看它到底怎么变，那是另一件事。
 *
 * 若哪天有人真的改了掩码：这个测试会红，红得正好——那是故意设的信号，不是噪音。
 * 红了两边都要动：这里的期望值，以及 docs/test-plan.md §2.1 的说明。
 */

#include <stdio.h>
#include <string.h>

#include "emaster/cia402/controller.h"

static int failures = 0;

static void check_int(const char *label, long actual, long expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "%s：实际 %ld，期望 %ld\n", label, actual, expected);
        failures++;
    }
}

static void check_true(const char *label, int condition)
{
    if (!condition)
    {
        fprintf(stderr, "%s：不成立\n", label);
        failures++;
    }
}

int main(void)
{
    emaster_cia402_state_t state = EMASTER_CIA402_STATE_UNKNOWN;
    bool known = false;

    /* 标准八个状态各自解码正确。这是规范行为，改了就是错。 */
    static const struct
    {
        uint16_t word;
        const char *label;
        emaster_cia402_state_t expected;
    } standard[] = {
        {0x0000U, "not-ready-to-switch-on", EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON},
        {0x0040U, "switch-on-disabled", EMASTER_CIA402_STATE_SWITCH_ON_DISABLED},
        {0x0021U, "ready-to-switch-on", EMASTER_CIA402_STATE_READY_TO_SWITCH_ON},
        {0x0023U, "switched-on", EMASTER_CIA402_STATE_SWITCHED_ON},
        {0x0027U, "operation-enabled", EMASTER_CIA402_STATE_OPERATION_ENABLED},
        {0x0007U, "quick-stop-active", EMASTER_CIA402_STATE_QUICK_STOP_ACTIVE},
        {0x000FU, "fault-reaction-active", EMASTER_CIA402_STATE_FAULT_REACTION_ACTIVE},
        {0x0008U, "fault", EMASTER_CIA402_STATE_FAULT},
    };
    for (size_t i = 0; i < sizeof(standard) / sizeof(standard[0]); i++)
    {
        state = EMASTER_CIA402_STATE_UNKNOWN;
        known = emaster_cia402_decode_status_word(standard[i].word, &state);
        check_true(standard[i].label, known);
        check_int(standard[i].label, (long)state, (long)standard[i].expected);
    }

    /* 台架实测的使能态 0x1237（Operation enabled + Voltage enabled）掩码后落在标准
     * 0x0027 那一档；高位（模式和厂商位）不参与解码。真正的使能态在标准的 0x006F
     * 掩码下就长这样，钉住它。 */
    static const uint16_t enabled_lookalikes[] = {0x1237U};
    for (size_t i = 0; i < sizeof(enabled_lookalikes) / sizeof(enabled_lookalikes[0]); i++)
    {
        char label[64];
        snprintf(label, sizeof(label), "0x%04X 掩码后落在 operation-enabled", enabled_lookalikes[i]);
        state = EMASTER_CIA402_STATE_UNKNOWN;
        known = emaster_cia402_decode_status_word(enabled_lookalikes[i], &state);
        check_true(label, known);
        check_int(label, (long)state, (long)EMASTER_CIA402_STATE_OPERATION_ENABLED);
    }

    /* 反常识那条：真使能时读 0x1250，掩码后等于 switch-on-disabled 的 0x0040。
     * 也就是"看起来是停的，其实是转的"。0x1250 & 0x006F == 0x0040 是算术事实。 */
    check_int("0x1250 & 0x006F", (long)(0x1250U & 0x006FU), 0x0040L);
    state = EMASTER_CIA402_STATE_UNKNOWN;
    check_true("0x1250 有解码结果", emaster_cia402_decode_status_word(0x1250U, &state));
    check_int("0x1250 解码", (long)state, (long)EMASTER_CIA402_STATE_SWITCH_ON_DISABLED);

    /* 掩码之外的信息没有被丢掉：同一份 0x1250 里 voltage/remote/厂商位照样读得出来。
     * 报告里的使能指示按固定 bit2 走，这条只是说"它不是唯一的信息来源"。 */
    {
        emaster_cia402_status_t status;
        memset(&status, 0, sizeof(status));
        check_true("0x1250 解析成完整状态", emaster_cia402_decode_status(0x1250U, &status));
        check_true("0x1250 的电压位", status.voltage_enabled);
        check_int("0x1250 的厂商位", (long)status.manufacturer_specific_bits, 0L);

        memset(&status, 0, sizeof(status));
        check_true("0x1237 解析成完整状态", emaster_cia402_decode_status(0x1237U, &status));
        check_true("0x1237 的电压位", status.voltage_enabled);
    }

    if (failures != 0)
    {
        fprintf(stderr, "CiA 402 解码基线：%d 项不符\n", failures);
        return 1;
    }
    printf("CiA 402 解码基线：全部符合当前已知行为\n");
    return 0;
}
