#include "emaster/config/runtime_config_load.h"

#include "emaster/config/config_reader.h"
#include "emaster/config/generated_apply.h"
#include "emaster/config/generated_catalog.h"
#include "emaster/config/generated_deployment.h"
#include "emaster/config/generated_runtime.h"

#include <stdio.h>
#include <string.h>

/*
 * 四个读取器常驻，直到进程结束都不关：
 * 填进结构体的字符串指向读取器那块缓冲，关掉就等于把配置里的每个字符串
 * 变成悬垂指针。第一版在每族加载后 close，运行结果是"引用了不存在的 ID"——
 * 而真正的原因不是查找失败，是内存已经没了。
 */
static emaster_config_reader_t g_readers[4];
static bool g_loaded = false;

static bool load_one(size_t slot, const char *directory, const char *name,
                     bool (*apply)(const emaster_config_reader_t *))
{
    char path[512];
    int written;

    written = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
        fprintf(stderr, "配置路径过长：%s/%s\n", directory, name);
        return false;
    }
    if (!emaster_config_reader_open(&g_readers[slot], path))
    {
        fprintf(stderr, "打不开 %s\n", path);
        return false;
    }
    if (!apply(&g_readers[slot]))
    {
        /*
         * 指名道姓地说出是哪个键。原来这里只有一句"缺键、类型不符，或引用了不存在的
         * ID"—— 三种完全不同的原因混成一句，实测四条故意造坏的配置里三条报的是同一
         * 句话。改配置的人需要知道的正是哪一个键。
         *
         * "引用了不存在的 ID" 这一类不走读取器（ID 是取到了、但在已加载的数组里查不到），
         * 所以这里单独说清楚：取到的键都在，失败在跨族引用上。
         */
        const char *why = NULL;
        const char *key = emaster_config_reader_last_failure(&why);

        if (key != NULL)
        {
            fprintf(stderr, "解析失败 %s：配置项 '%s' —— %s\n", path, key,
                    why != NULL ? why : "未知原因");
        }
        else
        {
            /*
             * 走到这里说明失败发生在**生成代码自己的检查**里（范围检查、
             * 跨族 ID 查找），那些地方还没有记录原因。
             * 所以这里只能如实列举可能性，不能挑一个说得像结论 ——
             * 第一版在这里断言"跨族引用查不到"，而实测一条值超范围的配置
             * 被报成了那个，是自信的错误结论。
             */
            fprintf(stderr, "解析失败 %s：失败原因未被记录（生成代码里有漏登记的检查）\n",
                    path);
        }
        return false;
    }
    return true;
}

bool emaster_runtime_config_load(const char *directory)
{
    if (directory == NULL || directory[0] == '\0')
    {
        return false;
    }
    if (g_loaded)
    {
        return true;
    }
    /* 顺序不能颠倒：部署按 ID 引用前三者。 */
    if (!load_one(0U, directory, "topologies.flat", emaster_generated_config_apply) ||
        !load_one(1U, directory, "catalog.flat", emaster_generated_catalog_apply) ||
        !load_one(2U, directory, "runtime.flat", emaster_generated_runtime_apply) ||
        !load_one(3U, directory, "deployments.flat", emaster_generated_deployment_apply))
    {
        return false;
    }
    g_loaded = true;
    return true;
}
