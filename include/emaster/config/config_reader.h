/*
 * 运行时配置读取器（原型）。
 *
 * 为什么不是 JSON
 *   主站是 C11 + SOEM，没有 JSON 解析器。要在 C 里读 JSON 就得自己写一个，
 *   或者引一个第三方单文件库 —— 两者都是"引入复杂变量"。
 *
 * 为什么不是"生成器直接产出二进制 blob"
 *   那条路要求 Python 复刻 C 的结构体布局（字段顺序、对齐、padding）。
 *   改一个结构体字段忘了同步生成器，就会用错偏移静默读错数据，而编译器
 *   不会报错。本仓库已经反复栽在这类静默错误上，所以不走。
 *
 * 走的是哪条
 *   生成器（Python）把 JSON 拍平成「每行一条 key<TAB>value」的扁平文本，并同时
 *   生成一段用 C 写的填充代码：字段名由生成器给，取址和类型检查由编译器做。
 *   于是 Python 不需要知道任何 C 布局，C 侧只需要一个按 key 取值的读取器。
 *
 * 字符串的生命周期 —— 这是接口契约，不是使用习惯
 *   整个文件读进一块内存，字符串就地去掉转义后指向这块内存，不额外分配。
 *
 *   **关掉读取器会让已经填进结构体的每一个字符串变成悬垂指针。**
 *   所以读取器必须活到进程结束：配置读一次、用到退出。
 *
 *   这条不是推测。第一版测试里每族加载完就 emaster_config_reader_close()，
 *   结果表现为"部署引用了不存在的 ID"——因为按 ID 查找时的 strcmp 读的是已经
 *   释放的内存。真正的原因和 ID 毫无关系，而报错信息指向了完全错误的方向。
 *
 *   需要提前释放的用法（比如读一份临时配置再丢掉）当前不支持，也不打算支持：
 *   配置对象的生命周期与进程相同。
 */

#ifndef EMASTER_CONFIG_CONFIG_READER_H
#define EMASTER_CONFIG_CONFIG_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    char *buffer;      /* 文件内容，就地修改（插 NUL） */
    size_t size;
    size_t *line_starts; /* 每条记录在 buffer 里的起点 */
    size_t line_count;
} emaster_config_reader_t;

/* 读文件并建索引。失败返回 false，此时不分配任何内存。 */
bool emaster_config_reader_open(emaster_config_reader_t *reader, const char *path);
void emaster_config_reader_close(emaster_config_reader_t *reader);

/* 按 key 取记录在 buffer 里的值起始位置；找不到返回 NULL。 */
const char *emaster_config_reader_raw(const emaster_config_reader_t *reader,
                                      const char *key);

/* 按键取字符串。缺失或类型不符返回 false；空字符串存在时返回 true 且 *out 为 ""。 */
bool emaster_config_reader_string(const emaster_config_reader_t *reader,
                                  const char *key, const char **out);

bool emaster_config_reader_u32(const emaster_config_reader_t *reader,
                               const char *key, uint32_t *out);
bool emaster_config_reader_i32(const emaster_config_reader_t *reader,
                               const char *key, int32_t *out);
bool emaster_config_reader_i64(const emaster_config_reader_t *reader,
                               const char *key, int64_t *out);
bool emaster_config_reader_bool(const emaster_config_reader_t *reader,
                                const char *key, bool *out);
bool emaster_config_reader_size(const emaster_config_reader_t *reader,
                                const char *key, size_t *out);

/*
 * 最近一次取值失败的键与原因。返回 NULL 表示还没有失败过。
 *
 * 为什么需要它：加载失败的原始报错是"缺键、类型不符，或引用了不存在的 ID"——
 * 三种完全不同的原因混成一句，而改配置的人需要知道的是**哪一个键**。
 * 实测四条故意造坏的配置里，三条报的是同一句话，只有"超容量"那条因为单独写了
 * 提示才指名道姓。那正是这个仓库反复出问题的地方：报错指向的位置不是原因所在。
 *
 * why 回填一个短原因："缺键" / "值不可解析" / "超出范围"。传 NULL 可忽略。
 *
 * 约束：加载过程是单线程一次的，所以这里用一份进程级记录而不是挂在读取器上
 * （读取器的取值接口都收 const 指针，往里写会破坏调用方的契约）。
 */
const char *emaster_config_reader_last_failure(const char **why);

/*
 * 由调用方登记一次失败的原因，键沿用"最近一次问过的键"。
 *
 * 用途是生成代码里的范围检查：读到的值合法地取出来了，但超出字段宽度。
 * 那条路径不经读取器的取值失败分支，所以没有记录 —— 于是加载器只能含糊其辞。
 * 有了这个入口，生成代码只要报一句原因，键由读取器自己记住。
 */
void emaster_config_reader_note_failure(const char *why);

#endif /* EMASTER_CONFIG_CONFIG_READER_H */
