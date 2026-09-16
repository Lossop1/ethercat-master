#define _POSIX_C_SOURCE 200809L

#include "session_internal.h"

#include "emaster/bus/command_socket_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 观测通道的发布侧。
 *
 * 这个文件存在的意义是把"周期线程为了观测要多做的事"限制在一处、且只有一件事：
 * 组一帧、调一次 emaster_observation_ring_publish()。环形缓冲本身的不变式（读者
 * 不会撕裂、不会阻塞写者）由 src/bus/observation/ 负责，并由 tools/observation_selftest
 * 在无硬件条件下证明；这里只负责把会话里的值搬进帧，并如实记录搬一次要多久。
 *
 * 开关关闭时整条路径只有一次指针判空，没有时钟调用、没有组帧。
 */

/*
 * 单次发布的耗时预算是周期的百分之一：1 ms 周期即 10 µs。
 *
 * 用除数而不是写死纳秒：预算要与周期同比例伸缩，换个周期就得跟着变。报告里把算出来
 * 的 publish_budget_ns 一起写出，判据才不会随配置漂移。
 */
#define EMASTER_OBSERVATION_PUBLISH_BUDGET_DIVISOR UINT64_C(100)

static uint64_t observation_monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/*
 * 从一个可选的 TxPDO 字段取 int32 反馈。
 *
 * ordinal 为 SIZE_MAX 表示这套 PDO 映射里根本没有这个字段。这不是错误，是部署属性：
 * 固定 PDO 集合只映射 6041h/6064h，动态集合才额外带 606Ch/6077h。返回 false 时调用者
 * 置 UNAVAILABLE 标志，让"不提供"和"值为 0"在消费者那边保持可区分。
 *
 * 超出 int32 的值也按"不可用"处理而不是钳位：钳位会静默改变数值语义，而这个通道的
 * 全部价值就在于消费者能判断它拿到的东西可不可信。实时路径上不做这种仲裁。
 */
static bool observation_tx_value_i32(const emaster_cia_process_image_t *image,
                                     size_t ordinal,
                                     int32_t *out_value)
{
    int64_t value;

    if (ordinal == SIZE_MAX || ordinal >= image->tx_field_count)
    {
        return false;
    }
    if (image->tx_values[ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED)
    {
        return false;
    }
    value = image->tx_values[ordinal].value.signed_value;
    if (value < INT32_MIN || value > INT32_MAX)
    {
        return false;
    }
    *out_value = (int32_t)value;
    return true;
}

/*
 * 帧级标志。
 *
 * WKC 是否相符只有整组一个数（EtherCAT 的返回工作计数是整段数据报的，不区分从站），
 * 因此它的结论只能是整帧级的。
 */
static uint32_t observation_frame_flags(const emaster_soem_session_t *session)
{
    uint32_t flags = 0U;

    if (session->report->actual_wkc <= 0)
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_FRAME_FLAG_WHOLE_FRAME_MISSING;
    }
    else if (session->report->actual_wkc != (int32_t)session->report->expected_wkc)
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_FRAME_FLAG_WKC_MISMATCH;
    }

    /*
     * 跳拍判定：与上一**发布**帧比。截止恢复的 continue 不发帧，于是下一帧的 cycle
     * 增量为 2 而不是 1，这里如实标出来。用发布帧而不是"上一拍"作基准，是因为消费
     * 者看到的就是发布帧序列——它问"我手里这两帧之间断没断"。
     */
    if (session->observation_last_cycle_valid &&
        session->exchange > session->observation_last_cycle + UINT64_C(1))
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_FRAME_FLAG_CYCLE_GAP;
    }
    return flags;
}

/*
 * 组一个轴。
 *
 * WKC 标志在这里也跟着帧级走：工作计数短了但短在哪一无从得知，于是把整帧所有轴都
 * 标成不可信，而不是猜"大概是最后一个"。宁可多标，不可少标——策略据此拒绝动作，
 * 多标只是少用一拍数据，少标是拿可疑数据去驱动电机。
 */
