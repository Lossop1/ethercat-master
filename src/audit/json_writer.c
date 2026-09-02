#include "json_writer.h"

#include <inttypes.h>

bool emaster_json_string(FILE *stream, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    if (stream == NULL || value == NULL || fputc('"', stream) == EOF)
    {
        return false;
    }
    while (*cursor != '\0')
    {
        switch (*cursor)
        {
            case '"':
                if (fputs("\\\"", stream) == EOF) return false;
                break;
            case '\\':
                if (fputs("\\\\", stream) == EOF) return false;
                break;
            case '\b':
                if (fputs("\\b", stream) == EOF) return false;
                break;
            case '\f':
                if (fputs("\\f", stream) == EOF) return false;
                break;
            case '\n':
                if (fputs("\\n", stream) == EOF) return false;
                break;
            case '\r':
                if (fputs("\\r", stream) == EOF) return false;
                break;
            case '\t':
                if (fputs("\\t", stream) == EOF) return false;
                break;
            default:
                if (*cursor < UINT8_C(0x20))
                {
                    if (fprintf(stream, "\\u%04X", (unsigned int)*cursor) < 0)
                    {
                        return false;
                    }
                }
                else if (fputc((int)*cursor, stream) == EOF)
                {
                    return false;
                }
                break;
        }
        ++cursor;
    }
    return fputc('"', stream) != EOF;
}

bool emaster_json_bool(FILE *stream, bool value)
{
    return stream != NULL && fputs(value ? "true" : "false", stream) != EOF;
}

bool emaster_json_u64(FILE *stream, uint64_t value)
{
    return stream != NULL && fprintf(stream, "%" PRIu64, value) >= 0;
}

bool emaster_json_i64(FILE *stream, int64_t value)
{
    return stream != NULL && fprintf(stream, "%" PRId64, value) >= 0;
}

bool emaster_json_hex(FILE *stream, uint64_t value, uint8_t bit_length)
{
    unsigned int digits = ((unsigned int)bit_length + 3U) / 4U;

    if (stream == NULL || digits == 0U || digits > 16U)
    {
        return false;
    }
    return fprintf(stream, "\"0x%0*" PRIX64 "\"", (int)digits, value) >= 0;
}

bool emaster_json_raw_hex(FILE *stream, const uint8_t *raw, uint8_t size)
{
    uint8_t index;

    if (stream == NULL || (size > 0U && raw == NULL) || fputc('"', stream) == EOF)
    {
        return false;
    }
    for (index = 0U; index < size; ++index)
    {
        if (fprintf(stream, "%02X", (unsigned int)raw[index]) < 0)
        {
            return false;
        }
    }
    return fputc('"', stream) != EOF;
}
