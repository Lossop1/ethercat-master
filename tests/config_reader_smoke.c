/*
 * 扁平文件读取器冒烟测试：读自己写的那份扁平文件，核对键名查找、类型转换、
 * 缺失语义和注释/空行处理。
 *
 * 为什么样本自己写：交付的仓库里不含任何设备配置，读器的输入由客户跑
 * gen/emaster_config.py 现生成。所以这里不问"某个型号的某个键是多少"，
 * 只问读器对**任意**一份扁平文件的行为 —— 那也是它在现场唯一需要成立的东西。
 */
#include "emaster/config/config_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

/* 样本自己带行尾空白、注释行与空行：现场生成的文件里它们都出现过。 */
static const char *FIXTURE =
    "# 注释行不算记录\n"
    "\n"
    "alpha\tone\n"
    "alpha.empty\t\n"
    "beta\t42\n"
    "gamma\t-7\n"
    "delta.flag\ttrue\n"
    "epsilon\t4294967295\n";

static int write_fixture(const char *path)
{
    FILE *handle = fopen(path, "wb");
    if (handle == NULL)
    {
        return 0;
    }
    fputs(FIXTURE, handle);
    return fclose(handle) == 0;
}

static void expect_str(const emaster_config_reader_t *r, const char *key, const char *want)
{
    const char *got = NULL;
    if (!emaster_config_reader_string(r, key, &got))
    {
        printf("缺失  %-24s 期望 %s\n", key, want);
        failures += 1;
        return;
    }
    if (strcmp(got, want) != 0)
    {
        printf("不同  %-24s 得到 %s 期望 %s\n", key, got, want);
        failures += 1;
        return;
    }
    printf("一致  %-24s %s\n", key, got);
}

static void expect_u32(const emaster_config_reader_t *r, const char *key, unsigned want)
{
    uint32_t got = 0U;
    if (!emaster_config_reader_u32(r, key, &got) || got != want)
    {
        printf("不同  %-24s 得到 %u 期望 %u\n", key, (unsigned)got, want);
        failures += 1;
        return;
    }
    printf("一致  %-24s %u\n", key, (unsigned)got);
}

static void expect_absent_string(const emaster_config_reader_t *r, const char *key)
{
    const char *got = NULL;
    if (emaster_config_reader_string(r, key, &got))
    {
        printf("不同  %-24s 不该存在却有值 %s\n", key, got);
        failures += 1;
        return;
    }
    printf("一致  %-24s 正确报告缺失\n", key);
}

int main(int argc, char **argv)
{
    emaster_config_reader_t reader;
    char path[512];

    if (argc == 2)
    {
        (void)snprintf(path, sizeof(path), "%s", argv[1]);
    }
    else if (argc == 1)
    {
        const char *tmpdir = getenv("TMPDIR");
        (void)snprintf(path, sizeof(path), "%s/emaster-config-reader-smoke.flat",
                       tmpdir != NULL ? tmpdir : ".");
    }
    else
    {
        fprintf(stderr, "用法: %s [扁平文件路径]\n", argv[0]);
        return 2;
    }

    if (!write_fixture(path))
    {
        fprintf(stderr, "写不出样本 %s\n", path);
        return 2;
    }
    if (!emaster_config_reader_open(&reader, path))
    {
        fprintf(stderr, "打不开 %s\n", path);
        return 2;
    }
    printf("读入 %zu 条记录，%zu 字节\n\n", reader.line_count, reader.size);

    expect_str(&reader, "alpha", "one");
    expect_u32(&reader, "beta", 42U);
    expect_u32(&reader, "epsilon", 4294967295U);

    /* 有符号：i32 读负数，u32 读同一个键必须拿不到正确的数。 */
    {
        int32_t signed_value = 0;
        if (!emaster_config_reader_i32(&reader, "gamma", &signed_value) || signed_value != -7)
        {
            printf("不同  %-24s 得到 %d 期望 -7\n", "gamma", (int)signed_value);
            failures += 1;
        }
        else
        {
            printf("一致  %-24s %d\n", "gamma", (int)signed_value);
        }
    }

    /*
     * 空值读出来就是空字符串，读取照样成功。读器不做"空即缺失"的推断 ——
     * 那层判断在生成器里（字段非空由校验器管）。这里钉住的是读器的边界：
     * 只有**真正没有这一行**才算缺失。
     */
    expect_str(&reader, "alpha.empty", "");

    /* 不存在的键必须报缺失，而不是给个默认值。 */
    expect_absent_string(&reader, "no.such.key");
    expect_absent_string(&reader, "alpha.one");

    emaster_config_reader_close(&reader);
    (void)remove(path);
    printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
