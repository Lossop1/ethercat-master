#ifndef EMASTER_DIAGNOSTICS_AL_STATUS_H
#define EMASTER_DIAGNOSTICS_AL_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * EtherCAT 从站 AL 状态寄存器（0x0130）与 AL 状态码寄存器（0x0134）的解码。
 *
 * 这两层东西经常被混为一谈，混了就看不出故障的形状：
 *   AL **state** —— 这一站现在在哪一态：1=INIT、2=PRE-OP、3=BOOT、4=SAFE-OP、8=OP，
 *                   第 5 位（0x10）是与上 ESC_STATE_ERROR 的错误指示位。
 *                   值为 0 有特殊含义：这一站**根本没回话**（寄存器读回 0），
 *                   不是"在 INIT"。
 *   AL **status code** —— 为什么没进到目标态、或为什么从 OP 掉了出来。
 *                   实测那次掉出 OP 的形状就是 state=20（=SAFE-OP|错误位）、code=0x001A。
 *
 * 本文件只做"数字 -> 名字"，不含任何总线或会话语义，因此能被审计、控制台与测试
 * 共同使用，而不必把任何一方拉进 SOEM（见 control_session.h 里"总线层不解释厂商
 * 故障码"那条纪律）。
 */

/* AL state 的低 4 位是状态本身，第 5 位是错误指示。 */
enum
{
    EMASTER_AL_STATE_MASK = 0x0FU,
    EMASTER_AL_STATE_ERROR_BIT = 0x10U
};

/* AL state 寄存器原值 -> 人读名字。含错误位时名字里会带上它。
 * 未收录的值也给得出文本（不会返回 NULL）。 */
const char *emaster_al_state_text(uint16_t state_with_error_bit);

/* 是否在表里收录了这条 AL 状态码。 */
bool emaster_al_status_is_known(uint16_t code);

/*
 * 这条 AL 状态码属不属于"同步"一族。
 *
 * 分类器判 B 类错误就靠它——把码表放在这里、而不是让分类器自己另抄一份，
 * 是为了让"哪些码算同步"只有一个定义处（见 docs/test-plan.md 里"两处各写各的
 * 会漂"那条）。
 *
 * 收录：0x001A 同步错误、0x001B SM 看门狗、0x002C 致命同步错误，
 *       以及 DC 家族 0x0030-0x0037。
 * **刻意收录不进来的**：0x0016（Pre-OP 邮箱配置）——它是配置/启动方向的证据，
 * 不是同步。这条边界写在这里，不靠调用者记。
 */
bool emaster_al_status_is_synchronization(uint16_t code);

/*
 * 把 AL 状态码写成可读文本，总是写出内容（未收录的写成"未收录(0x00FE)"），
 * 因此读的人不会把"没收录"当成"没错误"。返回需要的长度，行为同 snprintf。
 * 缓冲不足时截断，并且**无论哪种 snprintf 口味都保证以 NUL 结尾**（MinGW 在
 * -std=c11 下走 MSVCRT 那一支，截断时不写终止符，与 POSIX 不同）。
 * 0x0000 写成"无错误"。
 */
size_t emaster_al_status_describe(uint16_t code, char *buffer, size_t capacity);

#endif
