/*
 * 归档削旧的回归测试：削到几份、削的是哪几份、以及"不留历史"时**一份都不许删**。
 *
 * 现场来源（P12.29）：台架上同一个前缀连跑十几轮做 A/B，第 21 轮把第一轮的底稿删了，
 * 而"哪一轮丢过帧、丢了几次"恰恰要靠回头比对才能回答——P11.5 的 r5–r10 就是这么丢的，
 * 只剩文档里手抄的一份计数。当时的默认保留份数是 20，且 `unlink` 一句日志都不打：
 * 报告不见了，和"这一轮根本没跑"在现场长得一模一样。
 *
 * 本测试钉住四件事：
 *
 *   1. 同前缀的副本削到恰好 keep 份，且留下的是**最新的** keep 份（名字带 UTC 时间戳，
 *      字典序即时间序——要是排序方向写反，留下的就是最旧的，而报告看起来一样新）；
 *   2. **别的前缀一份都不动**。削旧只认前缀，不按"目录里最老的几个"删：归档目录理论上
 *      只放归档，但一个删除循环不该建立在"理论上"之上；
 *   3. keep = 0 时**一份都不删**。这一条此前是错的：注释写着"不做任何事"，代码走的是
 *      `count > keep`，0 会把同前缀的每一份都删掉（P12.32）。删除不可回滚，所以这条
 *      判据值得单独钉一次。
 *
 *      P12.32 的两道护栏是**成对**加的（archive_report 早退 = 不建副本，prune_archives
 *      早退 = 不删）。变异对照实测：抽掉 prune_archives 那道**单独**看不出区别——早退
 *      在前，prune 根本收不到 0，属等价变异；抽掉 archive_report 那道、或者两道一起抽掉
 *      就红。所以那道护栏留着是纵深防御（调用方将来多一个就可能传 0），不是这里要证的
 *      行为，别把它读成测试有洞；
 *
 *   4. 报告主体本身总在（削旧从不碰 run_report_path 那一份）。
 *
 * "删了要出声"（stderr 里那行 `归档削旧：…`）由
 * scripts/checks/report_archive_check.py 判——那要看子进程的 stderr，放在 C 里就得
 * 重定向并恢复 fd，反而更容易把要验的东西写歪。
 *
 * 编译与运行（本机 MinGW 建不了：src/audit 的 report 库只在 Linux 上建，见
 * src/audit/CMakeLists.txt）：
 *   gcc -std=c11 -Iinclude -o tmp/test_report_prune \
 *       tests/unit/audit/test_report_prune.c \
 *       src/audit/run_report.c src/audit/report_sections.c src/audit/json_writer.c
 *   ./tmp/test_report_prune
 */
/* mkdtemp 在严格 ANSI 模式下对 glibc 不可见，得在任何头文件之前先声明要哪个方言。 */
#define _POSIX_C_SOURCE 200809L

#include "emaster/audit/run_report.h"

#include "report_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 造这么多同前缀的旧副本，再按 keep 削。keep 远小于它是为了真的走到删除。 */
#define FAKE_ARCHIVE_COUNT 25U
#define PRUNE_KEEP 20U
/* 另一份部署的报告，前缀不同：它一份都不该被动。 */
#define FOREIGN_COUNT 6U

static unsigned int failures;

static void check(bool condition, const char *what)
{
    if (!condition)
    {
        ++failures;
        printf("  [不符] %s\n", what);
    }
}

static bool make_directory(const char *path)
{
    return mkdir(path, 0750) == 0 || (errno == EEXIST);
}

static bool touch(const char *path)
{
    FILE *stream = fopen(path, "wb");

    if (stream == NULL)
    {
        return false;
    }
    return fclose(stream) == 0;
}

/* 数归档目录里以 prefix 开头的条目，同时把最旧的那个名字带出来。 */
static size_t count_prefixed(const char *directory, const char *prefix, char *oldest,
                             size_t oldest_capacity)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    size_t count = 0U;

    if (dir == NULL)
    {
        return 0U;
    }
    if (oldest != NULL && oldest_capacity > 0U)
    {
        oldest[0] = '\0';
    }
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.' ||
            strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
        {
            continue;
        }
        ++count;
        if (oldest != NULL &&
            (oldest[0] == '\0' || strcmp(entry->d_name, oldest) < 0))
        {
            (void)snprintf(oldest, oldest_capacity, "%s", entry->d_name);
        }
    }
    (void)closedir(dir);
    return count;
}

static bool path_exists(const char *path)
{
    struct stat info;

    return stat(path, &info) == 0;
}

/* 夹具要摆在静态存储上：它装着一份完整报告，含现场环的两份 64 条样本。 */
static report_fixture_t fixture;

