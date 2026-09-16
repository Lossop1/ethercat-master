#include "session_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * P4.3: SDO 慢速观测线程
 *
 * 架构：独立非 RT 线程，50ms 轮询周期
 * 功能：按设备配置声明的慢速遥测清单（设备配置的 slow_telemetry 字段），对每个
 *   从站逐条读取其语义属于周期遥测的供应商对象；对象号、宽度和单位都来自配置，通用代码
 *   不写死任何供应商对象号。清单缺省即不读，行为与每次读失败一致。
 * 数据存储：按条目的 semantic 写入 session->axes[].sdo_* 和 drive_diagnostic
 * 线程安全：使用 observer_mutex 保护共享数据
 *
 * 另有一个低频探针：每 OBSERVER_SYNC_PROBE_INTERVAL 轮读一次 1C32/1C33 的同步违例
 * 计数器，记录运行期"首次读到非零"的时刻。停机后那一次读回答不了驱动器是什么时候
 * 带上同步错误的（见 control_session.h 的字段说明）。
 */

/*
 * 停机序言里观测线程的收尾（pthread_join）是唯一的长阻塞，而主线程在 join 期间
 * 不发送任何过程数据。驱动器靠过程数据的持续性维持 OP：一次迭代原本是 21 次邮箱
 * 往返加 50ms 睡眠，最坏上百毫秒，足以触发驱动器的同步监督（AL status code 0x1A）
 * 让三个驱动器一起掉出 OP。2026-09-14 台架证据：停机入口三轴 AL=8 / 0x0000，
 * 安全停机前（join 之后、第一个停机帧之前）三轴 AL=20 / 0x001A。
 *
 * 因此停止标志在每次读之前检查一次，把 join 的等待上限压到单次 SDO 往返；
 * 观测读的邮箱超时也从 EC_TIMEOUTRXM（700ms）收紧到 OBSERVER_MAILBOX_TIMEOUT_US，
 * 让这个上限真正有界。正常往返约 3ms，30ms 已有约十倍裕量；诊断样本读不到就按
 * 未读到记（sdo_*_read=false），不值得拿驱动器掉出 OP 去换。
 */
#define OBSERVER_MAILBOX_TIMEOUT_US 30000

/*
 * 同步诊断探针的读取间隔（轮，约 106ms/轮，即约 5s）。不像其它样本一样每轮读：
 * 1C32/1C33 各有 4 个子索引，三轴合计每轮多 24 次邮箱往返，而现有整个轮迭代只有
 * 21 次（21 → 45 次，翻倍以上）。邮箱流量本身会加重驱动器的 SM2 事件丢失
 * （同一部署的 A/B：有流量时三轴一起掉出 OP，没有流量时只有 DC 参考从站掉出），
 * 仪表不得改变被观测的量级。摊到 50 轮以后不到 3%，5s 的分辨率也足够回答
 * "丢失是运行期攒的还是停机瞬间才出现的"。
 */
#define OBSERVER_SYNC_PROBE_INTERVAL 50U

/* 1C32（输出 SM）/1C33（输入 SM）的同步违例计数器子集，字段与 sm2/sm3_diagnostic 同义。 */
typedef struct
{
    uint16_t sm_event_missed;
    uint16_t cycle_time_too_small;
    uint16_t shift_time_too_short;
    bool sync_error;
} observer_sync_probe_t;

static uint64_t observer_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/* 进入一个相位：记住相位类别与起点。相位起点是反推"标志落下那一刻线程在做什么"的依据。 */
static void observer_enter_phase(emaster_soem_session_t *session, int phase)
{
    session->report->observer_stop.phase = phase;
    session->report->observer_stop.phase_begin_ns = observer_now_ns();
}

/*
 * 记下"观测线程第一次看见停止标志"这一刻。只记第一次：后面的检查点都在收尾路径上，
 * 再记就把相位覆盖成收尾时的相位了。
 *
 * 相位是反推出来的，不是猜的：标志时刻（主线程写）落在当前相位区间内，才把当前相位
 * 记成"标志落下时线程在做什么"；否则说明标志落在更早的相位里，而更早的相位已经被覆盖，
 * 记 UNKNOWN。这个区分是这套仪表的核心——join 的双峰只可能来自"相位不同"。
 */
