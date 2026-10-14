#ifndef EMASTER_CLI_H
#define EMASTER_CLI_H

/*
 * 可交付程序共用的两个信息开关：--help 与 --version。
 *
 * 两者都打到 stdout 并以 0 退出。理由：
 *   * 用法信息是程序要输出的东西，管道该能接走它；用法**错误**才走 stderr + 2。
 *   * 不分成两个头三个函数，是因为每个程序都要重复一遍，重复的地方就会漂开。
 *
 * 用法文本各程序自带 —— 只有它自己知道有哪些选项。这里只做识别与版本输出。
 * 命中时调用方应当立刻 return 0。
 */

#include <stdio.h>
#include <string.h>

#include <emaster/version.h>

typedef enum
{
    EMASTER_CLI_RUN = 0, /* 不是信息开关，照常运行 */
    EMASTER_CLI_HELP,    /* 要出用法 */
    EMASTER_CLI_VERSION  /* 要出版本号 */
} emaster_cli_info_t;

static inline emaster_cli_info_t emaster_cli_info(int argc, char **argv)
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
        {
            return EMASTER_CLI_HELP;
        }
        if (strcmp(argv[1], "--version") == 0)
        {
            return EMASTER_CLI_VERSION;
        }
    }
    return EMASTER_CLI_RUN;
}

static inline void emaster_cli_print_version(const char *program)
{
    printf("%s %s\n", program, EMASTER_VERSION);
}

#endif /* EMASTER_CLI_H */
