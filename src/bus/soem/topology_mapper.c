/*
 * 拓扑发现与映射：扫描总线上的实际从站，根据身份匹配到配置的轴。
 *
 * 存在原因：
 * - 总线拓扑是运行时真相，配置文件是声明性意图
 * - 从站位置可能跳跃（1,2,5,7）或动态变化
 * - 需要容忍未配置的从站存在于总线上
 * - 支持"期望这些设备，请帮我找"的声明式配置风格
 */

#include "session_internal.h"
#include "soem_common.h"
#include <stdlib.h>
#include <string.h>

/*
 * 序列号映射只在拓扑里至少登记了一个序列号时才启用。
 *
 * 不用序列号的配置保持原来的行为：不读邮箱、不增加启动时间。登记了序列号的
 * 配置，序列号就是轴到物理从站的键 —— 同型号从站换了环序也照样找得到。
 */
static bool plan_declares_serial_numbers(const emaster_soem_session_t *session)
{
    size_t axis_index;

    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        if (session->plan->axes[axis_index].topology_slave->serial_number != 0U) {
            return true;
        }
    }
    return false;
}

static void read_serial_numbers(emaster_soem_session_t *session,
                                emaster_discovered_slave_t *discovered,
                                int discovered_count)
{
    int i;

    for (i = 1; i <= discovered_count; ++i) {
        emaster_soem_sdo_reader_context_t sdo;

        discovered[i - 1].serial_number_read = false;
        discovered[i - 1].serial_number = 0U;
        if ((session->context.slavelist[i].mbx_proto & ECT_MBXPROT_COE) == 0U) {
            continue;
        }
        emaster_soem_sdo_context_init(
            &sdo, &session->context, (uint16_t)i,
            session->report != NULL ? &session->report->audit : NULL,
            EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, session->exchange);
        /* 读不到就等满超时才返回。给 100 ms：够一次正常邮箱往返，又不会让
         * 不支持 0x1018:04 的设备把启动拖到几十秒。 */
        emaster_soem_sdo_set_timeout(&sdo, 100000);
        discovered[i - 1].serial_number_read =
            emaster_soem_read_u32(&sdo, UINT16_C(0x1018), UINT8_C(0x04),
                                  &discovered[i - 1].serial_number);
    }
}

static int find_unassigned_slave_by_serial(
    emaster_discovered_slave_t *discovered,
    int discovered_count,
    const emaster_slave_profile_t *profile,
    uint32_t serial_number)
{
    int i;
    emaster_slave_identity_t candidate;

    if (discovered == NULL || profile == NULL) {
        return -1;
    }

    for (i = 0; i < discovered_count; i++) {
        if (discovered[i].assigned || !discovered[i].serial_number_read ||
            discovered[i].serial_number != serial_number) {
            continue;
        }
        candidate.vendor_id = discovered[i].vendor_id;
        candidate.product_code = discovered[i].product_code;
        candidate.revision = discovered[i].revision;
        if (emaster_slave_identity_matches(profile, &candidate)) {
            return i;
        }
    }
    return -1;
}

/*
 * 在未分配的从站中查找与设备profile匹配的第一个。
 * 返回从站在discovered数组中的索引，未找到返回-1。
 */
static int find_unassigned_slave_by_identity(
    emaster_discovered_slave_t *discovered,
    int discovered_count,
    const emaster_slave_profile_t *profile)
{
    int i;
    emaster_slave_identity_t candidate;

    if (discovered == NULL || profile == NULL) {
        return -1;
    }

    for (i = 0; i < discovered_count; i++) {
        if (discovered[i].assigned) {
            continue;
        }
        candidate.vendor_id = discovered[i].vendor_id;
        candidate.product_code = discovered[i].product_code;
        candidate.revision = discovered[i].revision;

        if (emaster_slave_identity_matches(profile, &candidate)) {
            return i;
        }
    }
    return -1;
}

/*
 * 扫描总线并建立配置轴到实际从站的映射。
 * 成功返回true，所有配置轴都找到匹配从站；
 * 失败返回false，session->axes[].position 可能部分填充。
 */
bool emaster_soem_session_map_topology(
    emaster_soem_session_t *session,
    int slave_count,
    emaster_discovered_slave_t *discovered)
{
    size_t axis_index;
    int matched_index;
    int i;
    ec_slavet *slave;

    /* 第1步：扫描总线，建立发现表 */
    for (i = 1; i <= slave_count; i++) {
        slave = &session->context.slavelist[i];
        discovered[i - 1].bus_position = (uint16_t)i;
        discovered[i - 1].vendor_id = slave->eep_man;
        discovered[i - 1].product_code = slave->eep_id;
        discovered[i - 1].revision = slave->eep_rev;
        discovered[i - 1].assigned = false;
    }

    /*
     * 第2步：先认序列号，再按身份兜底。
     *
     * 顺序不能反过来。按身份先挑会按环序抓走"第一个同型号从站"，把本该属于
     * 后面那根轴的从站占掉；序列号轴必须先拿到自己的那一台。
     */
    if (plan_declares_serial_numbers(session)) {
        read_serial_numbers(session, discovered, slave_count);
    }
    for (axis_index = 0; axis_index < session->plan->axis_count; axis_index++) {
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];
        uint32_t expected_serial = axis->topology_slave->serial_number;

        if (expected_serial == 0U) {
            continue;
        }
        matched_index = find_unassigned_slave_by_serial(discovered, slave_count,
                                                        axis->device_profile,
                                                        expected_serial);
        if (matched_index < 0) {
            /* 留下"这一轴核对过、没过"，控制台才能点名到轴。 */
            session->axes[axis_index].serial_number_checked = true;
            session->axes[axis_index].serial_number_match = false;
            return false;
        }
        session->axes[axis_index].position = discovered[matched_index].bus_position;
        session->axes[axis_index].serial_number_read =
            discovered[matched_index].serial_number_read;
        session->axes[axis_index].actual_serial_number =
            discovered[matched_index].serial_number;
        discovered[matched_index].assigned = true;
    }

    /* 第3步：没有登记序列号的轴，仍按身份三元组取首个未分配从站。 */
    for (axis_index = 0; axis_index < session->plan->axis_count; axis_index++) {
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];

        if (axis->topology_slave->serial_number != 0U) {
            continue;
        }
        matched_index = find_unassigned_slave_by_identity(
            discovered,
            slave_count,
            axis->device_profile
        );
        if (matched_index < 0) {
            return false;
        }
        session->axes[axis_index].position = discovered[matched_index].bus_position;
        /* 序列号是顺带读到的，写进结果便于报告回答"这条线上到底是哪几台"。 */
        session->axes[axis_index].serial_number_read =
            discovered[matched_index].serial_number_read;
        session->axes[axis_index].actual_serial_number =
            discovered[matched_index].serial_number;
        discovered[matched_index].assigned = true;
    }

    return true;
}