static void observer_note_stop(emaster_soem_session_t *session, int site)
{
    emaster_observer_stop_trace_t *trace = &session->report->observer_stop;

    if (trace->stop_seen_ns != 0U)
    {
        return;
    }
    trace->stop_seen_ns = observer_now_ns();
    trace->stop_site = site;
    trace->stop_axis = trace->current_axis;
    trace->stop_iteration = trace->iteration_count;
    trace->stop_phase = EMASTER_OBSERVER_PHASE_UNKNOWN;
    trace->stop_phase_begin_ns = 0U;
    if (trace->stop_flag_ns != 0U && trace->stop_flag_ns >= trace->phase_begin_ns &&
        trace->phase_begin_ns != 0U)
    {
        trace->stop_phase = trace->phase;
        trace->stop_phase_begin_ns = trace->phase_begin_ns;
    }
}

static int observer_sdo_read(emaster_soem_session_t *session, uint16_t slave, uint16_t index,
                             uint8_t subindex, int *size, void *value)
{
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t duration_ns;
    int wkc;

    if (!session->observer_running)
    {
        /* 在读入口发现：标志落在两次读之间（上一次读返回后没有被立刻看到）。 */
        observer_note_stop(session, EMASTER_OBSERVER_STOP_SITE_BETWEEN);
        return 0; /* 与读失败同义：调用者只按 wkc > 0 判断，不会写入样本。 */
    }
    observer_enter_phase(session, EMASTER_OBSERVER_PHASE_READ);
    start_ns = observer_now_ns();
    wkc = ecx_SDOread(&session->context, slave, index, subindex, FALSE, size, value,
                      OBSERVER_MAILBOX_TIMEOUT_US);
    end_ns = observer_now_ns();
    duration_ns = end_ns > start_ns ? end_ns - start_ns : 0U;
    if (session->report->observer_stop.read_count == 0U ||
        duration_ns > session->report->observer_stop.read_max_ns)
    {
        session->report->observer_stop.read_max_ns = duration_ns;
    }
    session->report->observer_stop.read_count++;
    session->report->observer_stop.read_last_ns = duration_ns;
    if (!session->observer_running)
    {
        /*
         * 读返回时才发现：标志是在这次邮箱往返期间落下的。这一条与"读入口发现"必须
         * 分开——等待上限从"下一个检查点"变成"这次往返剩下的时间"，这正是 join 的
         * 双峰里那个更长的一峰。
         */
        observer_note_stop(session, EMASTER_OBSERVER_STOP_SITE_MAILBOX);
    }
    return wkc;
}

/* 观测线程缓存本轮遥测读数的上界；超出上界的条目按"本轮未读"处理，不改变其它条目。 */
#define OBSERVER_TELEMETRY_CACHE_MAX 16U

/* 按配置声明的类型给出读取字节宽度；未知类型返回 0，调用者据此跳过读取。 */
static int observer_telemetry_width(emaster_telemetry_type_t type)
{
    switch (type)
    {
        case EMASTER_TELEMETRY_TYPE_U8:
        case EMASTER_TELEMETRY_TYPE_I8:
            return (int)sizeof(uint8_t);
        case EMASTER_TELEMETRY_TYPE_U16:
        case EMASTER_TELEMETRY_TYPE_I16:
            return (int)sizeof(uint16_t);
        case EMASTER_TELEMETRY_TYPE_U32:
        case EMASTER_TELEMETRY_TYPE_I32:
            return (int)sizeof(uint32_t);
    }
    return 0;
}

/*
 * 把读到的原始字节解码成带符号的 64 位中间值。此前的代码把各宽度的原生整型直接交给
 * ecx_SDOread，这里的 memcpy 保持同样的"字节按本机字节序解释成该宽度"的语义。
 */
