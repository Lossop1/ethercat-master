#ifndef EMASTER_TOOL_FINGERPRINT_JSON_H
#define EMASTER_TOOL_FINGERPRINT_JSON_H

#include "emaster/bus/preop_probe.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * 把完整探测报告写为一个 JSON 文档。函数不关闭 output；成功返回 0，参数、报告结构或
 * 输出流无效时返回非零。captured_at_utc 必须由调用者提供，以便测试和证据时间可追溯。
 */
int emaster_fingerprint_write_json(FILE *output, const emaster_preop_report_t *report,
                                   const char *captured_at_utc);

/* 每个声明支持 CoE 的从站必须至少有一项成功 SDO 读取，否则报告不能作为通过证据。 */
bool emaster_fingerprint_has_sdo_evidence(const emaster_preop_report_t *report);

#endif
