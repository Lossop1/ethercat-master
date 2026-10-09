#include "emaster/config/config_reader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 格式：每行一条记录，key 与 value 之间是一个制表符。空行与以 '#' 起头的行忽略。
 * value 里允许出现制表符（只按第一个制表符切分），这样以后再放宽也不用换格式。
 */

/* 最近一次失败。加载是单线程一次的，所以进程级一份就够 —— 见头文件里的说明。 */
static char g_last_key[256];
static bool g_have_key = false;
static const char *g_last_why = NULL;

/*
 * 把键**拷进来**，不存调用方的指针。
 * 键多半是生成代码里栈上的临时缓冲（char nested[...]），函数一返回就悬垂 ——
 * 第一版直接存了指针，结果打印出来的是乱码。这个错和"键名拼错"不是一回事：
 * 拼错至少还指向一块有效内存。
 */
static void note_failure(const char *key, const char *why)
{
    if (key != NULL)
    {
        size_t length = strlen(key);

        if (length >= sizeof(g_last_key))
        {
            length = sizeof(g_last_key) - 1U;
        }
        memcpy(g_last_key, key, length);
        g_last_key[length] = '\0';
        g_have_key = true;
    }
    if (why != NULL)
    {
        g_last_why = why;
    }
}

void emaster_config_reader_note_failure(const char *why)
{
    note_failure(NULL, why);
}

const char *emaster_config_reader_last_failure(const char **why)
{
    if (why != NULL)
    {
        *why = g_last_why;
    }
    return g_have_key ? g_last_key : NULL;
}

static bool grow_lines(emaster_config_reader_t *reader, size_t needed)
{
    size_t capacity = reader->line_count == 0U ? 64U : reader->line_count;
    size_t *grown;

    while (capacity < needed)
    {
        capacity *= 2U;
    }
    grown = (size_t *)realloc(reader->line_starts, capacity * sizeof(*grown));
    if (grown == NULL)
    {
        return false;
    }
    reader->line_starts = grown;
    return true;
}

bool emaster_config_reader_open(emaster_config_reader_t *reader, const char *path)
{
    FILE *stream;
    long length;
    size_t read_total;
    size_t index;

    if (reader == NULL || path == NULL)
    {
        return false;
    }
    memset(reader, 0, sizeof(*reader));

    stream = fopen(path, "rb");
    if (stream == NULL)
    {
        return false;
    }
    if (fseek(stream, 0L, SEEK_END) != 0)
    {
        (void)fclose(stream);
        return false;
    }
    length = ftell(stream);
    if (length <= 0L || fseek(stream, 0L, SEEK_SET) != 0)
    {
        (void)fclose(stream);
        return false;
    }
    reader->size = (size_t)length;
    /* 多要一个字节放结尾的 NUL：下面解析时直接就地插 NUL，不额外拷贝。 */
    reader->buffer = (char *)malloc(reader->size + 1U);
    if (reader->buffer == NULL)
    {
        (void)fclose(stream);
        return false;
    }
    read_total = fread(reader->buffer, 1U, reader->size, stream);
    (void)fclose(stream);
    if (read_total != reader->size)
    {
        free(reader->buffer);
        memset(reader, 0, sizeof(*reader));
        return false;
    }
    reader->buffer[reader->size] = '\0';

    /* 扫一遍记下每条记录的起点，并把行尾的 CR/LF 和注释行处理掉。 */
    for (index = 0U; index < reader->size;)
    {
        size_t line_start = index;
        size_t line_end;

        while (index < reader->size && reader->buffer[index] != '\n')
        {
            ++index;
        }
        line_end = index;
        if (index < reader->size)
        {
            ++index; /* 跳过 '\n' */
        }
        while (line_end > line_start &&
               (reader->buffer[line_end - 1U] == '\r' || reader->buffer[line_end - 1U] == ' '))
        {
            --line_end;
        }
        reader->buffer[line_end] = '\0';
        if (line_end == line_start || reader->buffer[line_start] == '#')
        {
            continue;
        }
        if (reader->line_count % 64U == 0U && !grow_lines(reader, reader->line_count + 1U))
        {
            emaster_config_reader_close(reader);
            return false;
        }
        reader->line_starts[reader->line_count] = line_start;
        reader->line_count += 1U;
    }
    return true;
}

