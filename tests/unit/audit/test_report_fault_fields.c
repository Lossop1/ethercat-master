/*
 * 报告里 P12.17 那块故障证据的回归测试：字段有没有落盘、值对不对、"没记过"会不会
 * 被渲染成一个看起来像读数的 0。
 *
 * 现场来源（2026-09-18 18:05 那轮五轴空转，
 * orangepi-quint-30deg-latest-20260918T100505Z.json）：会话跑到第 22383 拍
 * （共 22388 拍）以 DRIVE_FAULT 结束，轴 2/3/5 是 CiA402 Fault（状态字 0x1208），
 * 轴 1/4 仍是 Operation Enabled（0x0237），而总线干净得反常——WKC 15/15、未回 0 次、
 * 邮箱错误环为空、五轴 AL 一律 state 8 / code 0。要接着问"哪根轴在第几拍开始不对、
 * 603F 里有没有东西"，报告里当时一样都没有。
 *
 * 本测试钉住的就是那一轮补上的那几列（schema_version 5），外加 P12.46 补的
 * 第四个 AL 坐标 shutdown_al_exit（schema_version 7，跳过 6 的理由见 report_sections.c）：
 *
 *   1. 逐轴判故障的坐标：recorded / exchange / status_word / reason / isolated
 *      全部出现在该轴对象里，值是记下的那一个；
 *   2. fault_timeline 四项各自带 seen。**这是"哪根轴第几拍开始不对"的唯一来源**——
 *      现场环答不了（每会话一条、只留最后 64 拍、且在逐轴解码之前入环）；
 *   3. **没判过故障的轴不许凭空长出故障码**：recorded 必须是 false、时间线四项
 *      必须全部 seen=false、运行期 603F 必须是 read=false。少这一条，任何"忘了赋值"
 *      都会在报告里长成一份看起来确凿的故障证据；
 *   4. 运行期 603F 的"读到 0x0000"与"从没读到"必须印得不一样（read=true + "0x0000"
 *      对 read=false + "0x0000"）。与慢速遥测那套同一条约定（P11.8）：0 与"没读到"
 *      长得一样时，报告里那一列就没有信息量。
 *
 * 值层面归本文件管；**JSON 是否仍然合法**（新加的三块把 runtime 那个对象的收尾
 * 改成了 `}}`，少一个括号就是一份解析不了的报告）归
 * scripts/checks/report_axis_fault_check.py 用 json.loads 判，并带变异对照。
 *
 * 编译与运行（本机 MinGW 建不了：src/audit 的 report 库只在 Linux 上建，见
 * src/audit/CMakeLists.txt）：
 *   gcc -std=c11 -Iinclude -o tmp/test_report_fault_fields \
 *       tests/unit/audit/test_report_fault_fields.c \
 *       src/audit/run_report.c src/audit/report_sections.c src/audit/json_writer.c
 *   ./tmp/test_report_fault_fields /tmp/out.json
 */
#include "emaster/audit/run_report.h"

#include "report_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int failures;

static void check(bool condition, const char *what)
{
    if (!condition)
    {
        ++failures;
        printf("  [不符] %s\n", what);
    }
}

/* 夹具要摆在静态存储上：它装着一份完整报告，含现场环的两份 64 条样本。 */
static report_fixture_t fixture;

/*
 * 取某根轴那一节 JSON。
 *
 * 报告里每根轴一个对象，以 `{"position":N,` 开头，所以按这个串切就够——而按"整份
 * 文本里搜一次"判会串轴：轴 2 有 `"recorded":true`，轴 1 判成"有故障"。
 */
