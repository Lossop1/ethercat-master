/*
 * 接入验证：只调用 runtime_config.h / slave_profile.h 声明的公开访问函数，
 * 不看任何生成物的内部符号 —— 也就是 src/ 里那几百处调用的用法。
 *
 * 期望值从哪来。**从被交给它的那几个扁平文件里读**，不写死任何型号或站点。
 * 这样同一份测试既跑得动交付时的空配置（问的是"空配置是合法的可装载状态"），
 * 也跑得动客户自己生成的那套（问的是"访问函数报的与文件里写的一致"）——
 * 而换一款电机、换一张网卡都不用改这个文件。
 *
 * 为什么要先有这一条：数据来源从编译期数组换成运行时加载时，最容易的错法不是
 * 崩，而是"装进去了但访问函数还看着旧的那份"。这里逐族对照 count 与 by_id。
 */
#include "emaster/catalog/slave_profile.h"
#include "emaster/config/config_reader.h"
#include "emaster/config/generated_apply.h"
#include "emaster/config/generated_catalog.h"
#include "emaster/config/generated_runtime.h"
#include "emaster/config/generated_deployment.h"

#include <stdio.h>
#include <string.h>

/*
 * 四个读取器全部常驻到进程结束才关：填进结构体的每个字符串都指向读取器那块缓冲，
 * 关掉就等于把整份配置变成悬垂指针。全部断言做完再统一关。
 */
static emaster_config_reader_t readers[4];
static int failures = 0;

static void fail(const char *what)
{
    printf("不同  %s\n", what);
    failures += 1;
}

static void ok(const char *what)
{
    printf("一致  %s\n", what);
}

static void check_size(const char *what, size_t got, size_t want)
{
    if (got != want)
    {
        printf("不同  %-46s 得到 %zu 期望 %zu\n", what, got, want);
        failures += 1;
        return;
    }
    printf("一致  %-46s %zu\n", what, got);
}

/* 从扁平文件里读某个计数；读不到就是 0 —— 空的族连 .count 键都可能没有。 */
static size_t flat_count(const emaster_config_reader_t *reader, const char *key)
{
    size_t value = 0U;
    if (!emaster_config_reader_size(reader, key, &value))
    {
        return 0U;
    }
    return value;
}

/*
 * 每一族的三条断言都长一个样，用宏摊开只为不把同一段写五遍：
 *   count 相等；at(0) 非空且 by_id(它的 id) 就是它；at(count) 越界返回空。
 */
#define CHECK_LOOKUP(label, API_COUNT, AT, BY_ID, TOTAL, ID_TEXT)                       \
    do                                                                                  \
    {                                                                                   \
        check_size(label " 条数", (size_t)(API_COUNT), (size_t)(TOTAL));                 \
        if ((TOTAL) > 0U)                                                               \
        {                                                                               \
            const void *first = (const void *)(AT)(0U);                                 \
            if (first == NULL)                                                          \
            {                                                                           \
                fail(label " at(0) 返回空");                                             \
            }                                                                           \
            else if ((const void *)(BY_ID)((ID_TEXT)) != first)                          \
            {                                                                           \
                fail(label " by_id 与 at(0) 不是同一个条目");                             \
            }                                                                           \
            else                                                                        \
            {                                                                           \
                ok(label " 首项可按 ID 查到");                                            \
            }                                                                           \
            if ((AT)((TOTAL)) != NULL)                                                  \
            {                                                                           \
                fail(label " at(count) 越界却返回了条目");                                \
            }                                                                           \
            else                                                                        \
            {                                                                           \
                ok(label " 越界返回空");                                                 \
            }                                                                           \
        }                                                                               \
    } while (0)

