/* 回合测试：扁平文件 -> apply -> dump，dump 的键空间与输入完全一致，可直接比对。 */
#include "emaster/config/config_reader.h"
#include "emaster/config/generated_apply.h"
#include "emaster/config/runtime_config.h"

#include <stdio.h>

/* 生成物提供的入口，声明与生成代码一致。 */
bool emaster_generated_config_apply(const emaster_config_reader_t *reader);

int main(int argc, char **argv)
{
    emaster_config_reader_t reader;
    size_t index;
    size_t slave_index;

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
    if (!emaster_generated_config_apply(&reader))
    {
        fprintf(stderr, "apply 失败：配置里缺键或键不可解析\n");
        emaster_config_reader_close(&reader);
        return 1;
    }

    printf("topology.count\t%zu\n", emaster_topology_config_count());
    for (index = 0U; index < emaster_topology_config_count(); ++index)
    {
        const emaster_topology_config_t *topology =
            emaster_topology_config_at(index);
        if (topology == NULL)
        {
            fprintf(stderr, "第 %zu 份拓扑为空\n", index);
            return 1;
        }
        printf("topology.%zu.topology_id\t%s\n", index, topology->topology_id);
        printf("topology.%zu.status\t%s\n", index, topology->status);
        printf("topology.%zu.slaves.count\t%zu\n", index, topology->slave_count);
        for (slave_index = 0U; slave_index < topology->slave_count; ++slave_index)
        {
            const emaster_topology_slave_config_t *slave =
                &topology->slaves[slave_index];
            printf("topology.%zu.slaves.%zu.position\t%u\n", index, slave_index,
                   (unsigned)slave->position);
            printf("topology.%zu.slaves.%zu.axis_id\t%s\n", index, slave_index,
                   slave->axis_id);
            printf("topology.%zu.slaves.%zu.profile_id\t%s\n", index, slave_index,
                   slave->profile_id);
        }
    }

    emaster_config_reader_close(&reader);
    return 0;
}
