#ifndef EMASTER_TOOL_FINGERPRINT_JSON_H
#define EMASTER_TOOL_FINGERPRINT_JSON_H

#include "emaster/bus/preop_probe.h"
#include "emaster/config/runtime_config.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * 把完整探测报告和本次选定的部署写为 schema 2 JSON 文档。函数不关闭 output；成功返回 0，
 * 参数、报告结构或输出流无效时返回非零。时间与部署均由编排层提供，格式层不读取环境变量、
 * 配置文件或系统时钟。
 */
int emaster_fingerprint_write_json(FILE *output, const emaster_preop_report_t *report,
                                   const emaster_deployment_config_t *deployment,
                                   const char *captured_at_utc);

/* 每个声明支持 CoE 的从站必须至少有一项成功 SDO 读取，否则报告不能作为通过证据。 */
bool emaster_fingerprint_has_sdo_evidence(const emaster_preop_report_t *report);

#endif
