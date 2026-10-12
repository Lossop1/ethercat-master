#include "emaster/diagnostics/al_status.h"

#include <stdio.h>

/*
 * AL 状态码表，按 ETG.1020 收。**不声称完整**——ETG.1020 的码空间比这里宽，
 * 而且各家从站还会自造码。这里的主张只有一条：读到的码要么有名字，要么被明确
 * 标成"未收录"，**绝不静默地当成正常**。所以 describe() 对任何输入都写出内容。
 *
 * 只收录"见得着"的那些：状态切换/邮箱、SM 与看门狗、DC 与同步、以及常见的
 * ESC 层错误。冷门区间靠 describe() 的区间兜底给一句话，不去逐条编造名字。
 */
typedef struct
{
    uint16_t code;
    const char *text;
} al_status_entry_t;

static const al_status_entry_t AL_STATUS_TABLE[] = {
    {0x0000, "无错误"},

    /* 0x0001-0x000F：通用/未指明 */
    {0x0001, "未指明的错误"},
    {0x0002, "内存不足"},
    {0x0003, "校验和错误（EEPROM）"},
    {0x0004, "EEPROM 访问非法"},
    {0x0005, "参数非法（邮箱）"},
    {0x0006, "参数非法（过程数据）"},
    {0x0007, "未知/未指明的错误"},

    /* 0x0011-0x0016：状态切换与邮箱配置 */
    {0x0011, "请求的状态切换非法"},
    {0x0012, "请求的状态未知"},
    {0x0013, "不支持 BOOT"},
    {0x0014, "没有有效的固件"},
    {0x0015, "邮箱配置非法"},
    {0x0016, "邮箱配置非法（PRE-OP）"},

    /* 0x0017-0x001F：同步管理器与看门狗 */
    {0x0017, "同步管理器配置非法"},
    {0x0018, "没有有效的输入"},
    {0x0019, "没有有效的输出"},
    {0x001A, "同步错误"},
    {0x001B, "同步管理器看门狗"},
    {0x001C, "同步管理器类型非法"},
    {0x001D, "输出配置非法"},
    {0x001E, "输入配置非法"},
    {0x001F, "看门狗配置非法"},

    /* 0x0020-0x0023：需要重新进入某个前序状态 */
    {0x0020, "从站需要冷启动"},
    {0x0021, "从站需要 INIT"},
    {0x0022, "从站需要 PRE-OP"},
    {0x0023, "从站需要 SAFE-OP"},

    /* 0x0024-0x002F：映射、设置与同步模式 */
    {0x0024, "输入映射非法"},
    {0x0025, "输出映射非法"},
    {0x0026, "设置不一致（SM 与 PDO）"},
    {0x0027, "不支持自由运行"},
    {0x0028, "不支持同步模式"},
    {0x0029, "自由运行需要三缓冲"},
    {0x002A, "后台看门狗"},
    {0x002B, "没有有效的输入与输出"},
    {0x002C, "致命的同步错误"},

    /* 0x0030-0x0037：DC 分布式时钟家族 */
    {0x0030, "DC SYNCH 配置非法"},
    {0x0031, "DC 锁存配置非法"},
    {0x0032, "PLL 错误"},
    {0x0033, "DC 同步 IO 错误"},
    {0x0034, "DC 同步超时"},
    {0x0035, "DC 同步周期非法"},
    {0x0036, "DC SYNC0 周期时间错误"},
    {0x0037, "DC SYNC1 周期时间错误"},
};

const char *emaster_al_state_text(uint16_t state_with_error_bit)
{
    const bool error_bit = (state_with_error_bit & EMASTER_AL_STATE_ERROR_BIT) != 0U;
    const uint16_t state = (uint16_t)(state_with_error_bit & EMASTER_AL_STATE_MASK);

    /* 0 是特殊值：寄存器读回 0 表示这一站根本没回话，不是"在 INIT"。 */
    if (state_with_error_bit == 0U)
    {
        return "不应答";
    }

    const char *base = NULL;
    switch (state)
    {
    case 1U: base = "INIT"; break;
    case 2U: base = "PRE-OP"; break;
    case 3U: base = "BOOT"; break;
    case 4U: base = "SAFE-OP"; break;
    case 8U: base = "OP"; break;
    default: base = "未知状态"; break;
    }

    if (state_with_error_bit == state)
    {
        return base;
    }
    if (!error_bit)
    {
        return base;
    }
    /* 带错误位：名字上明说是哪一态带着错误，别让人以为它就是"在 SAFE-OP"。 */
    switch (state)
    {
    case 1U: return "INIT（带错误位）";
    case 2U: return "PRE-OP（带错误位）";
    case 3U: return "BOOT（带错误位）";
    case 4U: return "SAFE-OP（带错误位）";
    case 8U: return "OP（带错误位）";
    default: return "未知状态（带错误位）";
    }
}

bool emaster_al_status_is_known(uint16_t code)
{
    const size_t count = sizeof(AL_STATUS_TABLE) / sizeof(AL_STATUS_TABLE[0]);
    for (size_t index = 0; index < count; ++index)
    {
        if (AL_STATUS_TABLE[index].code == code)
        {
            return true;
        }
    }
    return false;
}

bool emaster_al_status_is_synchronization(uint16_t code)
{
    switch (code)
    {
    case 0x001AU: /* 同步错误 */
    case 0x001BU: /* 同步管理器看门狗 */
    case 0x002CU: /* 致命的同步错误 */
        return true;
    default:
        break;
    }
    /* DC 家族：SYNCH 配置、锁存、PLL、同步 IO、同步超时、周期时间。 */
    return code >= 0x0030U && code <= 0x0037U;
}

size_t emaster_al_status_describe(uint16_t code, char *buffer, size_t capacity)
{
    if (buffer == NULL || capacity == 0U)
    {
        return 0U;
    }

    const char *text = NULL;
    char fallback[32];
    const size_t count = sizeof(AL_STATUS_TABLE) / sizeof(AL_STATUS_TABLE[0]);
    for (size_t index = 0; index < count; ++index)
    {
        if (AL_STATUS_TABLE[index].code == code)
        {
            text = AL_STATUS_TABLE[index].text;
            break;
        }
    }

    if (text == NULL)
    {
        /*
         * 未收录。分两档给一句有用的话，而不是干巴巴的"未知"：
         *   0x8000 及以上 —— 厂商私有码（ETG.1020 划的界）。
         *   其余          —— 本表没收录的标准码。
         * 两档都要点明"这不是没错误"，否则读的人会把空白当正常。
         *
         * 先把兜底文本拼进定长小缓冲，再统一往调用方缓冲里拷：这样只有一个
         * snprintf 出口，"保证 NUL 结尾"那条只写一次。
         */
        (void)snprintf(fallback, sizeof(fallback),
                       code >= 0x8000U ? "厂商私有码(0x%04X)" : "未收录(0x%04X)",
                       (unsigned int)code);
        text = fallback;
    }

    const int written = snprintf(buffer, capacity, "%s", text);
    /*
     * 无论如何都保证 NUL 结尾。两种 snprintf 的口味不一样：POSIX 的截断时返回
     * 需要的长度并且照样写终止符，而 MinGW 在 -std=c11 下走 MSVCRT 那一支，截断时
     * 返回 -1 且**不写终止符**。契约是"总是写出内容"，这条不该随 libc 漂。
     */
    buffer[capacity - 1U] = '\0';
    return written < 0 ? 0U : (size_t)written;
}
