#include "emaster/diagnostics/error_class.h"

/*
 * 类别表。键是稳定 ASCII（进 JSON，给分析脚本读），文本是中文（进控制台与报告，给人读）。
 * 两者都**不许随重构改名**：键改名等于让归档报告与脚本对不上。
 */
typedef struct
{
    const char *key;
    const char *text;
} error_class_entry_t;

static const error_class_entry_t CLASS_TABLE[] = {
    [EMASTER_ERROR_CLASS_UNCLASSIFIED] = {"UNCLASSIFIED", "未分类（规则没接住，属缺陷）"},
    [EMASTER_ERROR_CLASS_NONE] = {"NONE", "无故障"},
    [EMASTER_ERROR_CLASS_CONFIG] = {"CONFIG", "调用参数非法"},
    [EMASTER_ERROR_CLASS_BUS_INTERFACE] = {"BUS_INTERFACE", "网卡/套接字不可用"},
    [EMASTER_ERROR_CLASS_BUS_NO_SLAVES] = {"BUS_NO_SLAVES", "总线上没有从站"},
    [EMASTER_ERROR_CLASS_BUS_TOPOLOGY] = {"BUS_TOPOLOGY", "拓扑或身份不符"},
    [EMASTER_ERROR_CLASS_BUS_CONFIG] = {"BUS_CONFIG", "从站配置失败（PDO/映像/SDO）"},
    [EMASTER_ERROR_CLASS_BUS_STARTUP_STATE] = {"BUS_STARTUP_STATE", "从站没进到目标状态"},
    [EMASTER_ERROR_CLASS_BUS_DC_CONFIG] = {"BUS_DC_CONFIG", "DC/SYNC0 配置失败"},
    [EMASTER_ERROR_CLASS_BUS_DC_SYNC] = {"BUS_DC_SYNC", "运行期 DC 同步失败"},
    [EMASTER_ERROR_CLASS_BUS_CYCLE_TIMING] = {"BUS_CYCLE_TIMING", "周期等待失败或截止错失"},
    [EMASTER_ERROR_CLASS_BUS_RING_TAIL_LOST] = {"BUS_RING_TAIL_LOST", "环尾丢失/物理层：整帧缺失且轴不应答"},
    [EMASTER_ERROR_CLASS_BUS_LINK_DROPPED] = {"BUS_LINK_DROPPED", "整帧缺失（各站仍应答）"},
    [EMASTER_ERROR_CLASS_BUS_SYNCHRONIZATION] = {"BUS_SYNCHRONIZATION", "同步错误：轴报同步类 AL 码或 SYNC0 连续迟到"},
    [EMASTER_ERROR_CLASS_BUS_SHORT_FRAME] = {"BUS_SHORT_FRAME", "短帧：计数变短且无同步/应答证据"},
    [EMASTER_ERROR_CLASS_DRIVE_FAULT] = {"DRIVE_FAULT", "驱动器自报故障"},
    [EMASTER_ERROR_CLASS_DRIVE_FEEDBACK] = {"DRIVE_FEEDBACK", "反馈或控制器失败"},
    [EMASTER_ERROR_CLASS_DRIVE_LIMIT] = {"DRIVE_LIMIT", "驱动器内部限位或限幅生效"},
    [EMASTER_ERROR_CLASS_MOTION] = {"MOTION", "运动层：目标非法或跟随误差"},
    [EMASTER_ERROR_CLASS_SHUTDOWN] = {"SHUTDOWN", "停机阶段失败"},
    [EMASTER_ERROR_CLASS_AUDIT] = {"AUDIT", "审计层失败"},
    [EMASTER_ERROR_CLASS_INTERNAL] = {"INTERNAL", "内部失败（内存等）"},
};

/*
 * 完整性由编译期兜：表长必须等于枚举值个数。新增一个类别而不给它键/文本，
 * 这里就编译不过——和 audit 侧那个 -Wswitch 是同一个思路：穷尽性不靠自觉。
 * 前提是枚举连续且 EMASTER_ERROR_CLASS_INTERNAL 是最后一个（新增值加在它之前）。
 */
_Static_assert(sizeof(CLASS_TABLE) / sizeof(CLASS_TABLE[0]) == (size_t)EMASTER_ERROR_CLASS_INTERNAL + 1U,
               "类别表与 emaster_error_class_t 对不上：新增类别后要同步补键与文本");

static const error_class_entry_t UNKNOWN_ENTRY = {"UNCLASSIFIED", "未分类（越界值）"};

