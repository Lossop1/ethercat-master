#ifndef EMASTER_CONFIG_RUNTIME_CONFIG_CLI_H
#define EMASTER_CONFIG_RUNTIME_CONFIG_CLI_H

/*
 * CLI 工具共用的运行时配置目录解析。
 *
 * 存在原因：主站启动时从 --config-dir（默认 config-runtime）加载四族 flat，而诊断类
 * 工具只调用 emaster_deployment_config_by_id()/count()，从不加载，于是永远看到 0 个
 * 部署——工具本身是对的，只是没人喂它配置。
 *
 * 与 emaster_cli_resolve_socket 同一套路：把 --config-dir <目录> 从 argv 里摘掉，其余
 * 参数原地左移，调用方原有的位置参数解析不用改。
 */

#include <stdbool.h>
#include <string.h>

#include "emaster/cli/option_match.h"

#define EMASTER_CONFIG_DIR_DEFAULT "config-runtime"

/*
 * 摘掉 --config-dir <目录>，返回目录串（未给时返回默认值）。调用方随后自己调用
 * emaster_runtime_config_load()——不是所有子命令都需要配置，枚举网卡就不需要。
 *
 * 选项缺参数时返回 NULL，此时 argv 可能已被部分压缩，调用方应直接报用法错误退出。
 *
 * 带值的长选项两种写法都认：`--config-dir <目录>` 与 `--config-dir=<目录>`。`--` 结束
 * 选项，它本身被吃掉，其后的参数一律当操作数原样留下。
 */
static inline const char *emaster_cli_config_dir(int *argc, char **argv)
{
    static const char option[] = "--config-dir";
    const char *directory = NULL;
    int write_index = 1;
    bool options_ended = false;

    if (argc == NULL || argv == NULL)
    {
        return NULL;
    }

    for (int read_index = 1; read_index < *argc; ++read_index)
    {
        const char *argument = argv[read_index];
        const char *next = (read_index + 1 < *argc) ? argv[read_index + 1] : NULL;
        const char *value = NULL;
        bool consumed_next = false;
        emaster_cli_option_result_t result;

        if (options_ended || strcmp(argument, "--") == 0)
        {
            if (!options_ended)
            {
                options_ended = true;
                continue;
            }
            argv[write_index++] = argv[read_index];
            continue;
        }

        result = emaster_cli_option_match(argument, option, next, &value, &consumed_next);
        if (result == EMASTER_CLI_OPTION_MISSING)
        {
            return NULL;
        }
        if (result == EMASTER_CLI_OPTION_VALUE)
        {
            directory = value;
            if (consumed_next)
            {
                ++read_index;
            }
            continue;
        }

        argv[write_index++] = argv[read_index];
    }
    *argc = write_index;

    return directory != NULL ? directory : EMASTER_CONFIG_DIR_DEFAULT;
}

#endif /* EMASTER_CONFIG_RUNTIME_CONFIG_CLI_H */
