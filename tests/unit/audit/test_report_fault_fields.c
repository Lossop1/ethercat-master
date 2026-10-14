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
 * 第四个 AL 坐标 shutdown_al_exit（schema_version 7，跳过 6 的理由见 report_sections.c），
 * 以及 P12.41 补的驱动器自报目标位置 target_position_readback（schema_version 8）：
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
 * tests/checks/report_axis_fault_check.py 用 json.loads 判，并带变异对照。
 *
 * 编译与运行（本机 MinGW 建不了：src/audit 的 report 库只在 Linux 上建，见
 * src/audit/CMakeLists.txt）：
 *   gcc -std=c11 -Iinclude -o tmp/test_report_fault_fields \
 *       tests/unit/audit/test_report_fault_fields.c \
 *       src/audit/run_report.c src/audit/report_sections.c src/audit/json_writer.c \
 *       src/cyclic/histogram.c \
 *       src/diagnostics/error_class.c src/diagnostics/cia402_error.c \
 *       src/diagnostics/al_status.c src/audit/report_error_class.c \
 *       src/setpoint/wire.c
 *   ./tmp/test_report_fault_fields /tmp/out.json
 *
 * 诊断码表那几个源文件是 report_sections.c 要调的，最后那个给拒收原因文本：
 * 漏掉就是链接失败。
 * 这份清单是 tests/checks/report_sources_closure.py 算出来的闭包，与 tests/checks/report_axis_fault_check.py、
 * tests/checks/report_archive_check.py 里的 SOURCES 同源，改动要一起改。
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
    /*
     * 故障现场里的环上实扫台数（schema 18）。夹具本来不建故障现场，这里单为这一列建
     * 一份：环上 10 台而配置 3 轴，正是 2026-10-14 台架的形状。两个数分开写，读者
     * 才分得开"环上少站"与"身份或 PDO 不符"。
     */
    fixture.report.first_runtime_failure.present = true;
    fixture.report.first_runtime_failure.status = EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;
    fixture.report.first_runtime_failure.coordinator_status = -1;
    fixture.report.first_runtime_failure.bus_slave_count = 10;
    fixture.report.first_runtime_failure.axis_count = FIXTURE_AXIS_COUNT;
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

    /* ── 6. 驱动器自报的目标位置（P12.41，schema_version 8）───────────── */
    /*
     * 这一列存在的唯一理由是：**主站写的目标证不了驱动器收到了目标**。所以断言的重点
     * 不是"字段印出来了"，而是"印的是驱动器那一个、不是主站那一个"——两根轴的
     * target_position 与 target_position_readback 被故意设成不同的值。
     */
    printf("驱动器自报的目标位置：与主站写的那个不是一回事\n");
    if (!axis_section(text, 1U, section, sizeof(section)))
    {
        check(false, "找得到轴 1 的对象分界（目标位置读回）");
    }
    else
    {
        check(strstr(section,
                     "\"target_position_readback_read\":true,"
                     "\"target_position_readback\":207180,"
                     "\"target_position_readback_age_ns\":120000000") != NULL,
              "轴 1 的目标位置读回是驱动器报的 207180（不是主站写的 207148）");
        check(strstr(section, "\"target_position\":207148,") != NULL,
              "同一根轴上主站写的 target_position 仍单独印成 207148——两列不许互相顶替");
    }
    if (!axis_section(text, 2U, section, sizeof(section)))
    {
        check(false, "找得到轴 2 的对象分界（目标位置读回）");
    }
    else
    {
        check(strstr(section,
                     "\"target_position_readback_read\":true,"
                     "\"target_position_readback\":1031,") != NULL,
              "轴 2（卡住那根）：主站扫到 38229，驱动器报回来的是 1031");
        check(strstr(section, "\"target_position\":38229,") != NULL,
              "而主站自己写的那个 38229 也照印——'扫了全行程'与'一步没跟'在同一条记录里对得上");
    }
    if (!axis_section(text, 3U, section, sizeof(section)))
    {
        check(false, "找得到轴 3 的对象分界（目标位置读回）");
    }
    else
    {
        check(strstr(section, "\"target_position_readback_read\":false,") != NULL,
              "轴 3 从没读到过：read 必须是 false（诱饵值 955 不算数）");
        check(strstr(section, "\"target_position_readback\":955,") != NULL,
              "轴 3 的那个 955 仍原样印出——判据是 read，不是值看起来像不像真的");
    }

    /* ── 解码层：AL 码与 0x603F 选了哪个、从哪来、叫什么 ───────────────── */
    /*
     * schema_version 10 加的这一层。要钉住的是两件事：
     *   1. 四个键**平铺**在同一层（下面整段连起来断言，嵌一层 {} 就断）；
     *   2. "读到 0x0000"与"从没读到"印得不一样 —— 与慢速遥测那条约定同源（P11.8），
     *      只不过这次的 0 更毒：0x0000 的意思是"驱动器报无故障"，而"没读到"什么
     *      也没说，两者在数字上完全一样。
     */
    printf("解码层\n");
    if (!axis_section(text, 1U, section, sizeof(section)))
    {
        check(false, "找得到轴 1 的对象分界（解码层）");
    }
    else
    {
        check(strstr(section,
                     "\"al_status_code_decoded\":0,\"al_status_code_origin\":0,"
                     "\"al_status_code_text\":\"未读到\","
                     "\"drive_error_code_decoded\":0,\"drive_error_code_origin\":0,"
                     "\"drive_error_code_text\":\"未读到\"") != NULL,
              "轴 1 两个坐标都没读回来：origin=0 且文本印'未读到'，"
              "四个键平铺在同一层（有嵌套就断在这里）");
    }
    if (!axis_section(text, 2U, section, sizeof(section)))
    {
        check(false, "找得到轴 2 的对象分界（解码层）");
    }
    else
    {
        /* 0x2310 = 8976。夹具里这一轴的 0x603F 是运行期读到的。 */
        check(strstr(section,
                     "\"drive_error_code_decoded\":8976,\"drive_error_code_origin\":1,"
                     "\"drive_error_code_text\":\"过电流\"") != NULL,
              "轴 2 读到了 0x603F=0x2310，文本解成'过电流'（origin=1 表示来自运行期轮询）");
    }
    if (!axis_section(text, 3U, section, sizeof(section)))
    {
        check(false, "找得到轴 3 的对象分界（解码层）");
    }
    else
    {
        check(strstr(section,
                     "\"drive_error_code_decoded\":0,\"drive_error_code_origin\":1,"
                     "\"drive_error_code_text\":\"无故障\"") != NULL,
              "轴 3 **读到了 0x0000**：origin=1 且文本'无故障'——"
              "与轴 1 的'未读到'必须分得开，这正是这一层存在的理由");
    }

    /* ── 丢失链计数 ─────────────────────────────────────────────────────── */
    printf("丢失链计数\n");
    if (!axis_section(text, 1U, section, sizeof(section)))
    {
        check(false, "找得到轴 1 的对象分界（丢失链计数）");
    }
    else
    {
        check(strstr(section,
                     "\"error_counters_read\":true,"
                     "\"rx_error_counter\":[0,0,0,0,0,0,0,0],"
                     "\"forwarded_rx_error_counter\":[0,0,0,0],"
                     "\"lost_link_counter\":[0,0,0,3],"
                     "\"lost_link_counter_first\":[0,0,0,2],"
                     "\"lost_link_counter_first_valid\":true") != NULL,
              "轴 1 末端口上电以来 2 次、本轮 3 次：键排在两个 RX 计数器之后（错位就断在这里）");
    }
    if (!axis_section(text, 2U, section, sizeof(section)))
    {
        check(false, "找得到轴 2 的对象分界（丢失链计数）");
    }
    else
    {
        check(strstr(section,
                     "\"lost_link_counter\":[0,5,0,0],"
                     "\"lost_link_counter_first\":[0,5,0,0],"
                     "\"lost_link_counter_first_valid\":true") != NULL,
              "轴 2 首末一样，本轮一次没断——绝对值非零不等于本轮断过");
    }
    if (!axis_section(text, 3U, section, sizeof(section)))
    {
        check(false, "找得到轴 3 的对象分界（丢失链计数）");
    }
    else
    {
        check(strstr(section,
                     "\"error_counters_read\":false,"
                     "\"rx_error_counter\":[0,0,0,0,0,0,0,0],"
                     "\"forwarded_rx_error_counter\":[0,0,0,0],"
                     "\"lost_link_counter\":[7,0,0,0],"
                     "\"lost_link_counter_first\":[0,0,0,0],"
                     "\"lost_link_counter_first_valid\":false") != NULL,
              "轴 3 计数没更新却留着 7：判据是 read，不是值是否为 0");
    }

    /* ── 跟随误差窗与目标偏移上限 ──────────────────────────────────────── */
    /*
     * 主站不再自己判跟随误差，所以 6065 / 6066 这两个读回值就是"驱动器在不在守"
     * 的全部依据；偏移上限则是"目标被拒"这条判据的唯一来源。两处都逐轴，且都必须
     * 用 read 位把"没判"与"值是 0"分开。
     */
    printf("跟随误差窗与偏移上限\n");
    if (!axis_section(text, 1U, section, sizeof(section)))
    {
        check(false, "找得到轴 1 的对象分界（跟随误差窗）");
    }
    else
    {
        check(strstr(section,
                     "\"following_error_window_read\":true,\"following_error_window\":131072,"
                     "\"following_error_timeout_read\":true,\"following_error_timeout\":200,"
                     "\"offset_limit_counts\":114688,\"offset_limit_checked\":true") != NULL,
              "轴 1 窗与超时都读到，偏移上限 114688 且判过");
    }
    if (!axis_section(text, 2U, section, sizeof(section)))
    {
        check(false, "找得到轴 2 的对象分界（跟随误差窗）");
    }
    else
    {
        check(strstr(section,
                     "\"following_error_window_read\":true,\"following_error_window\":4096,"
                     "\"following_error_timeout_read\":false") != NULL,
              "轴 2 只读到窗口、超时没读到：两项的 read 必须各自独立，"
              "绑在一起会在这里断");
        check(strstr(section,
                     "\"offset_limit_counts\":102400,\"offset_limit_checked\":true") != NULL,
              "轴 2 的偏移上限是 102400 而不是 114688——25:1 与 28:1 折算不同，"
              "照抄首轴的值就会在这里断");
    }
    if (!axis_section(text, 3U, section, sizeof(section)))
    {
        check(false, "找得到轴 3 的对象分界（跟随误差窗）");
    }
    else
    {
        check(strstr(section,
                     "\"following_error_window_read\":false,\"following_error_window\":777777,"
                     "\"following_error_timeout_read\":false,\"following_error_timeout\":31,") != NULL,
              "轴 3 两项都没读到却留着诱饵值：判据是 read，不是值看起来像不像真的");
        check(strstr(section,
                     "\"offset_limit_counts\":0,\"offset_limit_checked\":false") != NULL,
              "轴 3 换算用不了：上限为 0 但 checked 必须为假——"
              "'不判'与'上限为零'读起来不能一样");
    }

    /* ── 换算与方向的来源（17）────────────────────────────────────────── */
    printf("换算来源\n");
    if (!axis_section(text, 1U, section, sizeof(section)))
    {
        check(false, "找得到轴 1 的对象分界（换算来源）");
    }
    else
    {
        check(strstr(section,
                     "\"position_scale\":{\"source\":\"configured\",\"read_succeeded\":false,"
                     "\"encoder_increments\":131072") != NULL,
              "轴 1 的换算来自设备档声明：read_succeeded 为假（设备没答）而数值是真的"
              "——这两件事必须分得开，否则'设备答了没有'读不出来");
        check(strstr(section, "\"polarity_source\":\"configured\"") != NULL,
              "轴 1 的方向也来自设备档声明（设备没有 607E）");
    }
    if (!axis_section(text, 2U, section, sizeof(section)))
    {
        check(false, "找得到轴 2 的对象分界（换算来源）");
    }
    else
    {
        check(strstr(section,
                     "\"position_scale\":{\"source\":\"device\",\"read_succeeded\":true,") != NULL,
              "轴 2 的换算来自设备读回");
        check(strstr(section, "\"polarity_source\":\"device\"") != NULL,
              "轴 2 的方向来自设备读回");
    }

    /* ── 停机来源与归档保留份数 ────────────────────────────────────────── */
    printf("结果级字段\n");
    /* 是 12 而不是 6/8：6 早被那批游离提交占用（带 tracking_error 的那种形状），
     * 同一版本号下并存两种形状正是这次要避开的。见 report_sections.c 里那段注释。
     * 升到 10 的理由：多了分类层，报告怎么被解读变了——旧脚本以为 status_code 就是
     * 结论，而它现在只是输入。升到 11 的理由：逐轴多了首次不符当下的 1C32/1C33
     * 计数器（first_mismatch_sm2/sm3），旧报告里这一层不存在。升到 12 的理由：
     * shutdown_disable 的键改名——旧报告里叫 verified 的那一列读起来像"已失能"。
     * 升到 13 的理由：逐轴多了丢失链计数（ESC 0x0310）。升到 14 的理由：逐轴多了
     * 偏移上限（offset_limit_counts / offset_limit_checked）与驱动器自己的
     * 6065/6066 读回值，结果级多了被拒目标的计数与现场以及设点通道的计数，
     * 同时删掉了运动方案的 max_step_millidegrees。升到 15 的理由：逐轴的 pdo 里多了
     * 按设备实际布局建立映像时的差异清单（adopted_device_layout 与
     * device_mapping_differences）。升到 16 的理由：逐轴的 operation 里多了同步类型的
     * 实际取值与"是否被设备顶替"（sm_sync_type_substituted 与 *_used）——报告印方案值
     * 而设备停在另一档，就是一处安静的谎。升到 17 的理由：逐轴的位置换算多了来源
     * （position_scale.source）与方向来源（polarity_source）——设备读回的与设备档声明
     * 的在这份报告里必须分得开，只判数值判不出"这台设备到底答没答"。升到 18 的理由：
     * 故障现场里只写配置轴数，于是"环上少站"与"身份或 PDO 不符"读出来是同一句
     * TOPOLOGY_MISMATCH——2026-10-14 台架就是这样（报告写轴数=12，环上只有 10 台，
     * 凭那一份报告断不出来）。 */
    check(strstr(text, "\"schema_version\":18") != NULL,
          "schema_version 是 18（故障现场要写明环上实扫台数，与配置轴数分开）");
    check(strstr(text, "\"bus_slave_count\":10") != NULL,
          "故障现场带上环上实扫台数：环上 10 台、配置 3 轴，读到这一列才知道是少站");
    check(strstr(text,
                 "\"error_class\":\"DRIVE_FAULT\",\"error_class_text\":\"驱动器自报故障\","
                 "\"error_class_evidence_no_frame\":false,"
                 "\"error_class_evidence_sync\":false,"
                 "\"error_class_silent_axes\":0,\"error_class_sync_axes\":0,") != NULL,
          "结果里带上了类别与证据位（夹具是 DRIVE_FAULT，两条通信证据都为假）");
    check(strstr(text, "\"error_class_first_silent_axis\":0,") != NULL &&
              strstr(text, "\"error_class_first_sync_axis\":0") != NULL,
          "两个'第一根'轴号也落盘（没有时取 0）");
    check(strstr(text, "\"stop_requested\":true") != NULL,
          "result 里有 stop_requested");
    check(strstr(text, "\"stop_source\":2") != NULL,
          "stop_source 是 2 = COMMAND（命令口发的停机）");
    check(strstr(text, "\"report_archive_keep\":200") != NULL,
          "deployment 里带上了本次生效的归档保留份数（P12.29）");
    /*
     * 被拒目标（14 起）。三个字段一起读才是完整的："拒过几条"、"最后一次是哪根轴"、
     * "最后一次超出去多少"。计数 3 而轴号 2：轴号从 0 起，2 与"从来没拒过"的 0
     * 长得不一样，正是这一列要分开的东西。
     */
    check(strstr(text,
                 "\"target_rejected_count\":3,\"target_rejection_axis\":2,"
                 "\"target_rejection_offset_counts\":150000,") != NULL,
          "被拒目标：计数 / 最后轴号（0 起）/ 最后的偏移量三者都在");
    /*
     * 设点通道（14 起）。整串比对把键名、键序与实参绑定一起钉住——格式串与实参错位
     * 是这份文件里反复出现的形状。四个 64 位计数都给成互不相同的值，错位了就对不上；
     * 拒收原因写成文本（"未被拒"是 NONE 的文本，与"这一列没写"长得一样，所以夹具
     * 取的是 MODE_MISMATCH）。
     */
    check(strstr(text,
                 "\"setpoint_enabled\":true,\"setpoint_records_accepted\":12345,"
                 "\"setpoint_records_rejected\":7,\"setpoint_sequence_gaps\":3,"
                 "\"setpoint_last_sequence\":12349,"
                 "\"setpoint_last_client_time_ns\":998877665544332211,"
                 "\"setpoint_last_record_age_ns\":4200000,"
                 "\"setpoint_last_reject_axis\":4,"
                 "\"setpoint_last_reject_reason\":\"某轴的运行模式与本部署不符\",") != NULL,
          "设点通道：开关 / 收 / 拒 / 缺口 / 序号 / 客户端时标 / 年龄 / 拒收轴号与原因都在");

    /* ── 停机失能核对与它用的判据 ──────────────────────────────────────── */
    /*
     * 一处整串比对把三件事一起钉住：键名（叫 verified 就会在这里断）、键序
     * （格式串与实参错位是这份文件里反复出现的形状，见上面 still_enabled 那条）、
     * 以及判据枚举到 ASCII 键的映射。值层面另外靠 attempted=2 露绑定错误。
     */
    printf("停机失能核对\n");
    check(strstr(text,
                 "\"shutdown_disable\":{\"attempted\":2,\"statusword_bit2_clear\":1,"
                 "\"statusword_bit2_set\":1,\"statusword_unreadable\":1},"
                 "\"shutdown_stop_criterion\":\"standard_cia402\"") != NULL,
          "失能核对三个键说的是读到了什么（不是'已失能'），判据键是 standard_cia402");

    free(text);

    if (failures != 0U)
    {
        printf("报告故障字段：%u 项不符\n", failures);
        return 1;
    }
    printf("报告故障字段：全部通过（夹具写在 %s）\n", path);
    return 0;
}
