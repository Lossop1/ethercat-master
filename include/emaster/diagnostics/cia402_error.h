#ifndef EMASTER_DIAGNOSTICS_CIA402_ERROR_H
#define EMASTER_DIAGNOSTICS_CIA402_ERROR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * CiA 402 故障码对象 0x603F（Error code）的解码。
 *
 * 与 AL 状态码是两层不同的事：AL 码说的是"这一站为什么没进 OP / 为什么掉出来"，
 * 0x603F 说的是"驱动器自己认为出了什么故障"。一次"纹丝不动"可以是其中之一，
 * 也可能两个都有——所以它们在报告里各占一列，不互相覆盖。
 *
 * 0x603F 的取值沿用的是 CiA 301 的紧急对象码（emergency error code）：
 * 0x2310 过电流、0x3210 母线过压……；bit15 置位表示这是厂商自定义码
 * （CiA 402 划的界），此时本表对它只能给出"厂商私有"这一档。
 *
 * **不声称完整**：驱动器厂商大量自造码，且同一数值在不同厂商那里含义未必一致。
 * 主张只有一条——读到的码要么有名字，要么被明确指出它属于哪一档，绝不静默当正常。
 */

/* 是否在表里收录了这条码（不含"厂商私有"与区间兜底）。 */
bool emaster_cia402_error_is_known(uint16_t code);

/*
 * 把 0x603F 写成可读文本，总是写出内容。0x0000 写成"无故障"，未收录的按
 * 厂商私有 / 数据通信类 / 未收录三档之一说明。返回需要的长度，行为同 snprintf；
 * 缓冲不足时截断，并且**无论哪种 snprintf 口味都保证以 NUL 结尾**（MinGW 在
 * -std=c11 下走 MSVCRT 那一支，截断时不写终止符，与 POSIX 不同）。
 */
size_t emaster_cia402_error_describe(uint16_t code, char *buffer, size_t capacity);

#endif
