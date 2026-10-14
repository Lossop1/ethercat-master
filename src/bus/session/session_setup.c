#include "session_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emaster/motion/relative_position.h"

/*
 * 外部目标偏移上限的内置默认值：90° 输出轴（负载侧）。
 *
 * 这个数不是限幅、不是速度限制，是一道"明显不合理"的闸：它的作用是拦住符号翻转、
 * 未初始化值、量纲错位（把弧度当计数、把电机侧当负载侧）这类错误。真按它工作会让
 * 主站变成限幅器，那不是它的活。90° 取的是台架往返用的 30° 的三倍——正常跟随永远
 * 够不着，而任何量纲级别的错误都必然越过它。
 *
 * 部署键 external_target_max_offset_millidegrees 可以改它；配 0 即用这个默认值。
 */
#define EMASTER_EXTERNAL_TARGET_MAX_OFFSET_DEFAULT_MILLIDEGREES UINT32_C(90000)

/*
 * 按配置声明的精确类型写入并立即读回，避免对象宽度由调用点猜测。
 * 返回成功只表示邮箱事务成功且读回值与配置值一致。
 */
static bool write_and_verify_configured_sdo(emaster_soem_sdo_reader_context_t *sdo,
                                            const emaster_sdo_write_config_t *command) {
    uint8_t u8_value;
    int8_t i8_value;
    uint16_t u16_value;
    int16_t i16_value;
    uint32_t u32_value;
    int32_t i32_value;

    if (sdo == NULL || command == NULL) {
        return false;
    }
    switch (command->type) {
    case EMASTER_CONFIG_SDO_VALUE_U8:
        u8_value = (uint8_t)command->value;
        return emaster_soem_write_u8(sdo, command->index, command->subindex, u8_value) &&
               emaster_soem_read_u8(sdo, command->index, command->subindex, &u8_value) &&
               u8_value == (uint8_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_I8:
        i8_value = (int8_t)command->value;
        return emaster_soem_write_i8(sdo, command->index, command->subindex, i8_value) &&
               emaster_soem_read_i8(sdo, command->index, command->subindex, &i8_value) &&
               i8_value == (int8_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_U16:
        u16_value = (uint16_t)command->value;
        return emaster_soem_write_u16(sdo, command->index, command->subindex, u16_value) &&
               emaster_soem_read_u16(sdo, command->index, command->subindex, &u16_value) &&
               u16_value == (uint16_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_I16:
        i16_value = (int16_t)command->value;
        return emaster_soem_write_i16(sdo, command->index, command->subindex, i16_value) &&
               emaster_soem_read_i16(sdo, command->index, command->subindex, &i16_value) &&
               i16_value == (int16_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_U32:
        u32_value = (uint32_t)command->value;
        return emaster_soem_write_u32(sdo, command->index, command->subindex, u32_value) &&
               emaster_soem_read_u32(sdo, command->index, command->subindex, &u32_value) &&
               u32_value == (uint32_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_I32:
        i32_value = (int32_t)command->value;
        return emaster_soem_write_i32(sdo, command->index, command->subindex, i32_value) &&
               emaster_soem_read_i32(sdo, command->index, command->subindex, &i32_value) &&
               i32_value == (int32_t)command->value;
    }
    return false;
}

/*
 * 同步管理（1C32/1C33）的同步类型：方案值写不进去，就试设备收得下的档。
 *
 * 方案里那个值是台架实测的，收不收由设备决定。写一个它不认的档，设备回的是
 * 0x06090030「值超出范围」而不是"不支持"，照着配置硬写就只剩失败这一条路。
 *
 * 本来想问设备"支持哪些档"，但位图子索引（:03）不是每台设备都有 —— MT 台架实测读
 * 它回 0x06010000（不支持访问对象）。所以改成直接试写：先方案值，再按"主站怎么驱动
 * 就该怎么同步"的次序试 DC SYNC0、SM 同步、自由运行。收下的那一档读回来核对，
 * 核对过了才算数，返回的就是设备实际停的那一档。
 *
 * 良质的方案值第一下就被收下，与从前一次写、一次读回完全一样。
 */
static bool place_sync_type(emaster_soem_sdo_reader_context_t *sdo, uint16_t index,
                            uint16_t configured, uint16_t *used, bool *substituted) {
    static const uint16_t candidates[] = {UINT16_C(0x0002), UINT16_C(0x0001), UINT16_C(0x0000)};
    uint16_t observed = 0U;
    size_t ordinal;

    *substituted = false;
    *used = configured;
    if (emaster_soem_write_u16(sdo, index, UINT8_C(0x01), configured) &&
        emaster_soem_read_u16(sdo, index, UINT8_C(0x01), &observed) && observed == configured) {
        return true;
    }
    for (ordinal = 0U; ordinal < sizeof(candidates) / sizeof(candidates[0]); ++ordinal) {
        if (candidates[ordinal] == configured) {
            continue;
        }
        if (emaster_soem_write_u16(sdo, index, UINT8_C(0x01), candidates[ordinal]) &&
            emaster_soem_read_u16(sdo, index, UINT8_C(0x01), &observed) &&
            observed == candidates[ordinal]) {
            *used = candidates[ordinal];
            *substituted = true;
            return true;
        }
    }
    return false;
}

emaster_control_session_status_t emaster_soem_session_configure(emaster_soem_session_t *session) {
    size_t axis_index;
    size_t io_map_capacity;
    int io_map_size;
    int slave_count;
    emaster_control_session_status_t status;
    emaster_discovered_slave_t *discovered = NULL;
    bool mapping_succeeded = false;

    memset(&session->context, 0, sizeof(session->context));
    if (!emaster_soem_interface_carrier(session->plan->deployment->ethercat_interface)) {
        status = EMASTER_CONTROL_SESSION_INTERFACE_NOT_READY;
        return status;
    }
    if (!ecx_init(&session->context, session->plan->deployment->ethercat_interface)) {
        status = EMASTER_CONTROL_SESSION_INTERFACE_OPEN_FAILED;
        return status;
    }
    session->context_open = true;
    slave_count = ecx_config_init(&session->context);
    /* 先落盘再判数：报告里"环上几台"只有这一个来源，失败路径也要带着它 */
    session->bus_slave_count = slave_count;
    if (slave_count <= 0) {
        status = EMASTER_CONTROL_SESSION_NO_SLAVES;
        return status;
    }
    /*
     * 拓扑验证与映射：
     * 1. 扫描总线，发现所有从站
     * 2. 根据身份匹配，将配置轴映射到实际从站
     * 3. 允许总线上有未配置的从站（只警告，不失败）
     *
     * 历史问题：硬性要求 slave_count == axis_count 导致：
     * - 配置2轴但总线只有1个从站时失败（2024-09-10故障）
     * - 无法容忍调试时临时断开的从站
     */
    if ((size_t)slave_count < session->plan->axis_count) {
        status = EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;
        return status;
    }

    /* 分配发现表 */
    discovered = (emaster_discovered_slave_t *)calloc((size_t)slave_count, sizeof(emaster_discovered_slave_t));
    if (discovered == NULL) {
        status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
        return status;
    }

    /* 执行拓扑映射 */
    mapping_succeeded = emaster_soem_session_map_topology(session, slave_count, discovered);

    free(discovered);

    if (!mapping_succeeded) {
        status = EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;
        return status;
    }
    if (!emaster_soem_wait_preop(&session->context)) {
        status = EMASTER_CONTROL_SESSION_PREOP_NOT_REACHED;
        return status;
    }
    ecx_readstate(&session->context);

    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];
        uint16_t bus_position;
        const ec_slavet *slave;
        emaster_session_layout_status_t layout_status;
        uint16_t sm2_chosen;
        uint16_t sm3_chosen;
        bool sm2_substituted;
        bool sm3_substituted;
        int8_t mode_value;
        emaster_soem_sdo_reader_context_t sdo;

        /* 使用映射后的总线位置，而不是假设 axis_index + 1 */
        bus_position = session->axes[axis_index].position;
        slave = &session->context.slavelist[bus_position];

        emaster_soem_sdo_context_init(&sdo, &session->context, bus_position,
                                      &session->report->audit,
                                      EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, session->exchange);
        /*
         * **先把驱动器落到已知状态，再开始配置。**
         *
         * 主站在此之前已经发过 INIT->PRE-OP 请求（ecx_config_init 里），那一刻过程数据
         * 就停了 —— 若驱动器此刻是 CiA 402 的 Operation enabled（电机使能着），它会带着
         * 使能、在"没有目标、也没有被明确禁用"的状态下，等主站走完 PRE-OP -> SAFE-OP -> OP
         * 这一整段。控制字要到 OP 才通过过程数据发出去，而 SAFE-OP 下输出不生效。
         *
         * 所以这里用邮箱补一次权威禁用，与停机路径同一个道理：
         * **能用邮箱送的命令，就不要依赖过程数据那条会断的通道。**
         *
         * 先读后写：读到的状态字留下，事后能回答"主站接手时驱动器在不在使劲"——
         * 这是操作员反复报告、而报告里长期没有的那个读数。
         */
        session->axes[axis_index].startup_statusword_read = emaster_soem_read_u16(
            &sdo, UINT16_C(0x6041), UINT8_C(0), &session->axes[axis_index].startup_statusword);
        session->axes[axis_index].startup_disable_written = emaster_soem_write_u16(
            &sdo, UINT16_C(0x6040), UINT8_C(0), UINT16_C(0x0000));
        session->axes[axis_index].startup_statusword_after_read = emaster_soem_read_u16(
            &sdo, UINT16_C(0x6041), UINT8_C(0),
            &session->axes[axis_index].startup_statusword_after);
        emaster_cyclic_timing_stats_init(&session->axes[axis_index].timing);
        session->axes[axis_index].requested_mode = axis->operation_mode->value;
        session->axes[axis_index].actual_vendor_id = slave->eep_man;
        session->axes[axis_index].actual_product_code = slave->eep_id;
        session->axes[axis_index].actual_revision = slave->eep_rev;
        session->axes[axis_index].identity_match = emaster_slave_identity_matches(
            axis->device_profile,
            &(emaster_slave_identity_t){slave->eep_man, slave->eep_id, slave->eep_rev});
        if (!session->axes[axis_index].identity_match) {
            status = EMASTER_CONTROL_SESSION_IDENTITY_MISMATCH;
            return status;
        }
        if (!emaster_soem_discover_pdo_layout_recorded(&sdo, &session->images[axis_index].layout)) {
            status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
            return status;
        }
        layout_status =
            emaster_session_axis_validate_layout(axis, &session->images[axis_index].layout);
        if (layout_status == EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED &&
            axis->device_profile->supports_pdo_assignment) {
            emaster_soem_pdo_assignment_result_t assignment_result;

            emaster_pdo_layout_destroy(&session->images[axis_index].layout);
            if (!emaster_soem_assign_pdo_set_recorded(&sdo, axis->pdo_set, &assignment_result)) {
                session->axes[axis_index].pdo_assignment_failed_index =
                    assignment_result.failed_index;
                session->axes[axis_index].pdo_assignment_failed_subindex =
                    assignment_result.failed_subindex;
                session->axes[axis_index].pdo_assignment_abort_code_available =
                    assignment_result.abort_code_available;
                session->axes[axis_index].pdo_assignment_abort_code = assignment_result.abort_code;
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                return status;
            }
            if (!emaster_soem_discover_pdo_layout_recorded(&sdo,
                                                           &session->images[axis_index].layout)) {
                status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
                return status;
            }
            layout_status =
                emaster_session_axis_validate_layout(axis, &session->images[axis_index].layout);
        }
        /*
         * 分配选对了、映射表**条目**还对不上：设备声明的 PdoConfig 就是为这一步准备的。
         * 走完上一段之后仍然不匹配，说明差别不在"用哪张表"而在"表里是什么"—— 厂商 ESI
         * 与设备出厂默认的表可以逐条不同（MT 台架实测：ESI 第 7 条是假字节 0x2FFD，
         * 设备映射 0x5FFE）。把条目按方案写下去，再重新发现一次。
         */
        if (layout_status == EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED &&
            axis->device_profile->supports_pdo_configuration) {
            emaster_soem_pdo_assignment_result_t configuration_result;

            emaster_pdo_layout_destroy(&session->images[axis_index].layout);
            if (!emaster_soem_configure_pdo_set_recorded(&sdo, axis->pdo_set,
                                                         &configuration_result)) {
                session->axes[axis_index].pdo_assignment_failed_index =
                    configuration_result.failed_index;
                session->axes[axis_index].pdo_assignment_failed_subindex =
                    configuration_result.failed_subindex;
                session->axes[axis_index].pdo_assignment_abort_code_available =
                    configuration_result.abort_code_available;
                session->axes[axis_index].pdo_assignment_abort_code =
                    configuration_result.abort_code;
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                return status;
            }
            if (!emaster_soem_discover_pdo_layout_recorded(&sdo,
                                                           &session->images[axis_index].layout)) {
                status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
                return status;
            }
            layout_status =
                emaster_session_axis_validate_layout(axis, &session->images[axis_index].layout);
        }
        /*
         * 来到这里的第三种结果：逐条不同，但设备实际映射能用 —— 方向总长一致，运行方案
         * 依赖的字段都在、位宽也对。按设备实际建映像，一个 SDO 都不写。
         *
         * 差异逐条记进报告：采用本身不拦会话，但"映像的字节来自哪张表"必须写出来，否则
         * 没人知道主站认了什么。
         */
        if (layout_status == EMASTER_SESSION_LAYOUT_DEVICE_ADOPTED) {
            session->axes[axis_index].pdo_device_mapping_adopted = true;
            session->axes[axis_index].pdo_device_mapping_difference_count = (uint32_t)
                emaster_slave_pdo_layout_differences(
                    axis->pdo_set, &session->images[axis_index].layout,
                    session->axes[axis_index].pdo_device_mapping_differences,
                    EMASTER_CONTROL_SESSION_PDO_DIFFERENCE_SLOTS);
        }
        if ((layout_status != EMASTER_SESSION_LAYOUT_MATCH &&
             layout_status != EMASTER_SESSION_LAYOUT_DEVICE_ADOPTED) ||
            !emaster_cia_process_image_init(axis, &session->images[axis_index])) {
            status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
            return status;
        }
        session->axes[axis_index].pdo_match = true;

        if (!place_sync_type(&sdo, UINT16_C(0x1C32), axis->operation_profile->sm2_sync_type,
                             &sm2_chosen, &sm2_substituted) ||
            !place_sync_type(&sdo, UINT16_C(0x1C33), axis->operation_profile->sm3_sync_type,
                             &sm3_chosen, &sm3_substituted) ||
            (!axis->pdo_set->mode_init_on_safeop_to_op &&
             !emaster_soem_write_i8(&sdo, UINT16_C(0x6060), UINT8_C(0x00),
                                    axis->operation_mode->value))) {
            status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
            return status;
        }
        session->axes[axis_index].sm_sync_type_substituted = sm2_substituted || sm3_substituted;
        session->axes[axis_index].sm2_sync_type_used = sm2_chosen;
        session->axes[axis_index].sm3_sync_type_used = sm3_chosen;
        if (!axis->pdo_set->mode_init_on_safeop_to_op &&
            (!emaster_soem_read_i8(&sdo, UINT16_C(0x6060), UINT8_C(0x00), &mode_value) ||
             mode_value != axis->operation_mode->value)) {
            status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
            return status;
        }
        if (!axis->pdo_set->mode_init_on_safeop_to_op)
        {
            session->axes[axis_index].mode_command_sdo_read = true;
            session->axes[axis_index].mode_command_sdo = mode_value;
        }
        emaster_session_observer_read_position_scale(&sdo,
                                                     &session->axes[axis_index].position_scale);
        /*
         * 设备不实现 608F/6091 时（对象字典里根本没有这两条），设备档里声明的换算接手。
         *
         * 这是"推不出来的字段由人和台架给"那条规矩的又一处：换算的分子（编码器每转
         * 多少计数）与分母（减速比）是驱动器内部的事实，手册给不出或者不给，就只能由
         * 拿到这台机器的人写进设备档。接手之后来源记为"设备档声明"，报告里与读回来的
         * 分开写 —— 换一台设备时，哪一种来源都不该需要改代码。
         */
        if (session->axes[axis_index].position_scale.source == EMASTER_FACT_SOURCE_NONE &&
            axis->device_profile != NULL)
        {
            /* 缺项时函数返回 false 且不碰 scale：来源仍是"没有"，后面那道门会拦。 */
            (void)emaster_position_scale_from_declaration(
                axis->device_profile->encoder_counts_per_motor_revolution_default,
                axis->device_profile->gear_motor_revolutions_default,
                axis->device_profile->gear_shaft_revolutions_default,
                &session->axes[axis_index].position_scale);
        }
        /*
         * 驱动器自己的跟随误差窗与超时（6065 / 6066）。只读回，不写。
         *
         * 主站不再自己做跟随误差判据，这条保护就成了整条链上唯一在守它的东西；它是否
         * 存在、值是多少，只能靠这里读回来才可核对。读不回来只记 read=false，不拦会话。
         */
        session->axes[axis_index].following_error_window_read =
            emaster_soem_read_u32(&sdo, UINT16_C(0x6065), UINT8_C(0),
                                  &session->axes[axis_index].following_error_window);
        session->axes[axis_index].following_error_timeout_read =
            emaster_soem_read_u16(&sdo, UINT16_C(0x6066), UINT8_C(0),
                                  &session->axes[axis_index].following_error_timeout);
        if (session->plan->motion_profile != NULL ||
            session->position_target_source != NULL) {
            session->axes[axis_index].software_position_limits_read =
                emaster_soem_read_i32(&sdo, UINT16_C(0x607D), UINT8_C(1),
                                      &session->axes[axis_index].software_position_limit_min) &&
                emaster_soem_read_i32(&sdo, UINT16_C(0x607D), UINT8_C(2),
                                      &session->axes[axis_index].software_position_limit_max);
            session->axes[axis_index].polarity_read =
                emaster_soem_read_u8(&sdo, UINT16_C(0x607E), UINT8_C(0),
                                     &session->axes[axis_index].polarity);
            if (session->axes[axis_index].polarity_read) {
                session->axes[axis_index].polarity_source = EMASTER_FACT_SOURCE_DEVICE;
            } else if (axis->device_profile != NULL &&
                       axis->device_profile->polarity_default >= 0) {
                /* 设备不实现 607E：方向由设备档声明，报告里按"设备档声明"写。 */
                session->axes[axis_index].polarity =
                    (uint8_t)axis->device_profile->polarity_default;
                session->axes[axis_index].polarity_source = EMASTER_FACT_SOURCE_CONFIGURED;
            }
            if (!emaster_position_scale_is_usable(
                    &session->axes[axis_index].position_scale) ||
                !session->axes[axis_index].software_position_limits_read ||
                session->axes[axis_index].polarity_source == EMASTER_FACT_SOURCE_NONE ||
                session->axes[axis_index].software_position_limit_min >
                    session->axes[axis_index].software_position_limit_max) {
                /*
                 * 运动的前提是换算与方向都说得出来：换算来自 608F/6091 或设备档，
                 * 方向来自 607E 或设备档。三条都没有就是"这台设备的计数与角度
                 * 之间的对应关系没人知道"——此时不动，比按一个猜的系数动安全。
                 */
                status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                return status;
            }
            if (session->plan->motion_profile != NULL && axis->motion_axis == NULL) {
                status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                return status;
            }
            /*
             * 外部目标的偏移上限在这里算，因为只有这里同时握着两样东西：本轴的
             * 运行时 position_scale（上面刚读回来）和部署给的毫度值。
             *
             * 折算必须逐轴做。合同上第 8、12 席是 25:1、其余 28:1，同一角度折算
             * 出来的 counts 差 12%；早先那版把首轴的系数套给所有轴，那是假逐轴。
             *
             * 折算不出来（608F/6091 没读回来、齿轮比为零、角度乘溢出）就把上限留成
             * 0，该轴不判。报告里 offset_limit_checked 会如实标出这一点——不判和
             * "上限为零"必须分得开。
             *
             * **只有目标量是位置的轴才判。** 速度和力矩模式下这条通道收的数不是位置，
             * 拿它减实际位置得到的量没有意义，判下去的结果是每一条都被拒（目标恒在
             * 上限之外），而症状是"轴不动、脚本全绿"。量由运行方案按模式声明。
             */
            {
                uint32_t limit_millidegrees =
                    session->plan->deployment->external_target_max_offset_millidegrees;
                int64_t limit_counts = 0;
                bool positional =
                    axis->operation_mode != NULL &&
                    axis->operation_mode->quantity == EMASTER_MODE_QUANTITY_POSITION;

                if (limit_millidegrees == 0U) {
                    limit_millidegrees =
                        EMASTER_EXTERNAL_TARGET_MAX_OFFSET_DEFAULT_MILLIDEGREES;
                }
                if (positional &&
                    session->position_target_source != NULL &&
                    session->external_target_limits_counts != NULL &&
                    emaster_motion_angle_to_counts(
                        (int32_t)limit_millidegrees, EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                        &session->axes[axis_index].position_scale, &limit_counts) &&
                    limit_counts > 0) {
                    session->external_target_limits_counts[axis_index] =
                        (uint64_t)limit_counts;
                    session->axes[axis_index].offset_limit_checked = true;
                }
                session->axes[axis_index].offset_limit_counts =
                    session->external_target_limits_counts != NULL
                        ? session->external_target_limits_counts[axis_index]
                        : 0U;
            }
            if (session->plan->motion_profile == NULL)
            {
                if (session->report->audit.allocation_failed) {
                    status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
                    return status;
                }
                continue;
            }
            session->axes[axis_index].position_scale_match = emaster_position_scale_matches(
                axis->motion_axis, &session->axes[axis_index].position_scale);
            if (!session->axes[axis_index].position_scale_match) {
                status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                return status;
            }
            session->motion_scales[axis_index] = session->axes[axis_index].position_scale;
            session->motion_axis_configs[axis_index] = axis->motion_axis;
        }
        if (session->report->audit.allocation_failed) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }

    io_map_capacity = (size_t)EC_MAXIOSEGMENTS * (size_t)EC_MAXLRWDATA;
    if (io_map_capacity == 0U || io_map_capacity > SIZE_MAX / sizeof(*session->io_map)) {
        status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
        return status;
    }
    session->io_map = calloc(io_map_capacity, sizeof(*session->io_map));
    if (session->io_map == NULL) {
        status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
        return status;
    }
    /* 禁止映射函数隐式请求 SAFE-OP，状态切换由本会话在 DC 配置后显式执行。 */
    session->context.manualstatechange = 1;
    io_map_size = ecx_config_map_group(&session->context, session->io_map, 0U);
    if (io_map_size <= 0 || (size_t)io_map_size > io_map_capacity) {
        status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
        return status;
    }
    session->report->io_map_size = (size_t)io_map_size;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const ec_slavet *slave = &session->context.slavelist[
            emaster_soem_session_axis_slave(session, axis_index)];
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];

        session->axes[axis_index].process_map_match =
            slave->Obytes == axis->pdo_set->rx_pdo_bytes &&
            slave->Ibytes == axis->pdo_set->tx_pdo_bytes &&
            slave->Obits == (uint32_t)axis->pdo_set->rx_pdo_bytes * UINT32_C(8) &&
            slave->Ibits == (uint32_t)axis->pdo_set->tx_pdo_bytes * UINT32_C(8) &&
            slave->outputs != NULL && slave->inputs != NULL;
        session->axes[axis_index].output_bytes = slave->Obytes;
        session->axes[axis_index].input_bytes = slave->Ibytes;
        session->axes[axis_index].output_bits = slave->Obits;
        session->axes[axis_index].input_bits = slave->Ibits;
        if (slave->outputs != NULL) {
            session->axes[axis_index].output_offset_bytes =
                (size_t)(slave->outputs - session->io_map);
        }
        if (slave->inputs != NULL) {
            session->axes[axis_index].input_offset_bytes =
                (size_t)(slave->inputs - session->io_map);
        }
        if (!session->axes[axis_index].process_map_match) {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            return status;
        }
    }
    session->process_map_ready = true;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        if (session->plan->axes[axis_index].operation_profile->sync_strategy ==
            EMASTER_SYNC_STRATEGY_DC) {
            session->dc_required = true;
            session->axes[axis_index].dc.requested = true;
            session->axes[axis_index].dc.requested_cycle_ns = session->plan->cycle_ns;
            session->axes[axis_index].dc.requested_shift_ns =
                session->plan->axes[axis_index].operation_profile->sync0_shift_ns;
            if (session->plan->axes[axis_index].operation_profile->assign_activate <= UINT16_MAX) {
                session->axes[axis_index].dc.requested_assign_activate =
                    (uint16_t)session->plan->axes[axis_index].operation_profile->assign_activate;
            }
        }
    }
    session->report->dc_required = session->dc_required;
    if (session->dc_required) {
        uint32_t cycle_value;
        uint16_t activation;

        if (!ecx_configdc(&session->context)) {
            status = EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED;
            return status;
        }
        session->report->dc_configured = true;
        session->report->dc_reference_slave = session->context.grouplist[0].DCnext;
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            const ec_slavet *slave = &session->context.slavelist[
                emaster_soem_session_axis_slave(session, axis_index)];

            if (session->plan->axes[axis_index].operation_profile->sync_strategy !=
                EMASTER_SYNC_STRATEGY_DC) {
                continue;
            }
            session->axes[axis_index].dc.slave_capable = slave->hasdc != 0U;
            if (slave->hasdc == 0U ||
                session->plan->axes[axis_index].operation_profile->assign_activate > UINT16_MAX) {
                status = EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED;
                return status;
            }
            ecx_dcsync0(&session->context, (uint16_t)(axis_index + 1U), TRUE,
                        session->plan->cycle_ns,
                        session->plan->axes[axis_index].operation_profile->sync0_shift_ns);
            session->images[axis_index].sync0_configured = true;
            session->axes[axis_index].dc.sync0_requested = true;
            session->axes[axis_index].dc.sync0_active = slave->DCactive != 0U;
            session->axes[axis_index].dc.soem_shift_ns = slave->DCshift;
            /* 只要某一轴已经激活，后续任何失败路径都必须执行统一关闭。 */
            session->sync0_started = true;
            /* DCCUC 与 DCSYNCACT 是连续的 8 位寄存器；按 16 位读回才能核对完整
             * AssignActivate，而不是只确认 SOEM 当前默认激活的 Sync0 位。 */
            if (!emaster_session_observer_read_dc_register(
                    &session->context, (uint16_t)(axis_index + 1U), ECT_REG_DCCYCLE0, &cycle_value,
                    (uint16_t)sizeof(cycle_value), &session->report->audit,
                    EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, session->exchange) ||
                !emaster_session_observer_read_dc_register(
                    &session->context, (uint16_t)(axis_index + 1U), ECT_REG_DCCUC, &activation,
                    (uint16_t)sizeof(activation), &session->report->audit,
                    EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, session->exchange) ||
                etohl(cycle_value) != session->plan->cycle_ns ||
                etohs(activation) !=
                    (uint16_t)session->plan->axes[axis_index].operation_profile->assign_activate) {
                status = EMASTER_CONTROL_SESSION_SYNC0_CONFIG_FAILED;
                return status;
            }
            session->axes[axis_index].dc.register_read_succeeded = true;
            session->axes[axis_index].dc.observed_cycle_ns = etohl(cycle_value);
            session->axes[axis_index].dc.observed_assign_activate = etohs(activation);
        }
        if (session->report->audit.allocation_failed) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }

    session->context.slavelist[0].state = EC_STATE_SAFE_OP;
    if (ecx_writestate(&session->context, 0U) <= 0 ||
        ecx_statecheck(&session->context, 0U, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4) !=
            EC_STATE_SAFE_OP) {
        ecx_readstate(&session->context);
        status = EMASTER_CONTROL_SESSION_SAFE_OP_NOT_REACHED;
        return status;
    }
    session->report->safe_op_reached = true;
    emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_SAFE_OP,
                                   EMASTER_CONTROL_SESSION_OK);

    /* ESI 的 SO 初始化命令必须在从站已到 SAFE-OP、请求 OP 之前执行。 */
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const emaster_pdo_set_profile_t *pdo_set = session->plan->axes[axis_index].pdo_set;
        const emaster_operation_mode_t *operation_mode =
            session->plan->axes[axis_index].operation_mode;
        int8_t mode_value;
        size_t command_index;
        emaster_soem_sdo_reader_context_t sdo;

        emaster_soem_sdo_context_init(&sdo, &session->context, (uint16_t)(axis_index + 1U),
                                      &session->report->audit,
                                      EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION, session->exchange);

        if (pdo_set->mode_init_on_safeop_to_op) {
            if (!emaster_soem_write_i8(&sdo, pdo_set->mode_init_index, pdo_set->mode_init_subindex,
                                       pdo_set->mode_init_value)) {
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                return status;
            }
            if (!emaster_soem_read_i8(&sdo, pdo_set->mode_init_index, pdo_set->mode_init_subindex,
                                      &mode_value) ||
                mode_value != pdo_set->mode_init_value) {
                status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
                return status;
            }
            session->axes[axis_index].mode_command_sdo_read = true;
            session->axes[axis_index].mode_command_sdo = mode_value;
        }
        for (command_index = 0U; command_index < operation_mode->safeop_to_op_sdo_write_count;
             ++command_index) {
            const emaster_sdo_write_config_t *command =
                &operation_mode->safeop_to_op_sdo_writes[command_index];

            if (!write_and_verify_configured_sdo(&sdo, command)) {
                status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
                return status;
            }
        }
        /* 所有 SAFE-OP 初始化项生效后，再采集模式显示和同一时刻的故障对象。 */
        session->axes[axis_index].safeop_mode_display_sdo_read = emaster_soem_read_i8(
            &sdo, UINT16_C(0x6061), UINT8_C(0), &session->axes[axis_index].safeop_mode_display_sdo);
        emaster_session_observer_read_drive(
            &sdo, session->plan->axes[axis_index].device_profile,
            &session->axes[axis_index].safeop_drive_diagnostic);

        /* 读取增益参数 (2008h) - 记录控制环配置 */
        session->axes[axis_index].gain_parameters_read = true;
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(1), &session->axes[axis_index].velocity_loop_kp);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(2), &session->axes[axis_index].velocity_loop_ki);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(3), &session->axes[axis_index].velocity_loop_kd);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(4), &session->axes[axis_index].position_loop_kp);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(5), &session->axes[axis_index].position_loop_ki);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(6), &session->axes[axis_index].position_loop_kd);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(7), &session->axes[axis_index].current_loop_kp);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(8), &session->axes[axis_index].current_loop_ki);
        session->axes[axis_index].gain_parameters_read &= emaster_soem_read_u16(
            &sdo, UINT16_C(0x2008), UINT8_C(9), &session->axes[axis_index].current_loop_kd);

        if (session->axes[axis_index].gain_parameters_read) {
            fprintf(stderr, "[2008h] 轴%zu 增益参数:\n"
                   "  速度环: Kp=%u Ki=%u Kd=%u (单位 0.01)\n"
                   "  位置环: Kp=%u Ki=%u Kd=%u (单位 0.01)\n"
                   "  电流环: Kp=%u Ki=%u Kd=%u (单位 0.01)\n",
                   axis_index,
                   session->axes[axis_index].velocity_loop_kp,
                   session->axes[axis_index].velocity_loop_ki,
                   session->axes[axis_index].velocity_loop_kd,
                   session->axes[axis_index].position_loop_kp,
                   session->axes[axis_index].position_loop_ki,
                   session->axes[axis_index].position_loop_kd,
                   session->axes[axis_index].current_loop_kp,
                   session->axes[axis_index].current_loop_ki,
                   session->axes[axis_index].current_loop_kd);
        } else {
            fprintf(stderr, "[2008h] 警告：轴%zu 增益参数读取失败\n", axis_index);
        }

        if (session->report->audit.allocation_failed) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }

    return EMASTER_CONTROL_SESSION_OK;
}
