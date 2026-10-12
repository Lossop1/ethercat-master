#include "emaster/audit/run_report.h"

#include "emaster/diagnostics/al_status.h"
#include "emaster/diagnostics/cia402_error.h"
#include "emaster/diagnostics/error_class.h"

#include <stddef.h>
#include <stdlib.h>

/*
 * 把报告的原始读数折成两个结论：
 *   1. 逐轴——这一轴的 AL 码与 0x603F 到底是什么，以及是从哪个坐标选来的；
 *   2. 会话——这一轮落成哪一类事故。
 *
 * 本文件是**唯一次派生**的落点：发布报告时调一次，之后报告里的分类字段就固定了。
 * 放在发布函数里而不是各个 app 里，是因为今天有两个发布者（apps/master 与
 * apps/target_probe），逐 app 调用迟早漏一个，而漏掉不会报错，只会让某一类工具
 * 产出的报告少一列。
 */

/* 从四个 AL 坐标里选一个可信的。优先级见 emaster_al_origin_t 的说明。 */
typedef struct
{
    bool read;
    uint16_t state;
    uint16_t status_code;
    uint8_t origin;
} al_point_t;

static al_point_t select_al_point(const emaster_control_session_axis_result_t *axis)
{
    /*
     * 首选"首次计数不符"那一次：它是帧刚不对、而驱动器还没被任何停机流程碰过的
     * 读数，因果方向最干净。它有显式的 al_read 伴随位，广播读齐时覆盖全部轴。
     */
    if (axis->first_mismatch_al_read)
    {
        return (al_point_t){true, axis->first_mismatch_al_state,
                            axis->first_mismatch_al_status_code,
                            EMASTER_AL_ORIGIN_FIRST_MISMATCH};
    }
    /*
     * 其余四个坐标只在**各自的读取伴随位**为真时才采信。没有这一位，state==0 就
     * 分不清"轴不应答"和"这一段压根没跑到"，而 0 恰好是两种情形的同一个编码
     * ——那正是把"没读到"判成"不应答"的那条老坑。
     */
    if (axis->shutdown_entry_al_read)
    {
        return (al_point_t){true, axis->shutdown_entry_al_state,
                            axis->shutdown_entry_al_status_code,
                            EMASTER_AL_ORIGIN_SHUTDOWN_ENTRY};
    }
    if (axis->shutdown_pre_stop_al_read)
    {
        return (al_point_t){true, axis->shutdown_pre_stop_al_state,
                            axis->shutdown_pre_stop_al_status_code,
                            EMASTER_AL_ORIGIN_SHUTDOWN_PRE_STOP};
    }
    if (axis->shutdown_exit_al_read)
    {
        return (al_point_t){true, axis->shutdown_exit_al_state,
                            axis->shutdown_exit_al_status_code,
                            EMASTER_AL_ORIGIN_SHUTDOWN_EXIT};
    }
    if (axis->shutdown_al_read)
    {
        return (al_point_t){true, axis->shutdown_al_state, axis->shutdown_al_status_code,
                            EMASTER_AL_ORIGIN_SHUTDOWN};
    }
    return (al_point_t){false, 0U, 0U, EMASTER_AL_ORIGIN_NONE};
}

/*
 * 0x603F 取哪个读数：运行期慢速轮询、停机终态诊断、SAFE-OP 读回。
 *
 * 分两轮选。第一轮找**第一个读到了且非零**的坐标——故障轮次里要解释的正是那个码，
 * 后一次读到 0x0000 不该把前面报过的故障盖掉。
 *
 * 全为零时走第二轮，取**第一个读到了**的坐标，哪怕值就是 0x0000。这一轮不能省：
 * 0x0000 的意思是"驱动器报无故障"，而"这一条从头到尾没读回来"什么也没说——两者
 * 在数字上完全一样，分开它们**只能靠 origin**。少了这一轮，报告里"读到 0"与
 * "没读到"就又合成一个样子了，而这正是本次要拆开的两种情形。
 */
