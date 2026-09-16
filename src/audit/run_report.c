#define _POSIX_C_SOURCE 200809L

#include "emaster/audit/run_report.h"

#include "report_sections.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 归档目录名，挂在报告路径所在目录下。 */
#define ARCHIVE_DIRECTORY_NAME "archive"
/* 单个归档文件名的容量，也是收集目录项时每个条目的步长。 */
#define ARCHIVE_NAME_CAPACITY 256U
/* 归档文件名里的时间戳：紧凑到秒，且按字典序等于按时间序，削旧时不必解析。 */
#define ARCHIVE_STAMP_FORMAT "%Y%m%dT%H%M%SZ"

static bool ensure_parent_directories(const char *path)
{
    char copy[PATH_MAX];
    char *cursor;

    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(copy))
    {
        return false;
    }
    (void)snprintf(copy, sizeof(copy), "%s", path);
    for (cursor = copy + 1; *cursor != '\0'; ++cursor)
    {
        if (*cursor != '/')
        {
            continue;
        }
        *cursor = '\0';
        if (mkdir(copy, 0750) != 0 && errno != EEXIST)
        {
            return false;
        }
        *cursor = '/';
    }
    return true;
}

static bool utc_timestamp(char *buffer, size_t capacity)
{
    struct timespec now;
    struct tm utc;

    return buffer != NULL && capacity > 0U &&
           clock_gettime(CLOCK_REALTIME, &now) == 0 &&
           gmtime_r(&now.tv_sec, &utc) != NULL &&
           strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%SZ", &utc) > 0U;
}

bool emaster_run_report_resolve_path(const char *path, char *buffer, size_t capacity)
{
    char cwd[PATH_MAX];
    int written;

    if (path == NULL || path[0] == '\0' || buffer == NULL || capacity == 0U)
    {
        return false;
    }
    if (path[0] == '/')
    {
        if (strlen(path) + 1U > capacity)
        {
            return false;
        }
        (void)snprintf(buffer, capacity, "%s", path);
        return true;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL)
    {
        return false;
    }
    written = snprintf(buffer, capacity, "%s/%s", cwd, path);
    return written > 0 && (size_t)written < capacity;
}

/*
 * 报告路径所在目录下的 archive/ 子目录。报告路径没有目录成分时用当前目录。
 * 目录不在这里创建——真正要写副本时才建，免得每次发布都留下空目录。
 */
static bool archive_directory_for(const char *path, char *buffer, size_t capacity)
{
    const char *slash = strrchr(path, '/');
    int written;

    if (slash == NULL)
    {
        written = snprintf(buffer, capacity, "%s", ARCHIVE_DIRECTORY_NAME);
    }
    else
    {
        written = snprintf(buffer, capacity, "%.*s/%s", (int)(slash - path), path,
                           ARCHIVE_DIRECTORY_NAME);
    }
    return written > 0 && (size_t)written < capacity;
}

/*
 * 归档文件名：<报告文件名主干>-<UTC 时间戳>.json。
 *
 * 主干在前、时间戳在后不是随手定的：削旧只认"主干-"这个前缀，时间戳若在前面，
 * 同一次运行产生的所有归档就都不以主干开头，前缀匹配一条也命中不了——删除循环
 * 会安静地什么都不做，保留份数形同虚设（这正是第一版写的顺序）。
 */
static bool archive_name_for(const char *stem, const char *stamp,
                             char *buffer, size_t capacity)
{
    int written = snprintf(buffer, capacity, "%s-%s.json", stem, stamp);

    return written > 0 && (size_t)written < capacity;
}

/*
 * 归档前缀。削旧时只认这个前缀，绝不按"目录里最老的几个文件"删——归档目录理论上
 * 只放归档，但"理论上"不足以让一个删除循环安全。
 */
static bool archive_prefix_for(const char *stem, char *buffer, size_t capacity)
{
    int written = snprintf(buffer, capacity, "%s-", stem);

    return written > 0 && (size_t)written < capacity;
}