static int64_t observer_decode_telemetry(const uint8_t *buffer, emaster_telemetry_type_t type)
{
    switch (type)
    {
        case EMASTER_TELEMETRY_TYPE_U8:
            return (int64_t)buffer[0];
        case EMASTER_TELEMETRY_TYPE_I8:
        {
            int8_t value;
            memcpy(&value, buffer, sizeof(value));
            return (int64_t)value;
        }
        case EMASTER_TELEMETRY_TYPE_U16:
        {
            uint16_t value;
            memcpy(&value, buffer, sizeof(value));
            return (int64_t)value;
        }
        case EMASTER_TELEMETRY_TYPE_I16:
        {
            int16_t value;
            memcpy(&value, buffer, sizeof(value));
            return (int64_t)value;
        }
        case EMASTER_TELEMETRY_TYPE_U32:
        {
            uint32_t value;
            memcpy(&value, buffer, sizeof(value));
            return (int64_t)value;
        }
        case EMASTER_TELEMETRY_TYPE_I32:
        {
            int32_t value;
            memcpy(&value, buffer, sizeof(value));
            return (int64_t)value;
        }
    }
    return 0;
}

/*
 * 观测线程只周期读取属于遥测的语义。扩展/伺服错误码由停机诊断
 * （emaster_session_observer_read_drive）按同一份清单读取：这里再读一遍会多出邮箱往返，
 * 而邮箱流量本身会加重驱动器的 SM2 事件丢失，仪表不得改变被观测对象的量级。
 */
static bool observer_telemetry_is_observed(emaster_telemetry_semantic_t semantic)
{
    switch (semantic)
    {
        case EMASTER_TELEMETRY_ACTUAL_CURRENT:
        case EMASTER_TELEMETRY_ERROR_CODE:
        case EMASTER_TELEMETRY_BUS_VOLTAGE:
        case EMASTER_TELEMETRY_MOSFET_TEMPERATURE:
        case EMASTER_TELEMETRY_MOTOR_TEMPERATURE:
        case EMASTER_TELEMETRY_ACTUAL_VELOCITY:
        case EMASTER_TELEMETRY_TARGET_VELOCITY:
            return true;
        case EMASTER_TELEMETRY_NONE:
        case EMASTER_TELEMETRY_EXTENDED_ERROR_CODE:
        case EMASTER_TELEMETRY_SERVO_ERROR_CODE:
            return false;
    }
    return false;
}

/*
 * 把一条遥测读数写进它对应的命名槽位。读失败只清掉该槽位的读取标志，字段保留旧值，
 * 与"这一条没读到"同义；没有对应槽位的语义不写任何字段。
 */
static void observer_store_telemetry(emaster_soem_session_t *session, size_t axis,
                                     const emaster_slow_telemetry_t *entry, int wkc,
                                     int64_t value)
{
    bool succeeded = wkc > 0;

    switch (entry->semantic)
    {
        case EMASTER_TELEMETRY_ACTUAL_CURRENT:
            session->axes[axis].sdo_current_read = succeeded;
            if (succeeded)
            {
                session->axes[axis].sdo_current_6078h = (int16_t)value;
            }
            break;
        case EMASTER_TELEMETRY_ERROR_CODE:
            if (succeeded)
            {
                uint16_t error_code = (uint16_t)value;

                session->axes[axis].drive_diagnostic.cia402_error_code = error_code;
                /* P2.6: 如果错误码非零，打印警告 */
                if (error_code != 0U && session->axes[axis].sdo_read_count < 3U)
                {
                    fprintf(stderr, "[P2.6] 警告：轴%zu 检测到 CiA402 错误码 0x%04X\n", axis,
                            (unsigned)error_code);
                }
            }
            break;
        case EMASTER_TELEMETRY_BUS_VOLTAGE:
            session->axes[axis].sdo_voltage_read = succeeded;
            if (succeeded)
            {
                session->axes[axis].sdo_voltage_6079h = (uint32_t)value;
            }
            break;
        case EMASTER_TELEMETRY_MOSFET_TEMPERATURE:
            session->axes[axis].sdo_mosfet_temp_read = succeeded;
            if (succeeded)
            {
                session->axes[axis].sdo_mosfet_temp_200b01h = (int16_t)value;
            }
            break;
        case EMASTER_TELEMETRY_MOTOR_TEMPERATURE:
            session->axes[axis].sdo_motor_temp_read = succeeded;
            if (succeeded)
            {
                session->axes[axis].sdo_motor_temp_200b02h = (int16_t)value;
            }
            break;
        case EMASTER_TELEMETRY_ACTUAL_VELOCITY:
            session->axes[axis].sdo_motor_speed_read = succeeded;
            if (succeeded)
            {
                session->axes[axis].sdo_motor_speed_200b08h = (int32_t)value;
            }
            break;
        case EMASTER_TELEMETRY_TARGET_VELOCITY:
            session->axes[axis].sdo_speed_command_read = succeeded;
            if (succeeded)
            {
                session->axes[axis].sdo_speed_command_200b09h = (int32_t)value;
            }
            break;
        case EMASTER_TELEMETRY_NONE:
        case EMASTER_TELEMETRY_EXTENDED_ERROR_CODE:
        case EMASTER_TELEMETRY_SERVO_ERROR_CODE:
            /* 没有对应槽位，或由停机诊断读取；本函数不写任何字段。 */
            break;
    }
}