static void observation_fill_axis(const emaster_soem_session_t *session,
                                  uint16_t axis_index,
                                  uint32_t frame_flags,
                                  emaster_observation_axis_t *out_axis)
{
    const emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
    const emaster_cia_process_image_t *image = &session->images[axis_index];
    uint32_t flags = 0U;

    if (!axis->input_decoded)
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_AXIS_FLAG_DECODE_INCOMPLETE;
    }
    if (axis->fault_isolated)
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_AXIS_FLAG_ISOLATED;
    }
    if ((frame_flags & (uint32_t)EMASTER_OBSERVATION_FRAME_FLAG_WKC_MISMATCH) != 0U)
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_AXIS_FLAG_WKC_MISMATCH;
    }
    if ((frame_flags & (uint32_t)EMASTER_OBSERVATION_FRAME_FLAG_WHOLE_FRAME_MISSING) != 0U)
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_AXIS_FLAG_WHOLE_FRAME_MISSING;
    }

    out_axis->actual_position = axis->actual_position;
    out_axis->target_position = axis->target_position;
    out_axis->status_word = axis->status_word;
    out_axis->control_word = axis->control_word;
    out_axis->actual_velocity = 0;
    out_axis->actual_torque = 0;
    if (!observation_tx_value_i32(image, image->tx_actual_velocity_ordinal,
                                  &out_axis->actual_velocity))
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_AXIS_FLAG_VELOCITY_UNAVAILABLE;
    }
    if (!observation_tx_value_i32(image, image->tx_actual_torque_ordinal,
                                  &out_axis->actual_torque))
    {
        flags |= (uint32_t)EMASTER_OBSERVATION_AXIS_FLAG_TORQUE_UNAVAILABLE;
    }
    out_axis->flags = flags;
}

bool emaster_soem_session_observation_open(emaster_soem_session_t *session)
{
    if (session == NULL || session->observation_ring != NULL)
    {
        return session != NULL;
    }
    /*
     * 开关默认关。第一轮迭代要能在同一条代码路径上跑"有关观测"和"无关观测"两条臂，
     * 而关掉的那条臂必须与加装之前逐字一致——所以关的时候连缓冲都不分配。
     */
    if (!emaster_soem_env_flag_enabled("EMASTER_OBSERVATION"))
    {
        return false;
    }
    session->observation_ring = calloc(1U, sizeof(*session->observation_ring));
    if (session->observation_ring == NULL)
    {
        /* 观测是旁路：分配失败只意味着没有观测，不意味着控制不能跑。 */
        return false;
    }
    emaster_observation_ring_init(session->observation_ring);
    session->report->observation.enabled = true;
    session->report->observation.ring_capacity = (uint32_t)EMASTER_OBSERVATION_RING_CAPACITY;
    session->report->observation.publish_budget_ns =
        (uint64_t)session->plan->cycle_ns / EMASTER_OBSERVATION_PUBLISH_BUDGET_DIVISOR;

    /*
     * 运输层。路径只从 emaster_observation_socket_path() 来——会话是唯一的建立点，
     * 这里再拼一次路径就会重演命令通道那两个服务器互相 unlink 的旧错。
     *
     * 建不起来不影响控制：观测缓冲照常发布，只是没有客户端能读到。
     */
    {
        char socket_path[EMASTER_SOCKET_PATH_CAPACITY];
        emaster_observation_info_t info;
        uint16_t axis_count = (uint16_t)session->plan->axis_count;

        /* 帧的轴数上限与环形缓冲一致；超出的轴在线上就不存在，这里不假装有。 */
        if (axis_count > (uint16_t)EMASTER_OBSERVATION_MAX_AXES)
        {
            axis_count = (uint16_t)EMASTER_OBSERVATION_MAX_AXES;
        }
        if (axis_count > 0U &&
            emaster_observation_socket_path(session->plan->deployment->deployment_id,
                                            socket_path, sizeof(socket_path)))
        {
            memset(&info, 0, sizeof(info));
            (void)snprintf(info.deployment_id, sizeof(info.deployment_id), "%s",
                           session->plan->deployment->deployment_id);
            (void)snprintf(info.interface_name, sizeof(info.interface_name), "%s",
                           session->plan->deployment->ethercat_interface);
            info.axis_count = axis_count;
            /*
             * 每周期一枚观测帧（cycle 每拍 +1），所以增量语义是 1。将来若引入抽帧
             * 部署，这里就是那个常量的来源；在那之前不虚构一个可配项。
             */
            info.stride = 1U;
            info.ring_capacity = (uint32_t)EMASTER_OBSERVATION_RING_CAPACITY;
            session->observation_server = emaster_observation_server_create(
                socket_path, session->observation_ring, &info);
            if (session->observation_server != NULL)
            {
                fprintf(stdout, "观测通道已启动：%s（只读，%u 轴，%u 槽）\n", socket_path,
                        (unsigned)axis_count, (unsigned)EMASTER_OBSERVATION_RING_CAPACITY);
                (void)fflush(stdout);
            }
        }
    }
    return true;
}