static int compare_names(const void *left, const void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

/*
 * 把归档目录里同前缀的副本削到剩 keep 份。名字带 UTC 时间戳，字典序即时间序，
 * 因此"最旧的"就是排序后最靠前的几个，不需要解析时间、不需要 stat。
 *
 * keep 为 0 时不做任何事：那表示"不留历史"，不是"删光历史"。
 */
static void prune_archives(const char *directory, const char *prefix, uint32_t keep)
{
    char (*names)[ARCHIVE_NAME_CAPACITY] = NULL;
    size_t prefix_length = strlen(prefix);
    size_t count = 0U;
    size_t reserved = 0U;
    size_t index;
    DIR *dir;
    struct dirent *entry;

    dir = opendir(directory);
    if (dir == NULL)
    {
        return;
    }
    /* 先收集再删除：边遍历边删目录项是未定义行为。 */
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.' ||
            strncmp(entry->d_name, prefix, prefix_length) != 0)
        {
            continue;
        }
        if (count == reserved)
        {
            size_t next = reserved == 0U ? 16U : reserved * 2U;
            void *grown = realloc(names, next * sizeof(*names));

            if (grown == NULL)
            {
                (void)closedir(dir);
                free(names);
                return;
            }
            names = grown;
            reserved = next;
        }
        (void)snprintf(names[count], sizeof(names[count]), "%s", entry->d_name);
        ++count;
    }
    (void)closedir(dir);

    if (count > keep)
    {
        qsort(names, count, sizeof(*names), compare_names);
        for (index = 0U; index + keep < count; ++index)
        {
            char victim[PATH_MAX];

            if (snprintf(victim, sizeof(victim), "%s/%s", directory, names[index]) > 0)
            {
                (void)unlink(victim);
            }
        }
    }
    free(names);
}

/*
 * 给已写好的报告留一份历史副本。失败不改变发布结果：报告主体已经落盘，
 * 历史副本是附加价值，不能因为它失败就让一次成功的运行报成失败。
 */
static void archive_report(const char *path, const char *stem, uint32_t keep)
{
    char directory[PATH_MAX];
    char prefix[ARCHIVE_NAME_CAPACITY];
    char stamp[32];
    char name[ARCHIVE_NAME_CAPACITY];
    char destination[PATH_MAX];
    struct tm utc;
    struct timespec now;

    if (!archive_directory_for(path, directory, sizeof(directory)) ||
        !archive_prefix_for(stem, prefix, sizeof(prefix)))
    {
        return;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || gmtime_r(&now.tv_sec, &utc) == NULL ||
        strftime(stamp, sizeof(stamp), ARCHIVE_STAMP_FORMAT, &utc) == 0U ||
        !archive_name_for(stem, stamp, name, sizeof(name)) ||
        snprintf(destination, sizeof(destination), "%s/%s", directory, name) <= 0)
    {
        return;
    }
    if (mkdir(directory, 0750) != 0 && errno != EEXIST)
    {
        return;
    }
    /* 硬链接：报告此后不再修改，链接在语义上就是一份副本，代价为零。 */
    if (link(path, destination) == 0)
    {
        prune_archives(directory, prefix, keep);
    }
}

/* 从报告路径取出不含目录与扩展名的文件名主干，用作归档名与前缀。 */
static bool report_stem(const char *path, char *buffer, size_t capacity)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash == NULL ? path : slash + 1;
    const char *dot = strrchr(base, '.');
    size_t length = dot == NULL ? strlen(base) : (size_t)(dot - base);

    if (length == 0U || length + 1U > capacity)
    {
        return false;
    }
    memcpy(buffer, base, length);
    buffer[length] = '\0';
    return true;
}

bool emaster_run_report_publish(const emaster_session_plan_t *plan,
                                const emaster_control_session_report_t *report,
                                const char *path,
                                uint32_t archive_keep)
{
    char temporary_path[PATH_MAX];
    char stem[ARCHIVE_NAME_CAPACITY];
    char timestamp[32];
    FILE *stream;
    bool written;

    if (plan == NULL || report == NULL || path == NULL ||
        strlen(path) >= sizeof(temporary_path) - 32U ||
        !ensure_parent_directories(path) || !utc_timestamp(timestamp, sizeof(timestamp)) ||
        snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.%ld", path,
                 (long)getpid()) < 0)
    {
        return false;
    }
    stream = fopen(temporary_path, "wx");
    if (stream == NULL)
    {
        return false;
    }
    written = emaster_run_report_write(stream, plan, report, timestamp) &&
              fflush(stream) == 0 && fsync(fileno(stream)) == 0;
    if (fclose(stream) != 0)
    {
        written = false;
    }
    if (!written || rename(temporary_path, path) != 0)
    {
        (void)unlink(temporary_path);
        return false;
    }
    if (report_stem(path, stem, sizeof(stem)))
    {
        archive_report(path, stem, archive_keep);
    }
    return true;
}