static void select_drive_error(const emaster_control_session_axis_result_t *axis,
                               uint16_t *code, uint8_t *origin)
{
    *code = 0U;
    *origin = EMASTER_DRIVE_ORIGIN_NONE;

    if (axis->runtime_error_code_read && axis->runtime_error_code != 0U)
    {
        *code = axis->runtime_error_code;
        *origin = EMASTER_DRIVE_ORIGIN_RUNTIME;
        return;
    }
    if (axis->drive_diagnostic.read_succeeded && axis->drive_diagnostic.cia402_error_code != 0U)
    {
        *code = axis->drive_diagnostic.cia402_error_code;
        *origin = EMASTER_DRIVE_ORIGIN_SHUTDOWN;
        return;
    }
    if (axis->safeop_drive_diagnostic.read_succeeded &&
        axis->safeop_drive_diagnostic.cia402_error_code != 0U)
    {
        *code = axis->safeop_drive_diagnostic.cia402_error_code;
        *origin = EMASTER_DRIVE_ORIGIN_SAFEOP;
        return;
    }

    /* 第二轮：都读到了 0。记下"读到过"，值就是 0。 */
    if (axis->runtime_error_code_read)
    {
        *origin = EMASTER_DRIVE_ORIGIN_RUNTIME;
        return;
    }
    if (axis->drive_diagnostic.read_succeeded)
    {
        *origin = EMASTER_DRIVE_ORIGIN_SHUTDOWN;
        return;
    }
    if (axis->safeop_drive_diagnostic.read_succeeded)
    {
        *origin = EMASTER_DRIVE_ORIGIN_SAFEOP;
    }
}

static uint64_t max_sync0_late_run(const emaster_control_session_report_t *report)
{
    uint64_t maximum = 0U;
    for (size_t index = 0U; index < report->axis_count; ++index)
    {
        const uint64_t candidate = report->axes[index].timing.sync0_late_max_consecutive;
        if (candidate > maximum)
        {
            maximum = candidate;
        }
    }
    return maximum;
}

/*
 * 状态 -> 类别。**没有 default，靠 -Wswitch -Werror 编译器强制穷尽 31 个值**：
 * 新增一个控制状态而不给它归类，这里就编译不过。这就是"完整性审计"的活性判据
 * ——不靠谁来记得同步一份文档。
 *
 * 只有 WKC_MISMATCH 走 derive()：其余 30 个状态各自就是一个明确的成因，不需要
 * 再判；而 WKC_MISMATCH 是个桶，装的事故要靠计数器与逐轴 AL 证据才分得开。
 */
static emaster_error_class_t class_of_status(const emaster_control_session_report_t *report,
                                             emaster_error_class_t wkc_class)
{
    switch (report->status)
    {
    case EMASTER_CONTROL_SESSION_OK:
        /*
         * 跑完了也可能是"中间抖过"。计数全为 0 才算真的无故障；否则照常分类——
         * 一次跑完但抖过的轮次，其形态（短帧还是同步）就是要能看出来。
         */
        if (report->wkc_error_count == 0U && report->wkc_no_frame_count == 0U)
        {
            return EMASTER_ERROR_CLASS_NONE;
        }
        return wkc_class;

    case EMASTER_CONTROL_SESSION_INVALID_ARGUMENT:
        return EMASTER_ERROR_CLASS_CONFIG;

    case EMASTER_CONTROL_SESSION_INTERFACE_OPEN_FAILED:
    case EMASTER_CONTROL_SESSION_INTERFACE_NOT_READY:
        return EMASTER_ERROR_CLASS_BUS_INTERFACE;

    case EMASTER_CONTROL_SESSION_NO_SLAVES:
        return EMASTER_ERROR_CLASS_BUS_NO_SLAVES;

    case EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH:
    case EMASTER_CONTROL_SESSION_IDENTITY_MISMATCH:
        return EMASTER_ERROR_CLASS_BUS_TOPOLOGY;

    case EMASTER_CONTROL_SESSION_PDO_MISMATCH:
    case EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED:
    case EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED:
    case EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED:
        return EMASTER_ERROR_CLASS_BUS_CONFIG;

    case EMASTER_CONTROL_SESSION_PREOP_NOT_REACHED:
    case EMASTER_CONTROL_SESSION_SAFE_OP_NOT_REACHED:
    case EMASTER_CONTROL_SESSION_OP_NOT_REACHED:
        return EMASTER_ERROR_CLASS_BUS_STARTUP_STATE;

    case EMASTER_CONTROL_SESSION_OUT_OF_MEMORY:
        return EMASTER_ERROR_CLASS_INTERNAL;

    case EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED:
    case EMASTER_CONTROL_SESSION_SYNC0_CONFIG_FAILED:
        return EMASTER_ERROR_CLASS_BUS_DC_CONFIG;

    case EMASTER_CONTROL_SESSION_WKC_MISMATCH:
        return wkc_class;

    case EMASTER_CONTROL_SESSION_FEEDBACK_INVALID:
    case EMASTER_CONTROL_SESSION_CONTROLLER_FAILED:
        return EMASTER_ERROR_CLASS_DRIVE_FEEDBACK;

    case EMASTER_CONTROL_SESSION_DRIVE_FAULT:
    case EMASTER_CONTROL_SESSION_ALL_AXES_FAULTED:
        return EMASTER_ERROR_CLASS_DRIVE_FAULT;

    case EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE:
        return EMASTER_ERROR_CLASS_DRIVE_LIMIT;

    case EMASTER_CONTROL_SESSION_MOTION_INVALID:
    case EMASTER_CONTROL_SESSION_FOLLOWING_ERROR:
        return EMASTER_ERROR_CLASS_MOTION;

    case EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED:
    case EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED:
        return EMASTER_ERROR_CLASS_BUS_CYCLE_TIMING;

    case EMASTER_CONTROL_SESSION_DC_SYNC_FAILED:
        return EMASTER_ERROR_CLASS_BUS_DC_SYNC;

    case EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED:
    case EMASTER_CONTROL_SESSION_RESTORE_INIT_FAILED:
        return EMASTER_ERROR_CLASS_SHUTDOWN;

    case EMASTER_CONTROL_SESSION_AUDIT_FAILED:
        return EMASTER_ERROR_CLASS_AUDIT;
    }
    /*
     * 走到这里说明上面漏了一个枚举值。编译器本该以 -Wswitch 拦下，兜底的这一句是
     * 防"有人加了 default 把警告消掉"——那种改动会让漏分类变成静默的。
     */
    return EMASTER_ERROR_CLASS_UNCLASSIFIED;
}

