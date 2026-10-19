#ifndef EMASTER_CLI_DEPLOYMENT_FOR_HOST_H
#define EMASTER_CLI_DEPLOYMENT_FOR_HOST_H

/*
 * 按当前主机名挑部署配置。
 *
 * 存在原因：主站、emaster-target-probe、emaster-dc-prepare、emaster-fingerprint 各有一份
 * 逐字相同的实现。四份会在"没匹配上"这条路上一起沉默——调用方都写着
 * "deployment_for_current_host 已输出详细错误信息"，而原实现只在匹配到多个时才打印，
 * 匹配到零个时直接返回 NULL。于是不带参数运行这四个程序，一个字都不打印就退出 1，
 * 操作员看不出是主机名不对、还是配置目录里根本没有部署档。
 *
 * 现在三种结局都有话说：
 *   恰好一个匹配 → 返回它
 *   零个匹配     → 打印当前主机名、配置里的部署清单、下一步怎么做，返回 NULL
 *   多个匹配     → 打印候选与 --deployment 用法，返回 NULL
 */

#include "emaster/config/runtime_config.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * program 用于拼"下一步怎么做"的示例命令行。subcommand 为 NULL 时示例是
 * `<program> --deployment <ID>`，否则是 `<program> <subcommand> --deployment <ID>`
 * （emaster-dc-prepare 有子命令）。
 *
 * 找不到唯一的部署配置时返回 NULL，且已把原因写到 stderr。
 */
static inline const emaster_deployment_config_t *emaster_cli_deployment_for_host(
    const char *program,
    const char *subcommand)
{
    char hostname[256];
    const emaster_deployment_config_t *match = NULL;
    const emaster_deployment_config_t *candidates[16];
    size_t candidate_count = 0U;
    size_t index;

    if (gethostname(hostname, sizeof(hostname) - 1U) != 0)
    {
        fprintf(stderr, "错误：读不到当前主机名，无法自动匹配部署配置\n");
        fprintf(stderr, "  加 --deployment <部署ID> 指定部署\n");
        return NULL;
    }
    hostname[sizeof(hostname) - 1U] = '\0';

    for (index = 0U; index < emaster_deployment_config_count(); ++index)
    {
        const emaster_deployment_config_t *candidate = emaster_deployment_config_at(index);
        if (candidate != NULL && candidate->hostname != NULL &&
            strcmp(hostname, candidate->hostname) == 0)
        {
            if (match == NULL)
            {
                match = candidate;
            }
            if (candidate_count < sizeof(candidates) / sizeof(candidates[0]))
            {
                candidates[candidate_count++] = candidate;
            }
        }
    }

    if (candidate_count == 1U)
    {
        return match;
    }

    if (candidate_count > 1U)
    {
        fprintf(stderr, "错误：当前主机 %s 有 %zu 个匹配的部署配置：\n",
                hostname, candidate_count);
        for (index = 0U; index < candidate_count; ++index)
        {
            fprintf(stderr, "  %zu. %s", index + 1U, candidates[index]->deployment_id);
            if (candidates[index]->topology != NULL)
            {
                fprintf(stderr, " (拓扑=%s, 从站数=%zu)",
                        candidates[index]->topology->topology_id,
                        candidates[index]->topology->slave_count);
            }
            fprintf(stderr, "\n");
        }
    }
    else
    {
        fprintf(stderr, "错误：当前主机 %s 没有匹配的部署配置\n", hostname);
        fprintf(stderr, "配置里的部署配置：\n");
        if (emaster_deployment_config_count() == 0U)
        {
            fprintf(stderr, "  （一个都没有——确认 --config-dir 指向的目录里有 "
                            "deployments.flat）\n");
        }
        for (index = 0U; index < emaster_deployment_config_count(); ++index)
        {
            const emaster_deployment_config_t *cfg = emaster_deployment_config_at(index);
            if (cfg != NULL)
            {
                fprintf(stderr, "  %s", cfg->deployment_id);
                if (cfg->hostname != NULL)
                {
                    fprintf(stderr, " (主机=%s)", cfg->hostname);
                }
                fprintf(stderr, "\n");
            }
        }
    }

    fprintf(stderr, "请使用 --deployment 参数指定：\n");
    if (subcommand != NULL)
    {
        fprintf(stderr, "  %s %s --deployment <部署ID>\n", program, subcommand);
    }
    else
    {
        fprintf(stderr, "  %s --deployment <部署ID>\n", program);
    }
    return NULL;
}

#endif /* EMASTER_CLI_DEPLOYMENT_FOR_HOST_H */
