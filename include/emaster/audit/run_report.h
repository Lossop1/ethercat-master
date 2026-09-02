#ifndef EMASTER_AUDIT_RUN_REPORT_H
#define EMASTER_AUDIT_RUN_REPORT_H

#include "emaster/bus/control_session.h"
#include "emaster/session/session_plan.h"

#include <stdbool.h>

/*
 * 把会话计划和同一次真实执行产生的报告合并为 JSON，并以临时文件加原子替换发布。
 * 函数不会补默认参数，也不会从说明文档推测未采集的真机值。
 */
bool emaster_run_report_publish(const emaster_session_plan_t *plan,
                                const emaster_control_session_report_t *report,
                                const char *path);

#endif
