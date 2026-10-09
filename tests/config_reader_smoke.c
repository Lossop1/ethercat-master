/* 配置读取器冒烟测试：从扁平文件读若干键，打印出来并核对。 */
#include "emaster/config/config_reader.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_str(const emaster_config_reader_t *r, const char *key, const char *want)
{
    const char *got = NULL;
    if (!emaster_config_reader_string(r, key, &got))
    {
        printf("缺失  %-44s 期望 %s\n", key, want);
        failures += 1;
        return;
    }
    if (strcmp(got, want) != 0)
    {
        printf("不同  %-44s 得到 %s 期望 %s\n", key, got, want);
        failures += 1;
        return;
    }
    printf("一致  %-44s %s\n", key, got);
}

static void expect_u32(const emaster_config_reader_t *r, const char *key, uint32_t want)
{
    uint32_t got = 0U;
    if (!emaster_config_reader_u32(r, key, &got) || got != want)
    {
        printf("不同  %-44s 得到 %u 期望 %u\n", key, got, want);
        failures += 1;
        return;
    }
    printf("一致  %-44s %u\n", key, got);
}

int main(int argc, char **argv)
{
    emaster_config_reader_t reader;

    if (argc != 2)
    {
        fprintf(stderr, "用法: %s <扁平配置文件>\n", argv[0]);
        return 2;
    }
    if (!emaster_config_reader_open(&reader, argv[1]))
    {
        fprintf(stderr, "打不开 %s\n", argv[1]);
        return 2;
    }
    printf("读入 %zu 条记录，%zu 字节\n\n", reader.line_count, reader.size);

    expect_str(&reader, "profile_id", "cyberbeast.isvd90rc-300b-100-70.rev1");
    expect_str(&reader, "identity.vendor_id", "0x000C0B00");
    expect_str(&reader, "model", "ISVD90RC-300B-100-70");
    expect_u32(&reader, "pdo_sets.count", 4U);
    expect_str(&reader, "pdo_sets.0.id", "dynamic_csp_csv_cst");
    expect_str(&reader, "pdo_sets.1.mode_initialization.index", "0x6060");
    expect_u32(&reader, "pdo_sets.1.mode_initialization.value", 10U);
    expect_str(&reader, "pdo_sets.2.rx.mappings.0.entries.1.name", "target_position");
    expect_u32(&reader, "pdo_sets.2.rx.mappings.0.entries.1.bits", 32U);
    expect_u32(&reader, "slow_telemetry.count", 12U);
    expect_str(&reader, "slow_telemetry.11.name", "max_motor_speed");

    /* 不存在的键必须报缺失，而不是给个默认值。 */
    {
        const char *missing = NULL;
        if (emaster_config_reader_string(&reader, "no.such.key", &missing))
        {
            printf("不同  %-44s 不该存在却有值\n", "no.such.key");
            failures += 1;
        }
        else
        {
            printf("一致  %-44s 正确报告缺失\n", "no.such.key");
        }
    }

    emaster_config_reader_close(&reader);
    printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
