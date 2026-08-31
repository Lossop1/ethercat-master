#ifndef EMASTER_TOOL_FINGERPRINT_PLAN_H
#define EMASTER_TOOL_FINGERPRINT_PLAN_H

#include "emaster/bus/preop_probe.h"

#include <stddef.h>

/* 返回只读 SDO 请求表；其生命周期覆盖整个进程，调用者不得释放或修改。 */
const emaster_sdo_request_t *emaster_fingerprint_sdo_plan(size_t *request_count);

#endif