/* 打印一条遥测读数的完整现场：名称、对象地址、wkc 与解码值（附配置声明的单位）。 */
static void observer_print_telemetry_entry(FILE *stream, const emaster_slow_telemetry_t *entry,
                                           int wkc, int64_t value)
{
    fprintf(stream, "  %s(0x%04X:%02X) wkc=%d val=%" PRIi64, entry->name, (unsigned)entry->index,
            (unsigned)entry->subindex, wkc, value);
    if (entry->unit != NULL && entry->unit[0] != '\0')
    {
        fprintf(stream, " %s", entry->unit);
    }
    fputc('\n', stream);
}

/* 打印一条遥测读数的紧凑形式，供单行周期日志使用。 */
static void observer_print_telemetry_value(FILE *stream, const emaster_slow_telemetry_t *entry,
                                           int64_t value)
{
    fprintf(stream, "%s=%" PRIi64, entry->name, value);
    if (entry->unit != NULL && entry->unit[0] != '\0')
    {
        fprintf(stream, " %s", entry->unit);
    }
}

/*
 * 读一个同步管理参数对象的四个子索引。任一子索引读失败即整体作废，不写入半份样本
 * （11=SM 事件丢失，12=周期时间过小，13=移位时间过短，32=同步错误位）。
 */
static bool observer_read_sync_counters(emaster_soem_session_t *session, uint16_t slave,
                                        uint16_t index, observer_sync_probe_t *probe)
{
    uint16_t sm_event_missed = 0U;
    uint16_t cycle_time_too_small = 0U;
    uint16_t shift_time_too_short = 0U;
    uint8_t sync_error = 0U;
    int size;

    size = (int)sizeof(sm_event_missed);
    if (observer_sdo_read(session, slave, index, UINT8_C(11), &size, &sm_event_missed) <= 0)
    {
        return false;
    }
    size = (int)sizeof(cycle_time_too_small);
    if (observer_sdo_read(session, slave, index, UINT8_C(12), &size, &cycle_time_too_small) <= 0)
    {
        return false;
    }
    size = (int)sizeof(shift_time_too_short);
    if (observer_sdo_read(session, slave, index, UINT8_C(13), &size, &shift_time_too_short) <= 0)
    {
        return false;
    }
    size = (int)sizeof(sync_error);
    if (observer_sdo_read(session, slave, index, UINT8_C(32), &size, &sync_error) <= 0)
    {
        return false;
    }
    probe->sm_event_missed = sm_event_missed;
    probe->cycle_time_too_small = cycle_time_too_small;
    probe->shift_time_too_short = shift_time_too_short;
    probe->sync_error = sync_error != 0U;
    return true;
}