static const error_class_entry_t *entry_of(emaster_error_class_t id)
{
    const size_t count = sizeof(CLASS_TABLE) / sizeof(CLASS_TABLE[0]);
    const size_t index = (size_t)id;
    if (index >= count)
    {
        return &UNKNOWN_ENTRY;
    }
    return &CLASS_TABLE[index];
}

const char *emaster_error_class_key(emaster_error_class_t id)
{
    return entry_of(id)->key;
}

const char *emaster_error_class_text(emaster_error_class_t id)
{
    return entry_of(id)->text;
}

emaster_error_class_t emaster_error_class_derive(const emaster_error_class_facts_t *facts,
                                                emaster_error_class_evidence_t *evidence)
{
    unsigned silent = 0U;
    unsigned sync = 0U;
    unsigned first_silent = 0U;
    unsigned first_sync = 0U;

    if (facts != NULL && facts->axes != NULL)
    {
        for (unsigned index = 0U; index < facts->axis_count; ++index)
        {
            const emaster_error_axis_facts_t *axis = &facts->axes[index];
            /* 没读到的轴不参与判定：它的 state/status_code 是 0，而 0 又恰好是
             * "不应答"的编码，采信它等于把"没读到"判成"不应答"。 */
            if (!axis->al_read)
            {
                continue;
            }
            if ((axis->state & EMASTER_AL_STATE_MASK) == 0U)
            {
                ++silent;
                if (first_silent == 0U)
                {
                    first_silent = index + 1U;
                }
            }
            if (emaster_al_status_is_synchronization(axis->status_code))
            {
                ++sync;
                if (first_sync == 0U)
                {
                    first_sync = index + 1U;
                }
            }
        }
    }

    const bool no_frame = facts != NULL && facts->wkc_no_frame_count > 0U;
    /*
     * "连续迟到"要至少两拍才算数。取 1 会把单次迟到也算成同步证据，而单次迟到
     * 是常态（session_control.c 里记着：8 臂一轮里 sync0_late_count 到过 84 次而
     * 一轮都没掉出）。这个下限是保守的占位值——等实测的 sync0_late_max_consecutive
     * 攒够，按那次定阈值的工作会把它一并收编（同一段注释里写明了那个计划）。
     */
    const bool sync_late = facts != NULL && facts->sync0_late_max_consecutive >= 2U;
    const bool counter_error =
        facts != NULL && (facts->wkc_error_count > 0U || facts->wkc_short_frame_window_max > 0U);

    /*
     * 有序规则，前一条命中即返回。顺序有意：
     *   A 压过"整帧缺失"，因为"有站彻底不说话"比"帧没回来"更具体；
     *   "整帧缺失"压过"同步"，因为帧根本没回来时，这一拍的同步错多半只是它的后果
     *   （SYNC0 会因此漂）；把因果颠倒会把 A 类误判成 B 类，而这两类的排查方向相反。
     */
    /*
     * "环尾"要求不应答的是**一部分**轴：整条环都不答话时把类判成"环尾"是过度断言
     * ——那更像整条链路掉了，报告里也没有任何尾部信息支持"环尾"这个说法。
     * axis_count 为 0 时 silent 不可能 > 0，走不到这里。
     */
    const bool tail_subset = silent > 0U && silent < facts->axis_count;

    emaster_error_class_t result;
    if (no_frame && tail_subset)
    {
        result = EMASTER_ERROR_CLASS_BUS_RING_TAIL_LOST;   /* ★A */
    }
    else if (no_frame)
    {
        result = EMASTER_ERROR_CLASS_BUS_LINK_DROPPED;
    }
    else if (sync > 0U || sync_late)
    {
        result = EMASTER_ERROR_CLASS_BUS_SYNCHRONIZATION;  /* ★B */
    }
    else if (counter_error)
    {
        result = EMASTER_ERROR_CLASS_BUS_SHORT_FRAME;
    }
    else
    {
        result = EMASTER_ERROR_CLASS_UNCLASSIFIED;
    }

    if (evidence != NULL)
    {
        /* 两项证据独立于命中了哪条规则地填写：一次"整帧缺失 + 同步码"的混合事故
         * 会落成 LINK_DROPPED，但 evidence_sync 照样为真——"同类别但证据不同"
         * 正是要能分开看的东西。 */
        evidence->evidence_no_frame = no_frame;
        evidence->evidence_sync = (sync > 0U) || sync_late;
        evidence->silent_axis_count = silent;
        evidence->sync_axis_count = sync;
        evidence->first_silent_axis = first_silent;
        evidence->first_sync_axis = first_sync;
    }

    return result;
}
