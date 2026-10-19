#ifndef EMASTER_CLI_OPTION_MATCH_H
#define EMASTER_CLI_OPTION_MATCH_H

/*
 * 带值长选项的两种写法识别。
 *
 * 存在原因：POSIX/GNU 约定 `--name=<值>` 与 `--name <值>` 等价，而先前每处调用点
 * 各写一遍 `strcmp(argv[i], "--name") == 0`，只认空格写法。于是同一个选项在
 * `--config-dir=` 下静默变成"不认识"，落进用法错误——命令行上两种写法看着一样，
 * 行为却不同。
 *
 * 判定分开三态，而不是用 NULL 兼表"不匹配"和"缺值"：这两件事的处置不同，前者继续
 * 往后看，后者要立刻报用法错误。
 */

#include <stdbool.h>
#include <string.h>

typedef enum
{
    EMASTER_CLI_OPTION_NONE = 0, /* 不是这个选项 */
    EMASTER_CLI_OPTION_VALUE,    /* 命中，值在 *value */
    EMASTER_CLI_OPTION_MISSING   /* 命中，但缺值或值为空 */
} emaster_cli_option_result_t;

/*
 * argument 是否为 name 选项，next 为紧随其后的参数（可为 NULL）。
 *
 * 等号写法（--name=值）不消费 next；空格写法消费，命中且有值时置 *consumed_next。
 * 空格写法沿用旧判据：下一个参数为空、以 '-' 开头，都算缺值——`--socket --read`
 * 里的 `--read` 是另一个选项，不是套接字路径。
 */
static inline emaster_cli_option_result_t emaster_cli_option_match(const char *argument,
                                                                   const char *name,
                                                                   const char *next,
                                                                   const char **value,
                                                                   bool *consumed_next)
{
    if (value != NULL)
    {
        *value = NULL;
    }
    if (consumed_next != NULL)
    {
        *consumed_next = false;
    }
    if (argument == NULL || name == NULL)
    {
        return EMASTER_CLI_OPTION_NONE;
    }

    const size_t name_length = strlen(name);

    if (strcmp(argument, name) == 0)
    {
        if (next == NULL || next[0] == '\0' || next[0] == '-')
        {
            return EMASTER_CLI_OPTION_MISSING;
        }
        if (value != NULL)
        {
            *value = next;
        }
        if (consumed_next != NULL)
        {
            *consumed_next = true;
        }
        return EMASTER_CLI_OPTION_VALUE;
    }

    if (strncmp(argument, name, name_length) == 0 && argument[name_length] == '=')
    {
        const char *inline_value = argument + name_length + 1U;

        if (inline_value[0] == '\0')
        {
            return EMASTER_CLI_OPTION_MISSING;
        }
        if (value != NULL)
        {
            *value = inline_value;
        }
        return EMASTER_CLI_OPTION_VALUE;
    }

    return EMASTER_CLI_OPTION_NONE;
}

/*
 * 摘掉已知选项之后，剩下的参数里若还有以 '-' 开头的，那是没被认出来的选项。
 *
 * 存在原因：各工具摘完 --socket/--deployment/--config-dir 就把剩下的当成位置参数。
 * 于是 `emaster-watch --socket X --bogus` 会把 `--bogus` 当成刷新间隔去解析，
 * `emaster-move --socket X --bogus` 干脆忽略它照常跑。打错一个选项，行为照旧改变而
 * 一声不吭——要么在这里报用法错误，要么操作员永远不知道自己的参数没生效。
 *
 * 返回第一个这样的参数，没有则返回 NULL。调用方据此报用法错误并退出 2。
 *
 * 注意：`--` 已经被共用解析器吃掉，所以 `--` 之后的、以 '-' 开头的操作数也会被当成
 * 未知选项。这些工具的合法操作数是数字与网卡名，不以此开头。
 */
static inline const char *emaster_cli_first_unknown_option(int argc, char **argv)
{
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] != NULL && argv[index][0] == '-')
        {
            return argv[index];
        }
    }
    return NULL;
}

#endif /* EMASTER_CLI_OPTION_MATCH_H */