/*
 * 记一次探针结果：第一次读到任何非零（含"周期时间过小""移位时间过短"两个方向）时留下
 * 交换号与该时刻的读数，之后只更新最后一次读数。调用者持有 observer_mutex。
 * 返回 true 表示这次就是首次非零，调用者据此打印一次日志。
 */
static bool observer_record_sync_probe(uint64_t *first_error_exchange,
                                       uint16_t *first_error_missed,
                                       bool *first_error_sync_error, uint16_t *last_missed,
                                       bool *last_sync_error, const observer_sync_probe_t *probe,
                                       uint64_t exchange)
{
    bool any_nonzero = probe->sync_error || probe->sm_event_missed != 0U ||
                       probe->cycle_time_too_small != 0U || probe->shift_time_too_short != 0U;
    bool recorded = false;

    if (*first_error_exchange == 0U && any_nonzero)
    {
        *first_error_exchange = exchange;
        *first_error_missed = probe->sm_event_missed;
        *first_error_sync_error = probe->sync_error;
        recorded = true;
    }
    *last_missed = probe->sm_event_missed;
    *last_sync_error = probe->sync_error;
    return recorded;
}

/* 50ms 睡眠同样要可中断，否则 join 还要多等一个完整睡眠周期。 */
static void observer_sleep(emaster_soem_session_t *session, const struct timespec *interval)
{
    const struct timespec slice = {0, 1000000}; /* 1ms */
    struct timespec remaining = *interval;

    while (session->observer_running && (remaining.tv_sec > 0 || remaining.tv_nsec > 0))
    {
        /* 相位逐片登记：睡眠片的起点是反推"标志落下时线程是否在睡"的依据，
         * 也说明 join 最多还要等这一个片（1ms）而不是整个 50ms。 */
        observer_enter_phase(session, EMASTER_OBSERVER_PHASE_SLEEP);
        if (nanosleep(&slice, NULL) != 0)
        {
            return;
        }
        if (remaining.tv_nsec >= slice.tv_nsec)
        {
            remaining.tv_nsec -= slice.tv_nsec;
        }
        else
        {
            if (remaining.tv_sec == 0)
            {
                return;
            }
            remaining.tv_sec -= 1;
            remaining.tv_nsec += 1000000000L - slice.tv_nsec;
        }
    }
    if (!session->observer_running)
    {
        observer_note_stop(session, EMASTER_OBSERVER_STOP_SITE_SLEEP);
    }
}

