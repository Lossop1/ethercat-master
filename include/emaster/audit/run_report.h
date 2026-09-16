#ifndef EMASTER_AUDIT_RUN_REPORT_H
#define EMASTER_AUDIT_RUN_REPORT_H

#include "emaster/bus/control_session.h"
#include "emaster/session/session_plan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 把 run_report_path 解成绝对路径。已经是绝对路径时原样复制。
 *
 * 存在的理由：相对路径按进程 CWD 解析，而主站不做 chdir——只有 bench 脚本先
 * `cd $REPO` 才成立。"从别处启动就写到别处"这种事故在报告里看不出任何异常，
 * 因为报告本身没写它落在哪。解析一次、写进报告，路径就成了可核对的事实。
 *
 * 只做拼接，不做 `..` 规整：报告路径是部署配置里的常量，不是用户输入。
 */
bool emaster_run_report_resolve_path(const char *path, char *buffer, size_t capacity);

/*
 * 把会话计划和同一次真实执行产生的报告合并为 JSON，并以临时文件加原子替换发布。
 * 函数不会补默认参数，也不会从说明文档推测未采集的真机值。
 *
 * archive_keep > 0 时，同一份报告另存一份带 UTC 时间戳的历史副本到
 * `<path 所在目录>/archive/`，并把该目录下同前缀的旧副本削到剩 archive_keep 份。
 * 0 表示不留历史。加这一层的理由：run_report_path 是覆盖写，同一部署跑一百次
 * 只会剩最后一个文件；而"上个月那次跑成什么样"往往正是要对比的东西。
 *
 * 历史副本用硬链接而不是复制：报告可达上百 MB，复制一份的代价是翻倍写盘和
 * 翻倍占用；内容此后不再修改，链接在语义上是准确的。
 */
bool emaster_run_report_publish(const emaster_session_plan_t *plan,
                                const emaster_control_session_report_t *report,
                                const char *path,
                                uint32_t archive_keep);

#endif
