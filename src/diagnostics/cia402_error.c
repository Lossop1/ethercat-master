#include "emaster/diagnostics/cia402_error.h"

#include <stdio.h>

/*
 * CiA 402 / CiA 301 紧急错误码表。
 *
 * 只收录伺服驱动上见得着的那些。ETG/CiA 的码空间远宽于此，且厂商自造码大量存在，
 * 硬编一份"全表"只会造出一个看着权威、实际会漂的东西——所以宁可少列几条，把
 * 未收录的三档说清楚。真要逐条对齐某个型号，那要拿它的手册来做，不是在这里猜。
 */
typedef struct
{
    uint16_t code;
    const char *text;
} cia402_error_entry_t;

static const cia402_error_entry_t CIA402_ERROR_TABLE[] = {
    {0x0000, "无故障"},
    {0x1000, "通用错误"},

    /* 电流 */
    {0x2310, "过电流"},
    {0x2320, "电机侧过电流"},
    {0x2330, "对地漏电"},

    /* 电压 */
    {0x3110, "主电源过压"},
    {0x3120, "主电源欠压"},
    {0x3210, "直流母线过压"},
    {0x3220, "直流母线欠压"},

    /* 温度 */
    {0x4210, "设备过温"},
    {0x4310, "驱动器/电机过温"},

    /* 软件与参数 */
    {0x6010, "软件复位（看门狗）"},
    {0x6320, "参数错误"},

    /* 反馈 */
    {0x7300, "传感器错误"},
    {0x7305, "编码器错误"},
    {0x7310, "旋变错误"},
    {0x7320, "旋变错误（解析失败）"},

    /* 运动 */
    {0x8611, "位置跟随误差"},
    {0x8612, "位置超限"},
};

bool emaster_cia402_error_is_known(uint16_t code)
{
    const size_t count = sizeof(CIA402_ERROR_TABLE) / sizeof(CIA402_ERROR_TABLE[0]);
    for (size_t index = 0; index < count; ++index)
    {
        if (CIA402_ERROR_TABLE[index].code == code)
        {
            return true;
        }
    }
    return false;
}

size_t emaster_cia402_error_describe(uint16_t code, char *buffer, size_t capacity)
{
    if (buffer == NULL || capacity == 0U)
    {
        return 0U;
    }

    const char *text = NULL;
    char fallback[32];
    const size_t count = sizeof(CIA402_ERROR_TABLE) / sizeof(CIA402_ERROR_TABLE[0]);
    for (size_t index = 0; index < count; ++index)
    {
        if (CIA402_ERROR_TABLE[index].code == code)
        {
            text = CIA402_ERROR_TABLE[index].text;
            break;
        }
    }

    if (text == NULL)
    {
        /*
         * 未收录。三档，按 CiA 402 的划界：
         *   bit15 置位      —— 厂商自定义码（本表无法给名字）。
         *   0x6000-0x7FFF   —— 数据/通信类（参数、配置、通信）。
         *   其余            —— 本表没收录的标准码。
         * 三档都点明"这不是没故障"，免得读的人把空白当正常。
         *
         * 拼进定长小缓冲再统一拷出，与 al_status.c 同理：只留一个 snprintf 出口。
         */
        const char *shape = (code & 0x8000U) != 0U ? "厂商私有码(0x%04X)"
                            : (code >= 0x6000U && code <= 0x7FFFU) ? "数据/通信类码(0x%04X)"
                                                                    : "未收录(0x%04X)";
        (void)snprintf(fallback, sizeof(fallback), shape, (unsigned int)code);
        text = fallback;
    }

    const int written = snprintf(buffer, capacity, "%s", text);
    /* 截断时也要 NUL 结尾：MinGW 在 -std=c11 下的 snprintf 截断不写终止符。 */
    buffer[capacity - 1U] = '\0';
    return written < 0 ? 0U : (size_t)written;
}
