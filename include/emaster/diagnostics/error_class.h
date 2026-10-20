#ifndef EMASTER_DIAGNOSTICS_ERROR_CLASS_H
#define EMASTER_DIAGNOSTICS_ERROR_CLASS_H

#include "emaster/diagnostics/al_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 会话的**错误分类**——回答"这一轮到底出了什么事"，而不是"哪一步失败了"。
 *
 * 与 emaster_control_session_status_t 的分工，是本文件存在的全部理由：
 *   那个是**机器码**：控制流判决，返回给调用栈，决定停机怎么走，整数值已冻在
 *     归档报告里，只能追加不能改。
 *   这个是**人读桶**：诊断分类，可以把一个机器码拆成几种真的事故。
 *
 * 今天的现场就是反例：四种完全不同的事故（环尾丢站、整帧缺失、短帧、从站同步错）
 * 全被压成同一个 WKC_MISMATCH，要看出差别只能事后人工翻异常表。分类落进报告之后，
 * 差别是报告里的一列。
 *
 * 本文件是纯逻辑：不碰 SOEM、不碰 pthread，因此开发机上就能单测。
 */

typedef enum
{
    /* 规则没覆盖到。这是 **bug 探测器，不是正常值**——出现它说明有一条失败路径
     * 没被分类规则接住，该去补规则，而不是当它"没事"。 */
    EMASTER_ERROR_CLASS_UNCLASSIFIED = 0,

    /* 会话正常结束，且全程没有计数异常。 */
    EMASTER_ERROR_CLASS_NONE,

    /* ── 配置与启动期 ── */
    EMASTER_ERROR_CLASS_CONFIG,            /* 调用方参数非法 */
    EMASTER_ERROR_CLASS_BUS_INTERFACE,     /* 网卡/套接字打不开或没就绪 */
    EMASTER_ERROR_CLASS_BUS_NO_SLAVES,     /* 总线上一个从站都没有 */
    EMASTER_ERROR_CLASS_BUS_TOPOLOGY,      /* 拓扑/身份不符：站序、位置、型号 */
    EMASTER_ERROR_CLASS_BUS_CONFIG,        /* PDO 不符、过程映像失败、SDO 写/回读失败 */
    EMASTER_ERROR_CLASS_BUS_STARTUP_STATE, /* PRE-OP/SAFE-OP/OP 没到 */
    EMASTER_ERROR_CLASS_BUS_DC_CONFIG,     /* DC / SYNC0 配置失败 */

    /* ── 运行期通信 ── */
    EMASTER_ERROR_CLASS_BUS_DC_SYNC,       /* 运行期 DC / SYNC0 同步失败 */
    EMASTER_ERROR_CLASS_BUS_CYCLE_TIMING,  /* 周期等待失败 / 截止错失 */
    /* ★A 类：整帧缺失，且有轴完全不应答（AL state=0）。指向环尾/物理层。 */
    EMASTER_ERROR_CLASS_BUS_RING_TAIL_LOST,
    /* 整帧缺失，但每一站都还在回话。中间某站或单站问题，不是环尾。 */
    EMASTER_ERROR_CLASS_BUS_LINK_DROPPED,
    /* ★B 类：有轴报同步类 AL 码（或 SYNC0 连续迟到），指向同步。 */
    EMASTER_ERROR_CLASS_BUS_SYNCHRONIZATION,
    /* 帧回来了但计数短，且没有 AL 码/同步证据。 */
    EMASTER_ERROR_CLASS_BUS_SHORT_FRAME,

    /* ── 驱动器侧 ── */
    EMASTER_ERROR_CLASS_DRIVE_FAULT,       /* 驱动器自报故障（0x603F / 状态字故障位） */
    EMASTER_ERROR_CLASS_DRIVE_FEEDBACK,    /* 反馈/控制器：编码器、控制器失败 */
    EMASTER_ERROR_CLASS_DRIVE_LIMIT,       /* 内部限位/限幅生效 */

    /* ── 其余 ── */
    EMASTER_ERROR_CLASS_MOTION,            /* 运动层：目标非法、跟随误差 */
    EMASTER_ERROR_CLASS_SHUTDOWN,          /* 停机阶段失败：安全停、恢复 INIT */
    /* 停机未确认：轴已离开运行使能，但没命中配置的停机判据。与 SHUTDOWN 分开，
     * 因为处置相反——这一档要查的是判据本身（掩码没填），那一档要查总线或驱动器。 */
    EMASTER_ERROR_CLASS_SHUTDOWN_UNCONFIRMED,
    EMASTER_ERROR_CLASS_AUDIT,             /* 审计层失败 */
    EMASTER_ERROR_CLASS_INTERNAL           /* 内存等内部失败 */
} emaster_error_class_t;