void emaster_soem_session_observation_close(emaster_soem_session_t *session)
{
    if (session == NULL)
    {
        return;
    }
    /*
     * 顺序是硬约束：服务器的监听线程一直读着环形缓冲，必须先把它停掉、join 掉，
     * 才能释放缓冲。反过来的话，客户端在停机前后一次 DUMP 就会读到已释放的内存。
     * 而**不**把这一步放进停机序言：那条线程是纯只读的，join 一个线程会让序言多等
     * 一个 poll 周期——正是前面花大力气消掉的那类缺口。
     */
    emaster_observation_server_destroy(session->observation_server);
    session->observation_server = NULL;
    free(session->observation_ring);
    session->observation_ring = NULL;
    session->observation_last_cycle_valid = false;
    session->observation_last_cycle = 0U;
}

void emaster_soem_session_observation_publish(emaster_soem_session_t *session,
                                              uint64_t now_ns,
                                              uint64_t deadline_ns)
{
    emaster_observation_frame_t frame;
    emaster_observation_report_t *stats;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    uint32_t flags;
    uint16_t axis_index;

    if (session == NULL || session->observation_ring == NULL)
    {
        return;
    }

    /*
     * 计时从函数入口开始，覆盖组帧与发布两者——这才是周期线程为观测多付的全部代价。
     * 只量 ring_publish() 会把 4 个轴的组帧开销藏起来，而那正是本函数新增的成本。
     */
    start_ns = observation_monotonic_ns();

    flags = observation_frame_flags(session);
    emaster_observation_frame_clear(&frame, (uint16_t)session->plan->axis_count);
    frame.cycle = session->exchange;
    frame.monotonic_ns = now_ns;
    frame.deadline_ns = deadline_ns;
    frame.frame_interval_ns = session->frame_interval_ns;
    frame.wkc = (int32_t)session->report->actual_wkc;
    frame.flags = flags;
    for (axis_index = 0U; axis_index < (uint16_t)session->plan->axis_count; ++axis_index)
    {
        observation_fill_axis(session, axis_index, flags, &frame.axes[axis_index]);
    }

    emaster_observation_ring_publish(session->observation_ring, &frame);

    session->observation_last_cycle = session->exchange;
    session->observation_last_cycle_valid = true;

    stats = &session->report->observation;
    if (stats->published_frames == 0U)
    {
        stats->first_publish_exchange = session->exchange;
    }
    if (stats->published_frames != UINT64_MAX)
    {
        ++stats->published_frames;
    }
    elapsed_ns = observation_monotonic_ns() - start_ns;
    if (elapsed_ns > stats->publish_max_ns)
    {
        stats->publish_max_ns = elapsed_ns;
        stats->publish_max_exchange = session->exchange;
    }
    if (stats->publish_budget_ns != 0U && elapsed_ns > stats->publish_budget_ns &&
        stats->publish_over_budget_count != UINT64_MAX)
    {
        ++stats->publish_over_budget_count;
    }
}