int main(void)
{
    char root[] = "/tmp/emaster-prune-XXXXXX";
    /* 缓冲给得比最长的路径还宽裕：写歪了要红，但不该被 -Wformat-truncation 的
     * "理论最长"提前劝退（它按 archive 满 1024 算）。 */
    char archive[1024];
    char report_path[1024];
    char name[2048];
    char oldest[2048];
    char oldest_survived[4096];
    unsigned int index;
    size_t deleted;

    if (mkdtemp(root) == NULL)
    {
        printf("  [不符] 造不出临时目录\n");
        return 2;
    }
    (void)snprintf(archive, sizeof(archive), "%s/archive", root);
    (void)snprintf(report_path, sizeof(report_path), "%s/bench-latest.json", root);
    if (!make_directory(archive))
    {
        printf("  [不符] 造不出归档目录\n");
        return 2;
    }
    if (!touch(report_path))
    {
        printf("  [不符] 造不出报告主体的占位文件\n");
        return 2;
    }

    /*
     * 25 份同前缀的旧副本，时间戳从 2025-01-01 起逐秒递增；外加同目录下别人前缀的
     * 6 份、以及一个点开头的隐藏文件（前缀匹配与点过滤都不该碰它们）。
     */
    for (index = 0U; index < FAKE_ARCHIVE_COUNT; ++index)
    {
        (void)snprintf(name, sizeof(name), "%s/bench-latest-20250101T0000%02uZ.json",
                       archive, index);
        if (!touch(name))
        {
            printf("  [不符] 造不出假副本 %s\n", name);
            return 2;
        }
    }
    for (index = 0U; index < FOREIGN_COUNT; ++index)
    {
        (void)snprintf(name, sizeof(name), "%s/other-deployment-20250101T0000%02uZ.json",
                       archive, index);
        if (!touch(name))
        {
            printf("  [不符] 造不出别人的副本 %s\n", name);
            return 2;
        }
    }
    (void)snprintf(name, sizeof(name), "%s/.hidden", archive);
    if (!touch(name))
    {
        printf("  [不符] 造不出隐藏文件\n");
        return 2;
    }

    /* ── 1/2/4. 正常削旧 ────────────────────────────────────────────────── */
    printf("削旧：留最新 keep 份，别的前缀不动\n");
    report_fixture_init(&fixture, report_path);
    if (!emaster_run_report_publish(&fixture.plan, &fixture.report, report_path, PRUNE_KEEP))
    {
        printf("  [不符] 报告发布失败\n");
        return 2;
    }

    {
        size_t remaining = count_prefixed(archive, "bench-latest-", oldest, sizeof(oldest));

        /* 25 份旧的 + 刚发布的那一份，削到 20 份。 */
        check(remaining == PRUNE_KEEP, "同前缀副本恰好剩 keep 份");
        /*
         * 留下的是最新的 20 份，所以最旧的那份必须是原来第 6 份（下标 5）：
         * 前 6 份（0~5）被删。排序方向写反时，这里留下的是下标 0。
         */
        deleted = FAKE_ARCHIVE_COUNT + 1U - PRUNE_KEEP;
        (void)snprintf(name, sizeof(name), "bench-latest-20250101T0000%02uZ.json",
                       (unsigned int)deleted);
        check(strcmp(oldest, name) == 0,
              "留下的是最新的 keep 份（最旧的一份是原来第 6 份）");
        (void)snprintf(name, sizeof(name), "%s/bench-latest-20250101T000000Z.json", archive);
        check(!path_exists(name), "真正最旧的那一份被删掉了");
        /* 记下削完之后的旧端点，第三步要拿它证明"一份都没再动过"。 */
        (void)snprintf(oldest_survived, sizeof(oldest_survived), "%s/%s", archive, oldest);
    }
    check(count_prefixed(archive, "other-deployment-", NULL, 0U) == FOREIGN_COUNT,
          "别的前缀一份都没动");
    (void)snprintf(name, sizeof(name), "%s/.hidden", archive);
    check(path_exists(name), "点开头的文件没被前缀匹配卷进来");
    check(path_exists(report_path), "报告主体那一份还在");

    /*
     * 两次发布之间必须跨过一个整秒。
     *
     * 归档名的时间戳精度是秒，同一秒里再发布一次会算出**完全相同**的文件名，
     * `link()` 撞上已存在的目标直接失败——于是既不建副本也不削旧。实测确认过：
     * 少了这个等待，第二次发布什么都不发生，"新副本建没建"根本观察不到，
     * 变异对照会假绿（把 keep=0 的两道护栏都拆掉，测试照样通过）。
     */
    {
        struct timespec pause = {.tv_sec = 1, .tv_nsec = 200000000L};

        (void)nanosleep(&pause, NULL);
    }

    /* ── 3. keep = 0：一份都不许删 ─────────────────────────────────────── */
    printf("keep=0：不留历史，但不许删光历史\n");
    check(count_prefixed(archive, "bench-latest-", NULL, 0U) == PRUNE_KEEP,
          "第二次发布前的基线：同前缀 20 份");
    if (!emaster_run_report_publish(&fixture.plan, &fixture.report, report_path, 0U))
    {
        printf("  [不符] keep=0 时报告发布失败\n");
        return 2;
    }
    check(count_prefixed(archive, "bench-latest-", NULL, 0U) == PRUNE_KEEP,
          "keep=0 时一份都不删（此前这里会削成 0，P12.32）");
    check(count_prefixed(archive, "other-deployment-", NULL, 0U) == FOREIGN_COUNT,
          "keep=0 时别人的副本也一份不动");
    /*
     * 钉住"新的也没建"：keep=0 按配置说明是"只保留 run_report_path 那一份"，
     * 建了的话计数会变成 21——删旧和建新是两件事，这里一起看住。
     */
    check(path_exists(oldest_survived), "keep=0 时削旧后剩下的最旧那一份也还在");

    if (failures != 0U)
    {
        printf("归档削旧：%u 项不符\n", failures);
        return 1;
    }
    printf("归档削旧：全部通过\n");
    return 0;
}
