/*
 * 接入验证：只调用 runtime_config.h / slave_profile.h 声明的公开访问函数，
 * 不看任何生成物的内部符号 —— 也就是 src/ 里那几百处调用的用法。
 * 它编得过、跑得出正确结果，就说明"数据来源从编译期数组换成运行时加载"对上层透明。
 */
#include "emaster/catalog/slave_profile.h"
#include "emaster/config/config_reader.h"
#include "emaster/config/generated_apply.h"
#include "emaster/config/generated_catalog.h"
#include "emaster/config/generated_runtime.h"
#include "emaster/config/generated_deployment.h"

#include <stdio.h>
#include <string.h>

static emaster_config_reader_t readers[4];
static int failures = 0;

static void check_str(const char *what, const char *got, const char *want)
{
    if (got == NULL || strcmp(got, want) != 0)
    {
        printf("不同  %-46s 得到 %s 期望 %s\n", what, got == NULL ? "(空)" : got, want);
        failures += 1;
        return;
    }
    printf("一致  %-46s %s\n", what, got);
}

static void check_num(const char *what, unsigned long long got, unsigned long long want)
{
    if (got != want) { printf("不同  %-46s 得到 %llu 期望 %llu\n", what, got, want); failures += 1; return; }
    printf("一致  %-46s %llu\n", what, got);
}

int main(int argc, char **argv)
{
    const char *paths[4];
    const emaster_deployment_config_t *dep;
    const emaster_slave_profile_t *profile;
    const emaster_topology_config_t *topology;
    const emaster_operation_profile_t *operation;
    const emaster_motion_profile_t *motion;
    size_t index;

    if (argc != 5) { fprintf(stderr, "用法: %s <topo> <cat> <rt> <dep>\n", argv[0]); return 2; }
    paths[0] = argv[1]; paths[1] = argv[2]; paths[2] = argv[3]; paths[3] = argv[4];
    for (index = 0U; index < 4U; ++index)
    {
        if (!emaster_config_reader_open(&readers[index], paths[index])) { fprintf(stderr, "打不开 %s\n", paths[index]); return 2; }
    }
    if (!emaster_generated_config_apply(&readers[0]) ||
        !emaster_generated_catalog_apply(&readers[1]) ||
        !emaster_generated_runtime_apply(&readers[2]) ||
        !emaster_generated_deployment_apply(&readers[3]))
    {
        fprintf(stderr, "加载失败\n");
        return 1;
    }

    /* 下面全部是 runtime_config.h 与 slave_profile.h 声明的公开接口。 */
    check_num("emaster_topology_config_count()", (unsigned long long)emaster_topology_config_count(), 13ULL);
    check_num("emaster_deployment_config_count()", (unsigned long long)emaster_deployment_config_count(), 18ULL);
    check_num("emaster_operation_profile_count()", (unsigned long long)emaster_operation_profile_count(), 6ULL);
    check_num("emaster_motion_profile_count()", (unsigned long long)emaster_motion_profile_count(), 16ULL);
    check_num("emaster_slave_profile_count()", (unsigned long long)emaster_slave_profile_count(), 1ULL);

    dep = emaster_deployment_config_by_id("orangepi-bench-duodec-30deg");
    if (dep == NULL) { printf("不同  emaster_deployment_config_by_id 返回空\n"); failures += 1; }
    else
    {
        check_str("deployment.hostname", dep->hostname, "orangepi6plus");
        check_str("deployment.ethercat_interface", dep->ethercat_interface, "enp49s0");
        check_num("deployment.realtime_cpu_core", (unsigned long long)dep->realtime_cpu_core, 11ULL);
        check_num("deployment.realtime_priority", (unsigned long long)dep->realtime_priority, 80ULL);
        check_num("deployment.topology->slave_count", (unsigned long long)dep->topology->slave_count, 12ULL);
        check_num("deployment.operation_profile_count", (unsigned long long)dep->operation_profile_count, 1ULL);
        operation = emaster_deployment_operation_profile_by_id(dep, "cyberbeast.dynamic-dc-csp-1ms");
        if (operation == NULL) { printf("不同  emaster_deployment_operation_profile_by_id 返回空\n"); failures += 1; }
        else { check_num("operation.cycle_ns", (unsigned long long)operation->cycle_ns, 1000000ULL); }
        if (dep->motion_profile == NULL) { printf("一致  deployment.motion_profile 为空（该部署不引用运动方案）\n"); }
        else { printf("一致  deployment.motion_profile = %s\n", dep->motion_profile->motion_profile_id); }
    }

    topology = emaster_topology_config_by_id("bench-duodec-slave");
    if (topology == NULL) { printf("不同  emaster_topology_config_by_id 返回空\n"); failures += 1; }
    else { check_str("topology.slaves[0].axis_id", topology->slaves[0].axis_id, "bench_axis_01"); }

    operation = emaster_operation_profile_by_id("cyberbeast.dynamic-dc-csp-1ms");
    if (operation == NULL) { printf("不同  emaster_operation_profile_by_id 返回空\n"); failures += 1; }
    else { check_str("operation.selected_mode_id", operation->selected_mode_id, "csp"); }

    motion = emaster_motion_profile_by_id("orangepi-bench.test-duodec-30deg");
    if (motion == NULL) { printf("不同  emaster_motion_profile_by_id 返回空\n"); failures += 1; }
    else
    {
        check_num("motion.axes[0].relative_angle", (unsigned long long)(int)motion->axes[0].relative_angle_millidegrees, (unsigned long long)30000);
        check_num("motion.duration_ms", (unsigned long long)motion->duration_ms, 5000ULL);
    }

    /* 设备目录：by_id 是手写的 src/catalog/slave_profile.c 提供的，这里验证它能在
       运行时加载的数据上工作（它内部调 count/at）。 */
    profile = emaster_slave_profile_by_id("cyberbeast.isvd90rc-300b-100-70.rev1");
    if (profile == NULL) { printf("不同  emaster_slave_profile_by_id 返回空\n"); failures += 1; }
    else
    {
        check_str("profile.model", profile->model, "ISVD90RC-300B-100-70");
        check_num("profile.pdo_set_count", (unsigned long long)profile->pdo_set_count, 4ULL);
        check_num("profile.slow_telemetry_count", (unsigned long long)profile->slow_telemetry_count, 12ULL);
        check_num("profile.identity.vendor_id", (unsigned long long)profile->identity.vendor_id, 0x000C0B00ULL);
        if (emaster_slave_reference_pdo_set(profile) == NULL) { printf("不同  emaster_slave_reference_pdo_set 返回空\n"); failures += 1; }
        else { check_str("reference_pdo_set", emaster_slave_reference_pdo_set(profile)->pdo_set_id, "dynamic_csp_csv_cst"); }
    }

    printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