static void *observer_thread_func(void *arg)
{
    emaster_soem_session_t *session = (emaster_soem_session_t *)arg;
    struct timespec interval = {0, 50000000}; /* 50ms */
    uint64_t iteration = 0U;
    emaster_observer_stop_trace_t *trace = &session->report->observer_stop;

    fprintf(stderr, "[P4.3] SDO 观测线程已启动\n");
    fprintf(stderr, "[P4.3] session=%p\n", (void*)session);
    fprintf(stderr, "[P4.3] observer_running=%d\n", session->observer_running);

    if (session->plan == NULL)
    {
        fprintf(stderr, "[P4.3] 错误：session->plan 为 NULL，线程退出\n");
        return NULL;
    }

    fprintf(stderr, "[P4.3] axis_count=%zu\n", session->plan->axis_count);
    fflush(stderr);

    for (;;)
    {
        uint64_t iteration_start_ns;
        uint64_t iteration_ns;

        if (!session->observer_running)
        {
            /* 循环顶发现：睡眠片与读入口都没先看到它，说明标志落在两次检查之间。 */
            observer_note_stop(session, EMASTER_OBSERVER_STOP_SITE_BETWEEN);
            break;
        }
        iteration_start_ns = observer_now_ns();
        /*
         * 同步诊断探针只在驱动器已经在 OP 里之后才读：进入 OP 之前 DC 同步还没跑，
         * 1C32/1C33 的计数器没有意义，先读一次会把启动瞬态记成运行期事件。
         */
        const bool probe_sync = (iteration % OBSERVER_SYNC_PROBE_INTERVAL) == 0U &&
                                session->report->op_reached;

        for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
        {
            trace->current_axis = axis;
            /*
             * 遥测清单来自设备配置：通用代码只按声明读，不写死供应商对象号。
             * 缓存保存本轮读数，供打印与写样本两段共用；上界只约束仪表读数的规模，
             * 超出上界的条目按"本轮未读"处理，字段保持上一轮的值，与读失败同义。
             */
            const emaster_slave_profile_t *device_profile =
                session->plan->axes[axis].device_profile;
            const emaster_slow_telemetry_t *telemetry =
                device_profile == NULL ? NULL : device_profile->slow_telemetry;
            size_t telemetry_count =
                device_profile == NULL ? 0U : device_profile->slow_telemetry_count;
            int telemetry_wkc[OBSERVER_TELEMETRY_CACHE_MAX];
            int64_t telemetry_value[OBSERVER_TELEMETRY_CACHE_MAX];
            uint16_t slave_position = emaster_soem_session_axis_slave(session, axis);

            if (telemetry_count > OBSERVER_TELEMETRY_CACHE_MAX)
            {
                telemetry_count = OBSERVER_TELEMETRY_CACHE_MAX;
            }
            memset(telemetry_wkc, 0, sizeof(telemetry_wkc));
            memset(telemetry_value, 0, sizeof(telemetry_value));

            /* 按清单逐条读取；读到的原始字节按声明宽度解码。 */
            for (size_t entry_index = 0U; entry_index < telemetry_count; ++entry_index)
            {
                const emaster_slow_telemetry_t *entry = &telemetry[entry_index];
                uint8_t buffer[4] = {0U, 0U, 0U, 0U};
                int size = observer_telemetry_width(entry->type);

                if (!observer_telemetry_is_observed(entry->semantic) || size <= 0)
                {
                    continue;
                }
                telemetry_wkc[entry_index] = observer_sdo_read(
                    session, slave_position, entry->index, entry->subindex, &size, buffer);
                telemetry_value[entry_index] = observer_decode_telemetry(buffer, entry->type);
            }

            /* 前3次读取始终打印以验证通道工作 */
            if (session->axes[axis].sdo_read_count < 3U)
            {
                fprintf(stderr, "[P4.3] 轴%zu 第%lu次读取:\n", axis,
                        (unsigned long)(session->axes[axis].sdo_read_count + 1U));
                for (size_t entry_index = 0U; entry_index < telemetry_count; ++entry_index)
                {
                    if (!observer_telemetry_is_observed(telemetry[entry_index].semantic))
                    {
                        continue;
                    }
                    observer_print_telemetry_entry(stderr, &telemetry[entry_index],
                                                   telemetry_wkc[entry_index],
                                                   telemetry_value[entry_index]);
                }
            }

            /*
             * 相位：解算并写样本。每 20 次读取有一次 stderr 日志落在这里，而 stderr
             * 是无缓冲的——这一轮里最可能被 I/O 挡住的就是这一段。
             */
            observer_enter_phase(session, EMASTER_OBSERVER_PHASE_SAMPLE);
            /* 加锁写入结果 */
            pthread_mutex_lock(&session->observer_mutex);
            for (size_t entry_index = 0U; entry_index < telemetry_count; ++entry_index)
            {
                observer_store_telemetry(session, axis, &telemetry[entry_index],
                                         telemetry_wkc[entry_index],
                                         telemetry_value[entry_index]);
            }

            session->axes[axis].sdo_read_count++;

            /* 每20次读取打印一次（约1秒间隔） */
            if (session->axes[axis].sdo_read_count % 20U == 0U)
            {
                bool first_field = true;

                fprintf(stderr, "[P4.3] 轴%zu:", axis);
                for (size_t entry_index = 0U; entry_index < telemetry_count; ++entry_index)
                {
                    if (!observer_telemetry_is_observed(telemetry[entry_index].semantic))
                    {
                        continue;
                    }
                    fprintf(stderr, "%s", first_field ? " " : ", ");
                    first_field = false;
                    observer_print_telemetry_value(stderr, &telemetry[entry_index],
                                                   telemetry_value[entry_index]);
                }
                fprintf(stderr, " (读取次数=%lu)\n",
                        (unsigned long)session->axes[axis].sdo_read_count);
            }
            pthread_mutex_unlock(&session->observer_mutex);

            if (probe_sync)
            {
                observer_sync_probe_t sm2_probe;
                observer_sync_probe_t sm3_probe;
                /*
                 * session->exchange 由周期线程每毫秒推进，这里只作为诊断读数的时间坐标；
                 * 对齐的 64 位读不会读到半截数值，最多读到相邻的交换号，
                 * 差一个周期不影响"这次丢失发生在运行期还是停机瞬间"的判断。
                 */
                uint64_t exchange_now = session->exchange;
                bool first_sm2 = false;
                bool first_sm3 = false;

                memset(&sm2_probe, 0, sizeof(sm2_probe));
                memset(&sm3_probe, 0, sizeof(sm3_probe));
                bool sm2_ok = observer_read_sync_counters(session, slave_position,
                                                          UINT16_C(0x1C32), &sm2_probe);
                bool sm3_ok = observer_read_sync_counters(session, slave_position,
                                                          UINT16_C(0x1C33), &sm3_probe);
                /* 两次读数之后不是读，是算：相位从这里退回 SAMPLE，别把上一拍的读当现场。 */
                observer_enter_phase(session, EMASTER_OBSERVER_PHASE_SAMPLE);

                pthread_mutex_lock(&session->observer_mutex);
                /*
                 * 计数的是"这一轴成功读到过同步计数的轮数"（1C32/1C33 任一成功即可），
                 * 用来说明 first_error_exchange=0 确实是"全程读到非零之前都没事"，
                 * 而不是"探针一次都没读成功"。
                 */
                if (sm2_ok || sm3_ok)
                {
                    ++session->axes[axis].sm_sync_probe_count;
                }
                if (sm2_ok)
                {
                    first_sm2 = observer_record_sync_probe(
                        &session->axes[axis].sm2_first_error_exchange,
                        &session->axes[axis].sm2_first_error_missed,
                        &session->axes[axis].sm2_first_error_sync_error,
                        &session->axes[axis].sm2_last_missed,
                        &session->axes[axis].sm2_last_sync_error, &sm2_probe, exchange_now);
                }
                if (sm3_ok)
                {
                    first_sm3 = observer_record_sync_probe(
                        &session->axes[axis].sm3_first_error_exchange,
                        &session->axes[axis].sm3_first_error_missed,
                        &session->axes[axis].sm3_first_error_sync_error,
                        &session->axes[axis].sm3_last_missed,
                        &session->axes[axis].sm3_last_sync_error, &sm3_probe, exchange_now);
                }
                pthread_mutex_unlock(&session->observer_mutex);

                /* 首次非零只在日志里报一次，不必等几百 MB 的报告落地才能看到时刻。 */
                if (first_sm2)
                {
                    fprintf(stderr, "[P4.3] 轴%zu 运行期首次读到 1C32 非零：交换号=%" PRIu64
                                    " missed=%u sync_error=%d\n",
                            axis, exchange_now, (unsigned int)sm2_probe.sm_event_missed,
                            sm2_probe.sync_error ? 1 : 0);
                }
                if (first_sm3)
                {
                    fprintf(stderr, "[P4.3] 轴%zu 运行期首次读到 1C33 非零：交换号=%" PRIu64
                                    " missed=%u sync_error=%d\n",
                            axis, exchange_now, (unsigned int)sm3_probe.sm_event_missed,
                            sm3_probe.sync_error ? 1 : 0);
                }
            }
        }

        ++iteration;
        trace->iteration_count = iteration;
        {
            /* 单轮工作耗时（不含睡眠）：50 ms 周期里真正占核的部分，也是"同优先级 FIFO
             * 线程互相遮挡"时最该被看见的量。 */
            uint64_t iteration_end_ns = observer_now_ns();

            iteration_ns = iteration_end_ns > iteration_start_ns
                               ? iteration_end_ns - iteration_start_ns
                               : 0U;
            if (iteration_ns > trace->iteration_max_ns)
            {
                trace->iteration_max_ns = iteration_ns;
            }
        }
        observer_sleep(session, &interval);
    }
    trace->loop_exit_ns = observer_now_ns();

    fprintf(stderr, "[P4.3] SDO 观测线程已退出\n");
    /*
     * 收尾时刻单独记：这条 fprintf 走的是无缓冲 stderr，主线程在 join 里等它写完。
     * "看见标志 → 循环退出"与"循环退出 → 线程函数返回"两段分开，才能判断 join 的
     * 双峰是等待检查点还是被收尾的 I/O 挡住。
     */
    trace->exit_ns = observer_now_ns();
    /*
     * 线程函数里的最后一件事。放在 return 之前而不是循环出口：调用者看到的必须是
     * "这个线程已经没事可做了"，而不是"它刚决定要退"。之后真正回收仍然靠 pthread_join，
     * 这个标志只负责让停机序言不必在 join 上阻塞掉一段过程数据。
     */
    session->observer_exited = true;
    return NULL;
}

