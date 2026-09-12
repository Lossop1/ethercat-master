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
#include <stdlib.h>
#include <string.h>

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

    /* 第2步：为每个配置轴查找匹配的从站 */
    for (axis_index = 0; axis_index < session->plan->axis_count; axis_index++) {
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];

        matched_index = find_unassigned_slave_by_identity(
            discovered,
            slave_count,
            axis->device_profile
        );

        if (matched_index < 0) {
            /* 未找到匹配从站 */
            return false;
        }

        /* 建立映射：配置轴 -> 总线位置 */
        session->axes[axis_index].position = discovered[matched_index].bus_position;
        discovered[matched_index].assigned = true;
    }

    return true;
}
