/* 部署回合测试：先加载另外三族（部署按 ID 查它们的指针），再加载部署并 dump。 */
#include "emaster/config/config_reader.h"
#include "emaster/config/generated_apply.h"
#include "emaster/config/generated_catalog.h"
#include "emaster/config/generated_runtime.h"
#include "emaster/config/generated_deployment.h"

#include <stdio.h>

/*
 * 四个读取器全部常驻，直到进程结束都不关：
 * 填进结构体的字符串指向读取器那块缓冲，关掉就等于把配置里的每个字符串变成
 * 悬垂指针。第一版这里每族加载完就 close，运行结果是"加载失败"——而真正的原因
 * 不是查找不到 ID，是字符串已经被释放。
 */
static emaster_config_reader_t readers[4];

static bool load_all(const char *topo, const char *cat, const char *rt, const char *dep)
{
    const char *paths[4];
    size_t index;

    paths[0] = topo; paths[1] = cat; paths[2] = rt; paths[3] = dep;
    for (index = 0U; index < 4U; ++index)
    {
        if (!emaster_config_reader_open(&readers[index], paths[index]))
        {
            fprintf(stderr, "打不开 %s\n", paths[index]);
            return false;
        }
    }
    if (!emaster_generated_config_apply(&readers[0])) { fprintf(stderr, "拓扑族加载失败\n"); return false; }
    if (!emaster_generated_catalog_apply(&readers[1])) { fprintf(stderr, "设备目录族加载失败\n"); return false; }
    if (!emaster_generated_runtime_apply(&readers[2])) { fprintf(stderr, "运行/运动族加载失败\n"); return false; }
    if (!emaster_generated_deployment_apply(&readers[3])) { fprintf(stderr, "部署族加载失败\n"); return false; }
    return true;
}

int main(int argc, char **argv)
{
    size_t di, ii;

    if (argc != 5) { fprintf(stderr, "用法: %s <topo.flat> <cat.flat> <rt.flat> <dep.flat>\n", argv[0]); return 2; }
    if (!load_all(argv[1], argv[2], argv[3], argv[4])) { fprintf(stderr, "加载失败\n"); return 1; }
    /* 部署条目数必须与文件声明的一致。部署族的指针是按每份部署声明的运行方案
       条数 calloc 的，count 读错一处就是越界或者少接一条。 */
    {
        size_t declared = 0U;
        if (!emaster_config_reader_size(&readers[3], "deployment.count", &declared) ||
            declared != emaster_deployment_config_count())
        {
            fprintf(stderr, "deployment.count 声明 %zu，装载了 %zu\n",
                    declared, emaster_deployment_config_count());
            return 1;
        }
    }

    printf("deployment.count\t%zu\n", emaster_deployment_config_count());
    for (di = 0U; di < emaster_deployment_config_count(); ++di)
    {
        const emaster_deployment_config_t *d = emaster_deployment_config_at(di);
        printf("deployment.%zu.id\t%s\n", di, d->deployment_id);
        printf("deployment.%zu.hostname\t%s\n", di, d->hostname);
        printf("deployment.%zu.ethercat_interface\t%s\n", di, d->ethercat_interface);
        printf("deployment.%zu.management_interface\t%s\n", di, d->management_interface);
        printf("deployment.%zu.run_report_path\t%s\n", di, d->run_report_path);
        printf("deployment.%zu.report_archive_keep\t%u\n", di, d->report_archive_keep);
        printf("deployment.%zu.error_recovery_policy_id\t%s\n", di, d->error_recovery_policy_id);
        printf("deployment.%zu.fault_policy\t%d\n", di, (int)d->fault_policy);
        printf("deployment.%zu.realtime_priority_present\t%s\n", di, d->has_realtime_priority ? "true" : "false");
        printf("deployment.%zu.realtime_priority\t%d\n", di, (int)d->realtime_priority);
        printf("deployment.%zu.realtime_cpu_core_present\t%s\n", di, d->has_realtime_cpu_core ? "true" : "false");
        printf("deployment.%zu.realtime_cpu_core\t%d\n", di, (int)d->realtime_cpu_core);
        printf("deployment.%zu.realtime_required\t%s\n", di, d->realtime_required ? "true" : "false");
        printf("deployment.%zu.external_target_timeout_ms\t%u\n", di, d->external_target_timeout_ms);
        printf("deployment.%zu.external_target_torque_limit_per_mille\t%u\n", di, d->external_target_torque_limit_per_mille);
        printf("deployment.%zu.topology_id\t%s\n", di, d->topology->topology_id);
        printf("deployment.%zu.operation_profile_ids.count\t%zu\n", di, d->operation_profile_count);
        for (ii = 0U; ii < d->operation_profile_count; ++ii)
        {
            printf("deployment.%zu.operation_profile_ids.%zu\t%s\n", di, ii, d->operation_profiles[ii]->profile_id);
        }
        printf("deployment.%zu.motion_profile_id_present\t%s\n", di, d->motion_profile != NULL ? "true" : "false");
        if (d->motion_profile != NULL)
        {
            printf("deployment.%zu.motion_profile_id\t%s\n", di, d->motion_profile->motion_profile_id);
        }
    }
    return 0;
}
