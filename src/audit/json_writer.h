#ifndef EMASTER_AUDIT_JSON_WRITER_H
#define EMASTER_AUDIT_JSON_WRITER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* 以下函数只负责生成合法 JSON，不解释报告或 EtherCAT 语义。 */
bool emaster_json_string(FILE *stream, const char *value);
bool emaster_json_bool(FILE *stream, bool value);
bool emaster_json_u64(FILE *stream, uint64_t value);
bool emaster_json_i64(FILE *stream, int64_t value);
bool emaster_json_hex(FILE *stream, uint64_t value, uint8_t bit_length);
bool emaster_json_raw_hex(FILE *stream, const uint8_t *raw, uint8_t size);

#endif
