/* 回合测试：扁平文件 -> catalog apply -> dump，键空间与输入一致，可直接比对。 */
#include "emaster/catalog/slave_profile.h"
#include "emaster/config/config_reader.h"
#include "emaster/config/generated_catalog.h"

#include <stdio.h>

bool emaster_generated_catalog_apply(const emaster_config_reader_t *reader);

int main(int argc, char **argv)
{
    emaster_config_reader_t reader;
    size_t pi;

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
    if (!emaster_generated_catalog_apply(&reader))
    {
        fprintf(stderr, "apply 失败\n");
        emaster_config_reader_close(&reader);
        return 1;
    }
    /* 装进来的条数必须等于文件声明的条数：少了就是装载中途半途而废，
       而它不会自己报错 —— 上层只会看到"少了几台设备"。 */
    {
        size_t declared = 0U;
        if (!emaster_config_reader_size(&reader, "profile.count", &declared) ||
            declared != emaster_slave_profile_count())
        {
            fprintf(stderr, "profile.count 声明 %zu，装载了 %zu\n",
                    declared, emaster_slave_profile_count());
            emaster_config_reader_close(&reader);
            return 1;
        }
    }

    printf("profile.count\t%zu\n", emaster_slave_profile_count());
    for (pi = 0U; pi < emaster_slave_profile_count(); ++pi)
    {
        const emaster_slave_profile_t *p = emaster_slave_profile_at(pi);
        size_t si, mi, ei, ti;

        printf("profile.%zu.profile_id\t%s\n", pi, p->profile_id);
        printf("profile.%zu.model\t%s\n", pi, p->model);
        printf("profile.%zu.identity.vendor_id\t0x%08X\n", pi, p->identity.vendor_id);
        printf("profile.%zu.identity.product_code\t0x%08X\n", pi, p->identity.product_code);
        printf("profile.%zu.identity.revision\t0x%08X\n", pi, p->identity.revision);
        printf("profile.%zu.reference_pdo_set_id\t%s\n", pi, p->reference_pdo_set_id);
        printf("profile.%zu.encoder_counts\t%u\n", pi, p->encoder_counts_per_motor_revolution_default);
        printf("profile.%zu.supports_pdo_assignment\t%s\n", pi, p->supports_pdo_assignment ? "true" : "false");
        printf("profile.%zu.supports_pdo_configuration\t%s\n", pi, p->supports_pdo_configuration ? "true" : "false");
        printf("profile.%zu.supports_distributed_clocks\t%s\n", pi, p->supports_distributed_clocks ? "true" : "false");
        printf("profile.%zu.safe_stop_status_mask\t%u\n", pi, (unsigned)p->safe_stop_status_mask);
        printf("profile.%zu.safe_stop_status_value\t%u\n", pi, (unsigned)p->safe_stop_status_value);
        printf("profile.%zu.pdo_sets.count\t%zu\n", pi, p->pdo_set_count);
        for (si = 0U; si < p->pdo_set_count; ++si)
        {
            const emaster_pdo_set_profile_t *s = &p->pdo_sets[si];
            printf("profile.%zu.pdo_sets.%zu.pdo_set_id\t%s\n", pi, si, s->pdo_set_id);
            printf("profile.%zu.pdo_sets.%zu.module_ident\t0x%08X\n", pi, si, s->module_ident);
            printf("profile.%zu.pdo_sets.%zu.module_slot\t%u\n", pi, si, (unsigned)s->module_slot);
            printf("profile.%zu.pdo_sets.%zu.assignment_style\t%s\n", pi, si,
                   s->assignment_style == EMASTER_PDO_ASSIGNMENT_DIRECT ? "direct_1c12"
                                                                        : "module_slot");
            printf("profile.%zu.pdo_sets.%zu.mode_init_value\t%d\n", pi, si, (int)s->mode_init_value);
            printf("profile.%zu.pdo_sets.%zu.rx_pdo_bytes\t%u\n", pi, si, (unsigned)s->rx_pdo_bytes);
            printf("profile.%zu.pdo_sets.%zu.tx_pdo_bytes\t%u\n", pi, si, (unsigned)s->tx_pdo_bytes);
            for (int d = 0; d < 2; ++d)
            {
                const char *dir = d == 0 ? "rx" : "tx";
                const emaster_pdo_mapping_profile_t *maps = d == 0 ? s->rx_mappings : s->tx_mappings;
                size_t count = d == 0 ? s->rx_mapping_count : s->tx_mapping_count;
                printf("profile.%zu.pdo_sets.%zu.%s_mappings.count\t%zu\n", pi, si, dir, count);
                for (mi = 0U; mi < count; ++mi)
                {
                    printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.index\t0x%04X\n", pi, si, dir, mi, (unsigned)maps[mi].index);
                    printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.bit_length\t%u\n", pi, si, dir, mi, maps[mi].bit_length);
                    printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.entries.count\t%zu\n", pi, si, dir, mi, maps[mi].entry_count);
                    for (ei = 0U; ei < maps[mi].entry_count; ++ei)
                    {
                        const emaster_pdo_entry_t *e = &maps[mi].entries[ei];
                        printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.entries.%zu.index\t0x%04X\n", pi, si, dir, mi, ei, (unsigned)e->index);
                        printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.entries.%zu.subindex\t%u\n", pi, si, dir, mi, ei, (unsigned)e->subindex);
                        printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.entries.%zu.bit_length\t%u\n", pi, si, dir, mi, ei, (unsigned)e->bit_length);
                        printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.entries.%zu.data_type\t%s\n", pi, si, dir, mi, ei, e->data_type);
                        printf("profile.%zu.pdo_sets.%zu.%s_mappings.%zu.entries.%zu.name\t%s\n", pi, si, dir, mi, ei, e->name);
                    }
                }
            }
        }
        printf("profile.%zu.slow_telemetry.count\t%zu\n", pi, p->slow_telemetry_count);
        for (ti = 0U; ti < p->slow_telemetry_count; ++ti)
        {
            const emaster_slow_telemetry_t *t = &p->slow_telemetry[ti];
            printf("profile.%zu.slow_telemetry.%zu.name\t%s\n", pi, ti, t->name);
            printf("profile.%zu.slow_telemetry.%zu.index\t0x%04X\n", pi, ti, (unsigned)t->index);
            printf("profile.%zu.slow_telemetry.%zu.subindex\t%u\n", pi, ti, (unsigned)t->subindex);
            printf("profile.%zu.slow_telemetry.%zu.type\t%d\n", pi, ti, (int)t->type);
            printf("profile.%zu.slow_telemetry.%zu.semantic\t%d\n", pi, ti, (int)t->semantic);
            printf("profile.%zu.slow_telemetry.%zu.unit\t%s\n", pi, ti, t->unit);
        }
    }
    emaster_config_reader_close(&reader);
    return 0;
}
