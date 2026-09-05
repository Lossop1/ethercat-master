#ifndef EMASTER_MASTER_CONSOLE_H
#define EMASTER_MASTER_CONSOLE_H

#include "emaster/bus/control_session.h"

/* 展示层只格式化会话事实，正文来自消息目录，不能发起任何总线访问。 */
void emaster_master_console_result(const emaster_session_plan_t *plan,
                                   const emaster_control_session_report_t *report, bool published);

#endif
