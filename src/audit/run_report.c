#define _POSIX_C_SOURCE 200809L

#include "emaster/audit/run_report.h"

#include "report_sections.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

bool emaster_run_report_publish(const emaster_session_plan_t *plan,
                                const emaster_control_session_report_t *report,
                                const char *path)
{
    char temporary_path[PATH_MAX];
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
    return true;
}
