#ifndef EMASTER_TOOL_FINGERPRINT_PLAN_H
#define EMASTER_TOOL_FINGERPRINT_PLAN_H

#include "emaster/bus/preop_probe.h"
#include "emaster/catalog/slave_profile.h"

#include <stddef.h>

/*
 * 根据当前编译目录中的全部设备配置生成一次有界 SDO 读取计划。调用者提供存储空间，
 * 计划包含通用身份/诊断对象以及每个设备配置声明的 PDO 映射对象，不依赖某个固定型号。
 */
bool emaster_fingerprint_sdo_plan(emaster_sdo_request_t *requests, size_t capacity,
                                  size_t *request_count);

#endif
