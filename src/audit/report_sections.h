#ifndef EMASTER_AUDIT_REPORT_SECTIONS_H
#define EMASTER_AUDIT_REPORT_SECTIONS_H

#include "emaster/bus/control_session.h"
#include "emaster/session/session_plan.h"

#include <stdbool.h>
#include <stdio.h>

/* 写入完整 JSON 根对象；文件创建和原子发布由上层负责。 */
bool emaster_run_report_write(FILE *stream,
                              const emaster_session_plan_t *plan,
                              const emaster_control_session_report_t *report,
                              const char *generated_at_utc);

#endif