void emaster_config_reader_close(emaster_config_reader_t *reader)
{
    if (reader == NULL)
    {
        return;
    }
    free(reader->buffer);
    free(reader->line_starts);
    memset(reader, 0, sizeof(*reader));
}

const char *emaster_config_reader_raw(const emaster_config_reader_t *reader,
                                      const char *key)
{
    size_t key_length;
    size_t index;

    if (reader == NULL || key == NULL)
    {
        return NULL;
    }
    key_length = strlen(key);
    /* 不论找没找到都记住这个键：范围检查发生在取值成功之后，那时它需要知道
       "刚才问的是哪个键"，而那条路径不走下面的失败分支。 */
    note_failure(key, NULL);
    for (index = 0U; index < reader->line_count; ++index)
    {
        const char *line = reader->buffer + reader->line_starts[index];
        if (strncmp(line, key, key_length) == 0 && line[key_length] == '\t')
        {
            return line + key_length + 1U;
        }
    }
    note_failure(key, "缺键");
    return NULL;
}

bool emaster_config_reader_string(const emaster_config_reader_t *reader,
                                  const char *key, const char **out)
{
    const char *value = emaster_config_reader_raw(reader, key);
    if (value == NULL || out == NULL)
    {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_i64(const char *text, int64_t *out)
{
    char *end = NULL;
    long long parsed;

    if (text == NULL || *text == '\0')
    {
        return false;
    }
    errno = 0;
    parsed = strtoll(text, &end, 0); /* base 0：允许 0x 前缀和十进制 */
    if (errno != 0 || end == text || *end != '\0')
    {
        note_failure(NULL, "值不可解析（不是合法整数）");
        return false;
    }
    *out = (int64_t)parsed;
    return true;
}

bool emaster_config_reader_i64(const emaster_config_reader_t *reader,
                               const char *key, int64_t *out)
{
    const char *raw = emaster_config_reader_raw(reader, key);

    if (raw == NULL)
    {
        return false; /* 缺键已在 raw 里记过 */
    }
    if (out == NULL || !parse_i64(raw, out))
    {
        note_failure(key, "值不可解析（不是合法整数）");
        return false;
    }
    return true;
}

bool emaster_config_reader_u32(const emaster_config_reader_t *reader,
                               const char *key, uint32_t *out)
{
    int64_t value;
    if (out == NULL || !parse_i64(emaster_config_reader_raw(reader, key), &value))
    {
        return false;
    }
    if (value < 0 || value > (int64_t)UINT32_MAX)
    {
        note_failure(NULL, "值超出 uint32 范围");
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

bool emaster_config_reader_i32(const emaster_config_reader_t *reader,
                               const char *key, int32_t *out)
{
    int64_t value;
    if (out == NULL || !parse_i64(emaster_config_reader_raw(reader, key), &value))
    {
        return false;
    }
    if (value < INT32_MIN || value > INT32_MAX)
    {
        note_failure(NULL, "值超出 int32 范围");
        return false;
    }
    *out = (int32_t)value;
    return true;
}

bool emaster_config_reader_size(const emaster_config_reader_t *reader,
                                const char *key, size_t *out)
{
    int64_t value;
    if (out == NULL || !parse_i64(emaster_config_reader_raw(reader, key), &value))
    {
        return false;
    }
    if (value < 0)
    {
        note_failure(NULL, "计数不能为负");
        return false;
    }
    *out = (size_t)value;
    return true;
}

bool emaster_config_reader_bool(const emaster_config_reader_t *reader,
                                const char *key, bool *out)
{
    const char *value = emaster_config_reader_raw(reader, key);
    if (value == NULL || out == NULL)
    {
        return false;
    }
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
    {
        *out = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
    {
        *out = false;
        return true;
    }
    note_failure(NULL, "布尔值只认 true/false/1/0");
    return false;
}
