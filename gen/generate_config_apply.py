#!/usr/bin/env python3
"""把拓扑配置渲染成"填充代码"，而不是静态初始化器。

与 generate_runtime_config.py 的关系
    两者共用同一个模型层（runtime_config_model.topology_values），只换渲染层。
    生成静态数组 + 初始化器那条路会把配置编进二进制；本脚本产出 apply(reader)，
    配置在启动时从扁平文件读入。CMakeLists.txt 调的是本脚本。

为什么由 Python 给字段名、而不是让 Python 算偏移
    生成的每一行都是 "结构体字段 = 读取器取出的值"，取址和类型检查由 C 编译器做。
    Python 完全不需要知道结构体的布局、对齐或 padding。见 config_reader.h 的说明。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_config_model import topology_values  # noqa: E402


def flat_rows(topologies: list[dict[str, Any]]) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = [("topology.count", str(len(topologies)))]
    for ordinal, topology in enumerate(topologies):
        base = f"topology.{ordinal}"
        rows.append((f"{base}.topology_id", topology["id"]))
        rows.append((f"{base}.status", topology["status"]))
        rows.append((f"{base}.slaves.count", str(len(topology["slaves"]))))
        # 模型层把每个从站归一化成 (position, axis_id, profile_id) 三元组并按
        # position 排序 —— 不是字典，别再按字段名取。
        for index, (position, axis_id, profile_id) in enumerate(topology["slaves"]):
            slot = f"{base}.slaves.{index}"
            rows.append((f"{slot}.position", str(position)))
            rows.append((f"{slot}.axis_id", axis_id))
            rows.append((f"{slot}.profile_id", profile_id))
    return rows


TOPOLOGY_CAPACITY = 32
SLAVES_CAPACITY = 64
KEY_CAPACITY = 192


def render(topologies: list[dict[str, Any]]) -> str:
    """拓扑族的填充代码。

    条目数在运行时从 flat 文件读，不再由生成期的 #define 决定 —— 加一份拓扑、
    给一份拓扑加从站，都不需要重新生成，也不需要重编译。数组按静态容量预留，
    超限时加载**响亮地失败**，不截断。
    """
    L: list[str] = [
        "/* 由 gen/generate_config_apply.py 生成，不要手改。 */",
        '#include "emaster/config/generated_apply.h"',
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <string.h>",
        "",
        f"#define EMASTER_TOPOLOGY_CAPACITY {TOPOLOGY_CAPACITY}U",
        f"#define EMASTER_TOPOLOGY_SLAVES_CAPACITY {SLAVES_CAPACITY}U",
        f"#define EMASTER_KEY_CAPACITY {KEY_CAPACITY}U",
        "",
        "static emaster_topology_config_t topologies[EMASTER_TOPOLOGY_CAPACITY];",
        "static emaster_topology_slave_config_t",
        "    slaves[EMASTER_TOPOLOGY_CAPACITY][EMASTER_TOPOLOGY_SLAVES_CAPACITY];",
        "static size_t topology_count = 0U;",
        "",
        "/* 键名逐级拼接；每一级都判截断 —— 截断当失败。否则会拿截短的前缀去查，",
        '   报出来的是"缺键"，与真正的原因（键太长）无关。 */',
        "static bool key_push(char *buffer, size_t capacity, const char *suffix)",
        "{",
        "    size_t used = strlen(buffer);",
        "    int written;",
        "",
        "    if (used + 1U >= capacity)",
        "    {",
        "        return false;",
        "    }",
        '    written = snprintf(buffer + used, capacity - used, ".%s", suffix);',
        "    return written > 0 && (size_t)written < capacity - used;",
        "}",
        "",
        "static bool key_push_index(char *buffer, size_t capacity, size_t index)",
        "{",
        "    size_t used = strlen(buffer);",
        "    int written;",
        "",
        "    if (used + 1U >= capacity)",
        "    {",
        "        return false;",
        "    }",
        '    written = snprintf(buffer + used, capacity - used, ".%zu", index);',
        "    return written > 0 && (size_t)written < capacity - used;",
        "}",
        "",
        "static bool load_topology(const emaster_config_reader_t *reader, size_t ordinal,",
        "                          emaster_topology_config_t *out)",
        "{",
        "    char key[EMASTER_KEY_CAPACITY];",
        "    char nested[EMASTER_KEY_CAPACITY];",
        "    size_t index;",
        "    size_t count;",
        "    uint32_t position = 0U;",
        "",
        '    if (snprintf(key, sizeof(key), "topology.%zu", ordinal) < 0)',
        "    {",
        "        return false;",
        "    }",
        "    memcpy(nested, key, sizeof(nested));",
        '    if (!key_push(nested, sizeof(nested), "topology_id") ||',
        "        !emaster_config_reader_string(reader, nested, &out->topology_id))",
        "    {",
        "        return false;",
        "    }",
        "    memcpy(nested, key, sizeof(nested));",
        '    if (!key_push(nested, sizeof(nested), "status") ||',
        "        !emaster_config_reader_string(reader, nested, &out->status))",
        "    {",
        "        return false;",
        "    }",
        "    memcpy(nested, key, sizeof(nested));",
        '    if (!key_push(nested, sizeof(nested), "slaves") ||',
        '        !key_push(nested, sizeof(nested), "count") ||',
        "        !emaster_config_reader_size(reader, nested, &count))",
        "    {",
        "        return false;",
        "    }",
        "    if (count > (size_t)EMASTER_TOPOLOGY_SLAVES_CAPACITY)",
        "    {",
        "        fprintf(stderr,",
        '                "拓扑 %zu 的从站数 %zu 超过编译期容量 %u\\n",',
        "                ordinal, count, (unsigned)EMASTER_TOPOLOGY_SLAVES_CAPACITY);",
        "        emaster_config_reader_note_failure(\"条目数超过编译期容量\");",
        "        return false;",
        "    }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        emaster_topology_slave_config_t *slave = &slaves[ordinal][index];",
        "",
        "        memcpy(nested, key, sizeof(nested));",
        '        if (!key_push(nested, sizeof(nested), "slaves") ||',
        "            !key_push_index(nested, sizeof(nested), index) ||",
        '            !key_push(nested, sizeof(nested), "position") ||',
        "            !emaster_config_reader_u32(reader, nested, &position))",
        "        {",
        "            return false;",
        "        }",
        "        if (position > UINT16_MAX)",
        "        {",
        '            emaster_config_reader_note_failure("超出 uint16 范围");',
        "            return false;",
        "        }",
        "        slave->position = (uint16_t)position;",
        "        memcpy(nested, key, sizeof(nested));",
        '        if (!key_push(nested, sizeof(nested), "slaves") ||',
        "            !key_push_index(nested, sizeof(nested), index) ||",
        '            !key_push(nested, sizeof(nested), "axis_id") ||',
        "            !emaster_config_reader_string(reader, nested, &slave->axis_id))",
        "        {",
        "            return false;",
        "        }",
        "        memcpy(nested, key, sizeof(nested));",
        '        if (!key_push(nested, sizeof(nested), "slaves") ||',
        "            !key_push_index(nested, sizeof(nested), index) ||",
        '            !key_push(nested, sizeof(nested), "profile_id") ||',
        "            !emaster_config_reader_string(reader, nested, &slave->profile_id))",
        "        {",
        "            return false;",
        "        }",
        "    }",
        "    out->slaves = slaves[ordinal];",
        "    out->slave_count = count;",
        "    return true;",
        "}",
        "",
        "bool emaster_generated_config_apply(const emaster_config_reader_t *reader)",
        "{",
        "    size_t index;",
        "",
        '    if (!emaster_config_reader_size(reader, "topology.count", &topology_count))',
        "    {",
        "        return false;",
        "    }",
        "    if (topology_count > (size_t)EMASTER_TOPOLOGY_CAPACITY)",
        "    {",
        "        fprintf(stderr,",
        '                "拓扑条目 %zu 超过编译期容量 %u\\n",',
        "                topology_count, (unsigned)EMASTER_TOPOLOGY_CAPACITY);",
        "        emaster_config_reader_note_failure(\"条目数超过编译期容量\");",
        "        return false;",
        "    }",
        "    for (index = 0U; index < topology_count; ++index)",
        "    {",
        "        if (!load_topology(reader, index, &topologies[index]))",
        "        {",
        "            return false;",
        "        }",
        "    }",
        "    return true;",
        "}",
        "",
        "size_t emaster_topology_config_count(void)",
        "{",
        "    return topology_count;",
        "}",
        "",
        "const emaster_topology_config_t *emaster_topology_config_at(size_t index)",
        "{",
        "    return index < topology_count ? &topologies[index] : NULL;",
        "}",
        "",
        "const emaster_topology_config_t *emaster_topology_config_by_id(const char *topology_id)",
        "{",
        "    size_t index;",
        "",
        "    if (topology_id == NULL)",
        "    {",
        "        return NULL;",
        "    }",
        "    for (index = 0U; index < topology_count; ++index)",
        "    {",
        "        if (strcmp(topologies[index].topology_id, topology_id) == 0)",
        "        {",
        "            return &topologies[index];",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    return "\n".join(L)


def render_header() -> str:
    """生成配套头文件。

    没有它，生成的三个入口就是"没有前置声明"的函数，在项目的严格告警旗标下
    会撞 -Wmissing-prototypes（实测撞了三条）。
    """
    return "\n".join(
        [
            "/* 由 gen/generate_config_apply.py 生成，不要手改。 */",
            "#ifndef EMASTER_CONFIG_GENERATED_APPLY_H",
            "#define EMASTER_CONFIG_GENERATED_APPLY_H",
            "",
            '#include "emaster/config/config_reader.h"',
            '#include "emaster/config/runtime_config.h"',
            "",
            "#include <stdbool.h>",
            "#include <stddef.h>",
            "",
            "/* 把扁平配置填进静态结构体。缺键或值不可解析时返回 false，不留下半填状态。 */",
            "bool emaster_generated_config_apply(const emaster_config_reader_t *reader);",
            "",
            "/* 公开访问函数：与编译期生成物同名同签名，上层无需改动。 */",
            "",
            "size_t emaster_topology_config_count(void);",
            "const emaster_topology_config_t *emaster_topology_config_at(size_t index);",
            "const emaster_topology_config_t *emaster_topology_config_by_id(const char *topology_id);",
            "",
            "#endif /* EMASTER_CONFIG_GENERATED_APPLY_H */",
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="拓扑配置 -> 填充代码 + 扁平文件")
    parser.add_argument("--topologies", type=Path, required=True, help="config/topologies 目录")
    parser.add_argument("--c-output", type=Path, required=True)
    parser.add_argument("--h-output", type=Path, required=True)
    parser.add_argument("--flat-output", type=Path, required=True)
    args = parser.parse_args()

    documents = [
        json.loads(path.read_text(encoding="utf-8"))
        for path in sorted(args.topologies.glob("*.json"))
    ]
    topologies = topology_values(documents)
    # 一律 LF。这两个产物都是给 Linux 上的 C 程序读的；在 Windows 上生成时
    # Python 默认会按平台转成 CRLF，那样同一份配置在不同机器上字节不同。
    args.c_output.write_text(render(topologies), encoding="utf-8", newline="\n")
    args.h_output.write_text(render_header(), encoding="utf-8", newline="\n")
    rows = flat_rows(topologies)
    args.flat_output.write_text(
        "".join(f"{key}\t{value}\n" for key, value in rows),
        encoding="utf-8",
        newline="\n",
    )
    print(f"拓扑 {len(topologies)} 份，从站 {sum(len(t['slaves']) for t in topologies)} 个")
    print(f"已写出 {args.c_output}")
    print(f"已写出 {args.h_output}")
    print(f"已写出 {args.flat_output}（{len(rows)} 条）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