bool emaster_soem_session_start_observer(emaster_soem_session_t *session)
{
    if (pthread_mutex_init(&session->observer_mutex, NULL) != 0)
    {
        return false;
    }

    session->observer_running = true;

    if (pthread_create(&session->observer_thread, NULL, observer_thread_func, session) != 0)
    {
        session->observer_running = false;
        pthread_mutex_destroy(&session->observer_mutex);
        return false;
    }

    session->observer_thread_created = true;
    return true;
}

/*
 * 停机拆成三段（EMASTER_SHUTDOWN_INLINE_PROLOGUE 的停机序言要用）：
 *
 *   request  ── 置标志。不阻塞，调用者可以立刻回去发下一帧。
 *   exited   ── 只看一眼线程有没有跑完，不阻塞、不回收。
 *   reap     ── pthread_join + 销毁互斥锁。只有在 exited 为真之后调用才不阻塞。
 *
 * 之所以必须拆开：join 会一直等到观测线程把手上的邮箱读做完（实测最长 5.34 ms），
 * 而这段等待原先整个落在"周期已停、停机帧未发"的窗口里，经时钟栅格量化成 2～3 ms
 * 的过程数据断供，驱动器按 CiA402 同步容差直接掉出 OP。拆开后这三段可以分散到
 * 不同的周期里，中间照常发帧，等待就不再等于断供。
 *
 * stop_observer = request + reap，语义与拆分之前逐字一致。
 */