void emaster_run_report_classify(emaster_control_session_report_t *report)
{
    if (report == NULL)
    {
        return;
    }

    emaster_error_class_evidence_t evidence = {false, false, 0U, 0U, 0U, 0U};
    emaster_error_class_t wkc_class = EMASTER_ERROR_CLASS_UNCLASSIFIED;

    /*
     * 建一份逐轴事实给纯函数用。分配失败时**不带轴证据**继续分类：31 个状态里
     * 只有 WKC 那一支需要轴证据，其余照样能分；退化成 UNCLASSIFIED 也好过让
     * 整条发布路径失败。
     */
    emaster_error_axis_facts_t *facts = NULL;
    if (report->axis_count > 0U)
    {
        facts = malloc(report->axis_count * sizeof(*facts));
    }

    for (size_t index = 0U; index < report->axis_count; ++index)
    {
        emaster_control_session_axis_result_t *axis = &report->axes[index];
        const al_point_t point = select_al_point(axis);

        axis->al_status_code_decoded = point.status_code;
        axis->al_status_code_origin = point.origin;
        select_drive_error(axis, &axis->drive_error_code_decoded, &axis->drive_error_code_origin);

        if (facts != NULL)
        {
            facts[index].al_read = point.read;
            facts[index].state = point.state;
            facts[index].status_code = point.status_code;
        }
    }

    emaster_error_class_facts_t class_facts = {
        .wkc_error_count = report->wkc_error_count,
        .wkc_no_frame_count = report->wkc_no_frame_count,
        .wkc_short_frame_window_max = report->wkc_short_frame_window_max,
        .sync0_late_max_consecutive = max_sync0_late_run(report),
        .axes = facts,
        .axis_count = facts == NULL ? 0U : (unsigned)report->axis_count};

    wkc_class = emaster_error_class_derive(&class_facts, &evidence);
    free(facts);

    report->error_class = class_of_status(report, wkc_class);
    report->error_class_evidence_no_frame = evidence.evidence_no_frame;
    report->error_class_evidence_sync = evidence.evidence_sync;
    report->error_class_silent_axes = evidence.silent_axis_count;
    report->error_class_sync_axes = evidence.sync_axis_count;
    report->error_class_first_silent_axis = evidence.first_silent_axis;
    report->error_class_first_sync_axis = evidence.first_sync_axis;
}