int main(int argc, char **argv)
{
    const char *paths[4];
    char first_id[256];
    const char *text = NULL;
    size_t index;
    size_t total;

    if (argc != 5)
    {
        fprintf(stderr, "用法: %s <topo> <cat> <rt> <dep>\n", argv[0]);
        return 2;
    }
    paths[0] = argv[1]; paths[1] = argv[2]; paths[2] = argv[3]; paths[3] = argv[4];
    for (index = 0U; index < 4U; ++index)
    {
        if (!emaster_config_reader_open(&readers[index], paths[index]))
        {
            fprintf(stderr, "打不开 %s\n", paths[index]);
            return 2;
        }
    }
    if (!emaster_generated_config_apply(&readers[0]) ||
        !emaster_generated_catalog_apply(&readers[1]) ||
        !emaster_generated_runtime_apply(&readers[2]) ||
        !emaster_generated_deployment_apply(&readers[3]))
    {
        fprintf(stderr, "加载失败\n");
        return 1;
    }
    printf("四族装载完成\n\n");

    /* 拓扑族。 */
    total = flat_count(&readers[0], "topology.count");
    if (total > 0U && emaster_config_reader_string(&readers[0], "topology.0.topology_id", &text))
    {
        (void)snprintf(first_id, sizeof(first_id), "%s", text);
        CHECK_LOOKUP("拓扑", emaster_topology_config_count(),
                     emaster_topology_config_at, emaster_topology_config_by_id,
                     total, first_id);
        /* 拓扑指向的设备档指针必须真的指到装好的那一条 —— 跨族指针接错了
           只会在运行期某处解引用时才炸。 */
        {
            const emaster_topology_config_t *topology =
                emaster_topology_config_by_id(first_id);
            size_t slaves = flat_count(&readers[0], "topology.0.slaves.count");
            if (topology == NULL)
            {
                fail("拓扑 by_id 返回空");
            }
            else if (topology->slave_count != slaves)
            {
                printf("不同  topology->slave_count 得到 %zu 期望 %zu\n",
                       topology->slave_count, slaves);
                failures += 1;
            }
            else
            {
                ok("topology->slave_count 与文件一致");
            }
        }
    }
    else
    {
        check_size("拓扑 条数", emaster_topology_config_count(), total);
    }

    /* 设备目录族。 */
    total = flat_count(&readers[1], "profile.count");
    if (total > 0U && emaster_config_reader_string(&readers[1], "profile.0.profile_id", &text))
    {
        (void)snprintf(first_id, sizeof(first_id), "%s", text);
        CHECK_LOOKUP("设备档", emaster_slave_profile_count(),
                     emaster_slave_profile_at, emaster_slave_profile_by_id,
                     total, first_id);
        {
            const emaster_slave_profile_t *profile = emaster_slave_profile_by_id(first_id);
            if (profile == NULL)
            {
                fail("设备档 by_id 返回空");
            }
            else if (emaster_slave_reference_pdo_set(profile) == NULL)
            {
                fail("设备档的参考 PDO 方案解析不出来");
            }
            else
            {
                ok("参考 PDO 方案可解析");
            }
        }
    }
    else
    {
        check_size("设备档 条数", emaster_slave_profile_count(), total);
    }

    /* 运行方案族。 */
    total = flat_count(&readers[2], "operation.count");
    if (total > 0U &&
        emaster_config_reader_string(&readers[2], "operation.0.operation_profile_id", &text))
    {
        (void)snprintf(first_id, sizeof(first_id), "%s", text);
        CHECK_LOOKUP("运行方案", emaster_operation_profile_count(),
                     emaster_operation_profile_at, emaster_operation_profile_by_id,
                     total, first_id);
    }
    else
    {
        check_size("运行方案 条数", emaster_operation_profile_count(), total);
    }

    /* 运动方案族。 */
    total = flat_count(&readers[2], "motion.count");
    if (total > 0U &&
        emaster_config_reader_string(&readers[2], "motion.0.motion_profile_id", &text))
    {
        (void)snprintf(first_id, sizeof(first_id), "%s", text);
        CHECK_LOOKUP("运动方案", emaster_motion_profile_count(),
                     emaster_motion_profile_at, emaster_motion_profile_by_id,
                     total, first_id);
    }
    else
    {
        check_size("运动方案 条数", emaster_motion_profile_count(), total);
    }

    /* 部署族。 */
    total = flat_count(&readers[3], "deployment.count");
    if (total > 0U && emaster_config_reader_string(&readers[3], "deployment.0.id", &text))
    {
        const emaster_deployment_config_t *deployment;

        (void)snprintf(first_id, sizeof(first_id), "%s", text);
        CHECK_LOOKUP("部署", emaster_deployment_config_count(),
                     emaster_deployment_config_at, emaster_deployment_config_by_id,
                     total, first_id);

        deployment = emaster_deployment_config_by_id(first_id);
        if (deployment == NULL)
        {
            fail("部署 by_id 返回空");
        }
        else
        {
            /* 部署引用的三族指针必须都接上了：装载顺序错了或 ID 对不上，
               这里就是空的。 */
            if (deployment->topology == NULL)
            {
                fail("部署没有接上拓扑");
            }
            else
            {
                ok("部署接上了拓扑");
            }
            /* 有运行方案就逐个查得到；没有也合法（空闲档）。 */
            if (deployment->operation_profile_count > 0U &&
                deployment->operation_profiles[0] == NULL)
            {
                fail("部署的运行方案指针数组里有空指针");
            }
            else
            {
                ok("部署的运行方案指针已接上");
            }
        }
    }
    else
    {
        check_size("部署 条数", emaster_deployment_config_count(), total);
    }

    /* 不存在的 ID 必须返回空，不能落到别的条目上。 */
    if (emaster_deployment_config_by_id("no-such-deployment") != NULL ||
        emaster_slave_profile_by_id("no-such-profile") != NULL ||
        emaster_topology_config_by_id("no-such-topology") != NULL)
    {
        fail("不存在的 ID 却查到了条目");
    }
    else
    {
        ok("不存在的 ID 返回空");
    }

    for (index = 0U; index < 4U; ++index)
    {
        emaster_config_reader_close(&readers[index]);
    }
    printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
