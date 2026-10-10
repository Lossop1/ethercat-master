#!/usr/bin/env python3
"""用 apps/fingerprint/fingerprint.schema.json 校验入库的装置指纹。

为什么要有它。那份 schema 2026-10 之前一直没人在执行：没有任何代码读它，也没有任何
工具 import jsonschema。一份没人执行的规范会安静地和它的生产者脱节——而指纹是"这台
机器当时是什么样"的唯一记录，脱节之后它证明的东西就不再是它声称的东西。

判据从 schema 里读，不写死在这里。schema 改了，本检查跟着改；两边不会各说各话。

只支持这份 schema 实际用到的 JSON Schema 构造：type / required / additionalProperties /
properties / const / enum / minLength / minimum / maximum / minItems / maxItems / items /
pattern / $ref(本地) / allOf + if/then/not。**不支持的一律报错退出**，不静默放过——
静默放过正是这份检查要消灭的那种失败。
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = os.path.join(ROOT, "apps", "fingerprint", "fingerprint.schema.json")
INSTANCES = os.path.join(ROOT, "evidence", "fingerprints")
SUPPORTED = {"type", "required", "additionalProperties", "properties", "const", "enum",
             "minLength", "minimum", "maximum", "minItems", "maxItems", "items", "pattern",
             "$ref", "allOf", "if", "then", "not", "$schema", "$id", "title", "$defs",
             # 注解类关键字：不影响校验结果，允许出现但不参与判定。
             # 之所以要显式列出来而不是"不认识的都放过"，是因为后者会让一个拼错的约束
             # （比如把 minimum 写成 minumum）安静地失效 —— 那正是本检查要消灭的失败方式。
             "description", "examples", "default", "$comment", "deprecated"}


def _type_ok(value, want):
    if want == "object":
        return isinstance(value, dict)
    if want == "array":
        return isinstance(value, list)
    if want == "string":
        return isinstance(value, str)
    if want == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if want == "boolean":
        return isinstance(value, bool)
    return False


def _resolve(root, ref):
    if not ref.startswith("#/"):
        raise ValueError("只支持本地 $ref，遇到：" + ref)
    node = root
    for part in ref[2:].split("/"):
        node = node[part]
    return node


def validate(instance, schema, root, path="$"):
    """返回错误列表。空列表=通过。"""
    errors = []
    if not isinstance(schema, dict):
        raise ValueError("schema 节点不是对象：" + path)
    unknown = set(schema) - SUPPORTED
    if unknown:
        raise ValueError("schema 里用到本检查不支持的构造 %s（在 %s）" % (sorted(unknown), path))

    if "$ref" in schema:
        return validate(instance, _resolve(root, schema["$ref"]), root, path)

    for sub in schema.get("allOf", []):
        errors += validate(instance, sub, root, path)

    if "if" in schema:
        if not validate(instance, schema["if"], root, path):
            errors += validate(instance, schema.get("then", {}), root, path)

    if "not" in schema:
        if not validate(instance, schema["not"], root, path):
            errors.append("%s：命中了 not 分支" % path)

    if "const" in schema and instance != schema["const"]:
        errors.append("%s：应为 %r，实际 %r" % (path, schema["const"], instance))

    if "enum" in schema and instance not in schema["enum"]:
        errors.append("%s：应为 %r 之一，实际 %r" % (path, schema["enum"], instance))

    if "type" in schema:
        wants = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_type_ok(instance, w) for w in wants):
            errors.append("%s：类型应为 %s，实际 %s" % (path, "/".join(wants), type(instance).__name__))
            return errors

    if isinstance(instance, str):
        if "minLength" in schema and len(instance) < schema["minLength"]:
            errors.append("%s：长度 %d < %d" % (path, len(instance), schema["minLength"]))
        if "pattern" in schema and not re.search(schema["pattern"], instance):
            errors.append("%s：%r 不匹配 %s" % (path, instance, schema["pattern"]))

    if isinstance(instance, int) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            errors.append("%s：%r < 下界 %r" % (path, instance, schema["minimum"]))
        if "maximum" in schema and instance > schema["maximum"]:
            errors.append("%s：%r > 上界 %r" % (path, instance, schema["maximum"]))

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            errors.append("%s：%d 项 < %d" % (path, len(instance), schema["minItems"]))
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            errors.append("%s：%d 项 > %d" % (path, len(instance), schema["maxItems"]))
        if "items" in schema:
            for i, item in enumerate(instance):
                errors += validate(item, schema["items"], root, "%s[%d]" % (path, i))

    if isinstance(instance, dict):
        for key in schema.get("required", []):
            if key not in instance:
                errors.append("%s：缺少必需字段 %s" % (path, key))
        props = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            for key in instance:
                if key not in props:
                    errors.append("%s：多出未声明的字段 %s" % (path, key))
        for key, sub in props.items():
            if key in instance:
                errors += validate(instance[key], sub, root, "%s.%s" % (path, key))
    return errors


def main():
    with open(SCHEMA, encoding="utf-8") as handle:
        schema = json.load(handle)
    files = sorted(glob.glob(os.path.join(INSTANCES, "*.json")))
    if not files:
        print("错误：%s 下没有指纹文件，这条检查就没在判任何东西" % INSTANCES)
        return 1

    failed = 0
    for path in files:
        with open(path, encoding="utf-8") as handle:
            instance = json.load(handle)
        errors = validate(instance, schema, schema)
        name = os.path.basename(path)
        if errors:
            failed += 1
            print("失败 %s" % name)
            for e in errors[:6]:
                print("    " + e)
        else:
            print("通过 %s" % name)

    # 变异对照：合格样本上破一处，必须判红。只判"好样本通过"的检查会奖励一个永远点头的工具。
    with open(files[0], encoding="utf-8") as handle:
        sample = json.load(handle)
    mutations = []
    broken = json.loads(json.dumps(sample)); broken.pop("slave_count", None)
    mutations.append(("删掉必需字段 slave_count", broken))
    broken = json.loads(json.dumps(sample)); broken["schema_version"] = 1
    mutations.append(("schema_version 改成 1", broken))
    broken = json.loads(json.dumps(sample)); broken["behavior"]["pdo_mapping_performed"] = True
    mutations.append(("behavior 里 pdo_mapping_performed 改成 true", broken))
    broken = json.loads(json.dumps(sample)); broken["slaves"][0]["pdo_layout"]["status"] = "partial"
    mutations.append(("某从站的 pdo_layout.status 改成 partial", broken))

    missed = 0
    for label, mutated in mutations:
        if not validate(mutated, schema, schema):
            missed += 1
            print("变异漏判：%s 没被抓住" % label)
    print("变异对照：%d 项，漏判 %d 项" % (len(mutations), missed))

    if failed or missed:
        print("结论：不合格（%d 个文件失败，%d 项变异漏判）" % (failed, missed))
        return 1
    print("结论：%d 个指纹文件全部合格，变异对照全部抓住" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
