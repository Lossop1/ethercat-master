#ifndef EMASTER_TOOL_FINGERPRINT_PLAN_H
#define EMASTER_TOOL_FINGERPRINT_PLAN_H

#include "emaster/bus/preop_probe.h"

#include <stddef.h>

/*
 * 生成与具体设备 PDO 布局无关的身份和诊断对象读取计划。PDO 映射由独立协议模块根据
 * 从站分配对象动态发现，不能混入本固定计划。
 */
bool emaster_fingerprint_sdo_plan(emaster_sdo_request_t *requests, size_t capacity,
                                  size_t *request_count);

#endif
