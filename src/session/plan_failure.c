#include "emaster/session/session_plan.h"

#include "emaster/messages.h"

#include <stdio.h>

/*
 * 计划失败时打印中止原因。单独一个编译单元，为的是让 session_plan.c 保持纯粹：
 * 那里只做判定、不碰 I/O。
 *
 * 为什么这句话要共用一个实现。三个入口（控制会话、DC 准备、实时目标验证）都会因为
 * 同一份运行方案建计划失败，而同一个错误在三个工具里报出三种说法，操作员会以为遇上
 * 了三个问题。所以话术只写一份。
 */
void emaster_session_plan_report_failure(emaster_session_plan_status_t status,
                                         const emaster_session_plan_t *plan,
                                         const char *fallback)
{
    if (plan == NULL || plan->failing_mode_id == NULL)
    {
        if (fallback != NULL)
        {
            fputs(fallback, stderr);
        }
        return;
    }
    if (status == EMASTER_SESSION_PLAN_MODE_NOT_DECLARED)
    {
        fprintf(stderr, emaster_text(EMASTER_TEXT_CONTROL_SESSION_MODE_NOT_DECLARED),
                (unsigned int)(plan->failing_axis_ordinal + 1U),
                (int)plan->failing_mode_value, plan->failing_mode_id);
        return;
    }
    if (status == EMASTER_SESSION_PLAN_MODE_NOT_DRIVABLE)
    {
        fprintf(stderr, emaster_text(EMASTER_TEXT_CONTROL_SESSION_MODE_NOT_DRIVABLE),
                (unsigned int)(plan->failing_axis_ordinal + 1U),
                (int)plan->failing_mode_value, plan->failing_mode_id);
        return;
    }
    if (fallback != NULL)
    {
        fputs(fallback, stderr);
    }
}