static bool axis_section(const char *text, unsigned int position, char *buffer,
                         size_t capacity)
{
    char head[32];
    const char *start;
    const char *end;
    size_t span;

    (void)snprintf(head, sizeof(head), "{\"position\":%u,", position);
    start = strstr(text, head);
    if (start == NULL)
    {
        return false;
    }
    (void)snprintf(head, sizeof(head), "{\"position\":%u,", position + 1U);
    end = strstr(start + 1, head);
    if (end == NULL)
    {
        /* 最后一根轴：到逐轴数组结束为止（`],"axes"` 数组后面接的是 accesses）。 */
        end = strstr(start, "],\"accesses\"");
        if (end == NULL)
        {
            return false;
        }
    }
    span = (size_t)(end - start);
    if (span + 1U > capacity)
    {
        return false;
    }
    memcpy(buffer, start, span);
    buffer[span] = '\0';
    return true;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tmp/report-fault-fixture.json";
    char section[16384];
    FILE *stream;
    char *text;
    long size;
    bool published;

    report_fixture_init(&fixture, path);
    published = emaster_run_report_publish(&fixture.plan, &fixture.report, path,
                                           FIXTURE_ARCHIVE_KEEP);
    if (!published)
    {
        printf("  [不符] 报告发布失败（%s）\n", path);
        return 2;
    }

    stream = fopen(path, "rb");
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (size = ftell(stream)) <= 0L || fseek(stream, 0L, SEEK_SET) != 0)
    {
        printf("  [不符] 报告写出去却读不回来\n");
        return 2;
    }
    text = malloc((size_t)size + 1U);
    if (text == NULL || fread(text, 1U, (size_t)size, stream) != (size_t)size)
    {
        printf("  [不符] 报告读回不全\n");
        return 2;
    }
    text[size] = '\0';
    (void)fclose(stream);

    /* ── 1. 轴 2：判故障的坐标 ─────────────────────────────────────────── */
    printf("轴 2：判故障的坐标\n");
    if (!axis_section(text, 2U, section, sizeof(section)))
    {
        check(false, "找得到轴 2 的对象分界");
    }
    else
    {
        check(strstr(section, "\"recorded\":true") != NULL,
              "轴 2 的 fault.recorded 是 true（与 fault_cycle==0 同义，但要显式写）");
        check(strstr(section, "\"isolated\":false") != NULL,
              "轴 2 的 fault.isolated 是 false（走的是全局停机，不是隔离）");
        check(strstr(section, "\"exchange\":22383,") != NULL,
              "轴 2 的 fault.exchange 是 22383（session->exchange，不是 cycle_count）");
        check(strstr(section, "\"status_word\":\"0x1208\"}") != NULL,
              "轴 2 的 fault.status_word 是 0x1208（判故障那一刻的 6041 原值）");
        check(strstr(section, "\"reason\":20,") != NULL,
              "轴 2 的 fault.reason 是 20 = DRIVE_FAULT");

        /* ── 2. 首次越界时间线 ─────────────────────────────────────────── */
        printf("轴 2：首次越界时间线\n");
        check(strstr(section, "\"enable_lost\":{\"seen\":true,\"exchange\":22380,") != NULL,
              "轴 2 的 enable_lost 是第 22380 拍、seen=true");
        check(strstr(section,
                     "\"fault_bit\":{\"seen\":true,\"exchange\":22383,"
                     "\"status_word\":\"0x1208\"}") != NULL,
              "轴 2 的 fault_bit 是第 22383 拍（故障位与判故障同拍）");
        check(strstr(section, "\"warning\":{\"seen\":false") != NULL,
              "轴 2 的 warning 没出现过：seen 必须是 false（exchange 里的诱饵 7 不算）");
        check(strstr(section, "\"internal_limit\":{\"seen\":false") != NULL,
              "轴 2 的 internal_limit 没出现过：seen 必须是 false");
        check(strstr(section, "\"warning\":{\"seen\":false,\"exchange\":7,") != NULL,
              "轴 2 的 warning.exchange 原样印出 7——判据是 seen，不是 exchange 是否为 0");

        /* ── 4. 运行期 603F ────────────────────────────────────────────── */
        printf("轴 2：运行期 603F\n");
        check(strstr(section,
                     "\"runtime_error_code_read\":true,"
                     "\"runtime_error_code\":\"0x2310\"") != NULL,
              "轴 2 的运行期 603F 是 0x2310、read=true");
        check(strstr(section, "\"runtime_error_code_age_ns\":182000000") != NULL,
              "轴 2 的 603F 带上了'多久以前读到的'");
    }

    /* ── 3. 健康的轴不许凭空长出故障码 ─────────────────────────────────── */
    printf("轴 1：健康轴不许有故障痕迹\n");
    if (!axis_section(text, 1U, section, sizeof(section)))
    {
        check(false, "找得到轴 1 的对象分界");
    }
    else
    {
        check(strstr(section, "\"recorded\":false") != NULL,
              "轴 1 的 fault.recorded 是 false");
        check(strstr(section, "\"enable_lost\":{\"seen\":false") != NULL,
              "轴 1 的 enable_lost 是 seen=false");
        check(strstr(section, "\"fault_bit\":{\"seen\":false") != NULL,
              "轴 1 的 fault_bit 是 seen=false");
        check(strstr(section, "\"warning\":{\"seen\":false") != NULL,
              "轴 1 的 warning 是 seen=false");
        check(strstr(section, "\"internal_limit\":{\"seen\":false") != NULL,
              "轴 1 的 internal_limit 是 seen=false");
        check(strstr(section, "\"runtime_error_code_read\":false") != NULL,
              "轴 1 的 603F 从没读到过：read 必须是 false");
    }

    /* ── 4. 轴 3：读到 0x0000 与从没读到必须不一样 ─────────────────────── */
    printf("轴 3：读到 0x0000 与从没读到分得开\n");
    if (!axis_section(text, 3U, section, sizeof(section)))
    {
        check(false, "找得到轴 3 的对象分界");
    }
    else
    {
        check(strstr(section,
                     "\"runtime_error_code_read\":true,"
                     "\"runtime_error_code\":\"0x0000\"") != NULL,
              "轴 3 的 603F 真读到 0x0000：read=true 且值照印 0x0000");
    }

    /* ── 5. 第四个 AL 坐标：主站退出之后驱动器停在哪一态 ───────────────── */
    printf("关机坐标：逐站读，不是拿一个值照抄给全体\n");
    {
        static const struct
        {
            unsigned int position;
            const char *expected;
            const char *what;
        } cases[] = {
            {1U, "\"shutdown_al_exit\":{\"state\":2,\"status_code\":0}",
             "轴 1 退到了 PRE-OP = 确实离开了 OP"},
            {2U, "\"shutdown_al_exit\":{\"state\":20,\"status_code\":26}",
             "轴 2 停在 SAFE-OP 且带 0x001A = 没退干净"},
            {3U, "\"shutdown_al_exit\":{\"state\":8,\"status_code\":0}",
             "轴 3 还在 OP = 广播没叫动它"},
        };
        size_t case_index;

        for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]);
             ++case_index)
        {
            if (!axis_section(text, cases[case_index].position, section, sizeof(section)))
            {
                check(false, "找得到那一轴的对象分界（第四个 AL 坐标）");
                continue;
            }
            check(strstr(section, cases[case_index].expected) != NULL,
                  cases[case_index].what);
        }
    }

    /* ── 停机来源与归档保留份数 ────────────────────────────────────────── */
    printf("结果级字段\n");
    /* 是 7 而不是 6：6 早被那批游离提交占用（带 tracking_error 的那种形状），
     * 同一版本号下并存两种形状正是这次要避开的。见 report_sections.c 里那段注释。 */
    check(strstr(text, "\"schema_version\":7") != NULL,
          "schema_version 是 7（加了字段就要升版本；6 被游离提交占着）");
    check(strstr(text, "\"stop_requested\":true") != NULL,
          "result 里有 stop_requested");
    check(strstr(text, "\"stop_source\":2") != NULL,
          "stop_source 是 2 = COMMAND（命令口发的停机）");
    check(strstr(text, "\"report_archive_keep\":200") != NULL,
          "deployment 里带上了本次生效的归档保留份数（P12.29）");

    free(text);

    if (failures != 0U)
    {
        printf("报告故障字段：%u 项不符\n", failures);
        return 1;
    }
    printf("报告故障字段：全部通过（夹具写在 %s）\n", path);
    return 0;
}