/* 稳定 ASCII 键（"BUS_RING_TAIL_LOST" 等）。**这是给分析脚本的接口**：报告里
 * 写的是这个键，Python 侧读它，不镜像任何码表。 */
const char *emaster_error_class_key(emaster_error_class_t id);

/* 稳定数字编号，给现场电话/工单用（"E130" 比念一个长键快）。编号一经发布
 * 不再改变，也不随枚举顺序走——分配记录在 docs/product/error-codes.md。 */
uint16_t emaster_error_class_code(emaster_error_class_t id);

/* 中文文本，进控制台与报告，给人读。两者都保证不返回 NULL。 */
const char *emaster_error_class_text(emaster_error_class_t id);

/* 下一步做什么（一句中文）。进控制台与报告；不许写成"请联系厂商"这种
 * 哪一类都用得上的话——每一类必须给出可执行的排查方向。 */
const char *emaster_error_class_next_step(emaster_error_class_t id);

/* ── 派生 ────────────────────────────────────────────────────────────────
 *
 * 只负责把 **WKC 一类失败**（以及"跑完了但中间抖过"）细分成上面那几个通信类。
 * 其余 30 个控制状态由调用方（audit 侧那个无 default 的 switch）一对一映射，
 * 那部分靠编译期强制穷尽，不在这里。
 *
 * 为什么拆两处：本文件必须是不依赖 pthread 的纯函数，才能开发机上单测；而
 * "31 个状态一个不漏"要靠 -Wswitch -Werror 兜。混在一起就二选一——要么丢掉
 * 编译期穷尽，要么让纯函数拖上 pthread。
 */

/* 逐轴 AL 证据。al_read 是**伴随位**：没读到的那一轴，state/status_code 是 0，
 * 而 0 恰好又是"这一站不应答"的编码。少了它，"没读到"与"真不应答"就分不开
 * ——这正是仓库里反复踩的那个坑。 */
typedef struct
{
    bool al_read;
    uint16_t state;       /* 含 EMASTER_AL_STATE_ERROR_BIT */
    uint16_t status_code;
} emaster_error_axis_facts_t;

typedef struct
{
    /* 计数用**计数器**而不是最后那一拍的 actual_wkc：报告里的 actual_wkc 只描述
     * 最后一次交换，"整场有没有出现过整帧缺失"要看计数器。 */
    uint64_t wkc_error_count;
    uint64_t wkc_no_frame_count;
    uint64_t wkc_short_frame_window_max;
    /* 主站侧的独立同步证据：SYNC0 连续迟到的峰值（报告里逐轴 timing 的最大值）。
     * 它不依赖 AL 码读没读到，所以 AL 读数缺失时它仍能把 B 类认出来。
     *
     * 用**原始峰值**而不是 report->dc_late_threshold_exceeded：后者在默认策略下
     * 恒为假（dc_late_recovery.enabled=false 且阈值为 0，策略本意是"先不中止、
     * 只把峰值量出来再定阈值"，见 session_control.c 里那段说明）。 */
    uint64_t sync0_late_max_consecutive;
    const emaster_error_axis_facts_t *axes;
    unsigned axis_count;
} emaster_error_class_facts_t;

typedef struct
{
    bool evidence_no_frame;      /* 整帧缺失的签名出现过 */
    bool evidence_sync;          /* 同步证据成立（AL 码那一支或 SYNC0 迟到那一支） */
    unsigned silent_axis_count;  /* 有 AL 读数、且完全不应答的轴数 */
    unsigned sync_axis_count;    /* 报同步类 AL 码的轴数（0 而 evidence_sync 为真
                                  *  表示同步证据只来自 SYNC0 迟到那一支） */
    unsigned first_silent_axis;  /* 1 基轴号；0 = 无 */
    unsigned first_sync_axis;    /* 1 基轴号；0 = 无 */
} emaster_error_class_evidence_t;

/*
 * 按固定的先后规则给出分类。前一条命中即返回——顺序是有意的：
 *   · 整帧缺失压过同步，因为帧根本没回来时，这一拍的"同步错"多半只是它的后果；
 *   · 不应答压过单纯整帧缺失，因为"有站彻底不说话"比"帧没回来"更具体。
 * evidence 可以传 NULL；非 NULL 时总是被完整填写（两项证据独立于命中了哪条规则，
 * 所以"同类别但证据不同"仍分得开）。
 */
emaster_error_class_t emaster_error_class_derive(const emaster_error_class_facts_t *facts,
                                                emaster_error_class_evidence_t *evidence);

#endif