void emaster_soem_session_request_observer_stop(emaster_soem_session_t *session)
{
    if (session == NULL || !session->observer_thread_created)
    {
        return;
    }

    /*
     * 先记时刻再置标志，次序不能反：这两个时间点之间的差就是"标志落下 → 被看见"，
     * 而 join 的总耗时是双峰的（12 臂实测 ≤0.806 ms 全通过、≥0.935 ms 全失败）。
     * 不把这两段分开，就只能看到"join 长了一点"，看不出长在哪。
     */
    session->observer_exited = false;
    session->report->observer_stop.stop_flag_ns = observer_now_ns();
    session->observer_running = false;
}

bool emaster_soem_session_observer_exited(const emaster_soem_session_t *session)
{
    /* 没起来过的线程算已经退出：调用者据此直接进 next 步，不会白等。 */
    return session == NULL || !session->observer_thread_created || session->observer_exited;
}

void emaster_soem_session_reap_observer(emaster_soem_session_t *session)
{
    if (session == NULL || !session->observer_thread_created)
    {
        return;
    }

    pthread_join(session->observer_thread, NULL);
    pthread_mutex_destroy(&session->observer_mutex);
    session->observer_thread_created = false;
}

void emaster_soem_session_stop_observer(emaster_soem_session_t *session)
{
    if (session == NULL || !session->observer_thread_created)
    {
        return;
    }

    emaster_soem_session_request_observer_stop(session);
    emaster_soem_session_reap_observer(session);
}
