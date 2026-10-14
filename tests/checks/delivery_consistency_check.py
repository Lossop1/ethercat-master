#!/usr/bin/env python3
"""交付面的一致性：文档说的、代码做的、CI 跑的，三样必须对得上（离线）。

用法:
    python3 tests/checks/delivery_consistency_check.py
    python3 tests/checks/delivery_consistency_check.py --mutate   # 证明每一条都抓得住

为什么单独一条。上面每一件事本身都不出声——坏掉的样子不是报错，是**说了一句不真的话**：

  * README 的上手流程漏了 `sudo`。照着走的人在非 root 下看到"ecx_init 失败，网卡名对不对"，
    于是一直在查网线，查不到。
  * 文档声称能吃 `.eds`，工具只认 `.xml`。客户按文档把 EDS 递进去，拿到一个裸的解析回溯。
  * 文档互相引用一个已经不存在的文件。点过去 404，读者不知道该信哪句。
  * `recover_axis` 有枚举、有处理分支、解析器里没有分支——从套接字永远发不出去，
    而 README 把它列成可用命令。
  * 已经删掉的配置字段又从别处长回来（`max_step_millidegrees` 这类）。回来的时候
    没有任何一步会红。
  * 消息资源的三个副本（C 枚举、生成器清单、中文资源）只要少对上一处，要么编译不过，
    要么某一条文本在运行期是空串——后者更坏，它不出声。
  * CI 与它的本机复刻（`tests/checks/ci_replay.sh`）漂开。两者是复制关系，一边加了检查另一边没加，
    "本机跑过了"就不再等于"CI 会绿"。

这几条的共同点：都是**声明**与**实现**之间的缝，不在任何一条已有的编译或测试路径上。
所以这里一次性把它们钉住，`--mutate` 逐条证明这个检查抓得住，不是摆设。

本文件里的"坏值"都是拼出来的（见 _needle）：直接写全名的话，下面那条"死字段必须消失"
会抓住本文件自己——靠豁免自己通过的检查不再是检查。
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# 扫描范围：交付面的文本。external/ 是并入的第三方源码，evidence/ 与 docs/archive/
# 是历史记录（只读不改，里面留着的旧引用正是它该有的样子），其余三项是产物：
# build/ 与 runtime/ 里的东西是跑出来的（runtime/reports 有几百 MB 的运行报告），
# tmp/ 是本机的临时脚本。这几处都不该被当成"项目说的话"。
SKIP_DIRS = {".git", "build", "__pycache__", "external", "evidence", "archive", "tmp", "runtime"}
TEXT_SUFFIXES = (".md", ".c", ".h", ".py", ".sh", ".yml", ".json", ".txt", ".cmake")

USAGE_TEXT_END = "EMASTER_TEXT_COUNT"


def _needle(*pieces: str) -> str:
    """把标识符拼出来。写全名会让本文件自己撞上"死字段必须消失"那条规则。"""
    return "".join(pieces)


# 已删除、不许长回来的配置字段。左边那个是部署级的死字段，右边那个是单步限幅。
DEAD_FIELDS = (
    _needle("position_target_max_", "following_error_counts"),
    _needle("max_step_", "millidegrees"),
)

# 文档里提到 EDS 时，同一段话里必须有一句"读不了"。
EDS_NEGATIONS = ("读不了", "不收", "不认", "不是 ESI", "只读 ESI")


class Tree:
    """按仓库相对路径读文件；变异对照往 overrides 里塞改过的文本，绝不碰磁盘。

    overrides 的值为 None 表示"这个文件不存在"。
    """

    def __init__(self, overrides=None):
        self._overrides = dict(overrides or {})

    def read(self, relpath):
        if relpath in self._overrides:
            return self._overrides[relpath]
        with open(os.path.join(ROOT, relpath), encoding="utf-8", errors="replace") as handle:
            return handle.read()

    def exists(self, relpath):
        if relpath in self._overrides:
            return self._overrides[relpath] is not None
        return os.path.exists(os.path.join(ROOT, relpath))

    def paths(self, suffixes=TEXT_SUFFIXES):
        found = []
        for base, dirs, files in os.walk(ROOT):
            dirs[:] = [name for name in dirs if name not in SKIP_DIRS]
            for name in files:
                if name.endswith(suffixes):
                    found.append(os.path.relpath(os.path.join(base, name), ROOT).replace("\\", "/"))
        return sorted(found)


def check_readme_scan_needs_sudo(tree, failures):
    """README 里调 emaster-scan 的那行必须带 sudo：不是 root 时它必然打不开网卡。"""
    text = tree.read("README.md")
    invocations = [line.strip() for line in text.splitlines()
                   if "build/apps/diagnose_slave/emaster-scan" in line]
    if not invocations:
        failures.append("README 里找不到 emaster-scan 的调用行——上手流程改了？")
        return
    for line in invocations:
        if not line.startswith("sudo "):
            failures.append("README 里的扫描没有 sudo：%s" % line)


def check_no_eds_claim(tree, failures):
    """文档不许在"能不能读 EDS"上留一句含糊的话。

    按**段落**判，不按行判：一句"有些厂商只给 EDS"和紧跟着的"本项目读不了"本来是两行。
    分开判会把正确的话判成错的。
    """
    for relpath in tree.paths((".md",)):
        if relpath.startswith("config/") and relpath != "config/README.md":
            continue
        text = tree.read(relpath)
        for index, paragraph in enumerate(re.split(r"\n\s*\n", text)):
            if "EDS" not in paragraph and ".eds" not in paragraph:
                continue
            if not any(negation in paragraph for negation in EDS_NEGATIONS):
                first = paragraph.strip().splitlines()[0][:80]
                failures.append(
                    "%s 第 %d 段提到 EDS 却没说不读它：%s" % (relpath, index + 1, first))


def check_referenced_docs_exist(tree, failures):
    """被引用的文档必须存在。只判两种指法，避免把脚本里的路径片段也算进来：
    markdown 链接的目标，以及正文里带 `docs/` 前缀的引用。
    """
    link_pattern = re.compile(r"\]\(([^)\s]+\.md)(?:#[^)]*)?\)")
    docs_pattern = re.compile(r"\b(docs/[A-Za-z0-9_./-]+\.md)")
    for relpath in tree.paths():
        text = tree.read(relpath)
        targets = set(link_pattern.findall(text)) | set(docs_pattern.findall(text))
        for target in sorted(targets):
            if target.startswith(("http://", "https://")):
                continue
            # 链接按引用文件所在目录解析，`docs/...` 那类按仓库根解析；两处命中其一即可。
            here = os.path.normpath(os.path.join(os.path.dirname(relpath), target))
            if not (tree.exists(here.replace("\\", "/")) or tree.exists(target)):
                failures.append("%s 引用了不存在的 %s" % (relpath, target))


def check_recover_axis_wired(tree, failures):
    """有枚举、有处理分支、解析器里也得有分支。三处缺一处，这条命令从套接字就到不了。"""
    header = tree.read("include/emaster/bus/command_server.h")
    if "EMASTER_COMMAND_RECOVER_AXIS," not in header:
        failures.append("命令枚举里没有 RECOVER_AXIS")
        return
    parser = tree.read("src/bus/io/command_server.c")
    if '"recover_axis "' not in parser or "EMASTER_COMMAND_RECOVER_AXIS;" not in parser:
        failures.append("命令解析器里没有 recover_axis 的分支（枚举与处理分支都在，也发不出去）")
    handler = tree.read("src/bus/session/session_control.c")
    if "case EMASTER_COMMAND_RECOVER_AXIS:" not in handler:
        failures.append("会话语义里没有 RECOVER_AXIS 的处理分支")


def _is_comment_line(line):
    stripped = line.strip()
    return stripped.startswith(("#", "//", "*", "/*"))


def check_dead_fields_gone(tree, failures):
    """已删的字段不许长回来。

    注释里提到名字是允许的——那正是"删了什么、为什么删"该留的地方，把历史一并禁掉
    只会逼人删掉解释。禁的是名字回到**活代码**里的位置。
    """
    for relpath in tree.paths():
        for number, line in enumerate(tree.read(relpath).splitlines(), start=1):
            for field in DEAD_FIELDS:
                if field in line and not _is_comment_line(line):
                    failures.append("%s:%d 已删字段又回来了：%s" % (relpath, number, field))


def check_messages_three_way(tree, failures):
    """消息资源的三个副本必须逐项对上：C 枚举、生成器清单、中文资源。

    少一处是两个不同的坏法：枚举多一项而资源少一条 → 编译不过（还算吵）；资源多一条而
    枚举少一项 → 生成器拒收，构建失败；枚举多一项而生成器清单少一条 → 那一条的文本
    在运行期是空串，**不出声**。三种都不该发生，所以一次判齐。
    """
    header = tree.read("include/emaster/messages.h")
    body = header.split("typedef enum", 1)[-1].split(USAGE_TEXT_END, 1)[0]
    from_enum = {name.lower() for name in re.findall(r"EMASTER_TEXT_([A-Z0-9_]+)", body)}

    generator = tree.read("tools/gen/generate_messages.py")
    block = generator.split("MESSAGE_KEYS = [", 1)[-1].split("]", 1)[0]
    from_generator = set(re.findall(r'"([a-z0-9_]+)"', block))

    resource = json.loads(tree.read("config/messages/zh_CN.json"))["messages"]
    from_resource = set(resource)

    for label, a, b in (("C 枚举", from_enum, from_generator),
                        ("生成器清单", from_generator, from_resource)):
        only_a = sorted(a - b)
        only_b = sorted(b - a)
        if only_a or only_b:
            failures.append("%s 与另一份对不上：多 %s，少 %s"
                            % (label, only_a or "无", only_b or "无"))


def _invoked_checks(text, pattern):
    """一段 CI 文本里被调用的检查脚本连同参数，按集合比。"""
    return {match.strip() for match in re.findall(pattern, text)}


def check_ci_lockstep(tree, failures):
    """CI 与它的本机复刻必须跑同一批检查。两者是复制关系，漂开就没有意义了。"""
    workflow = tree.read(".github/workflows/ci.yml")
    replay = tree.read("tests/checks/ci_replay.sh")
    pattern = r"(tests/checks/[a-z_]+\.(?:py|sh)[^\"'\n]*)"
    in_workflow = _invoked_checks(workflow, pattern)
    in_replay = _invoked_checks(replay, pattern)
    missing_in_replay = sorted(in_workflow - in_replay)
    missing_in_workflow = sorted(in_replay - in_workflow)
    if missing_in_replay:
        failures.append("ci.yml 跑了而 ci_replay.sh 没跑：%s" % missing_in_replay)
    if missing_in_workflow:
        failures.append("ci_replay.sh 跑了而 ci.yml 没跑：%s" % missing_in_workflow)


CHECKS = (
    check_readme_scan_needs_sudo,
    check_no_eds_claim,
    check_referenced_docs_exist,
    check_recover_axis_wired,
    check_dead_fields_gone,
    check_messages_three_way,
    check_ci_lockstep,
)


def run(tree, announce):
    failures = []
    for check in CHECKS:
        check(tree, failures)
    for failure in failures:
        announce("不合格：%s" % failure)
    announce("不合格 %d 项" % len(failures))
    return failures


def mutations():
    """每条规则一个变异：改过的副本必须被判红，且红的正是那条规则。

    返回值是 (说明, overrides)。
    """
    readme = Tree().read("README.md")
    header = Tree().read("include/emaster/messages.h")
    return [
        ("README 的扫描去掉 sudo",
         {"README.md": readme.replace(
             "sudo build/apps/diagnose_slave/emaster-scan",
             "build/apps/diagnose_slave/emaster-scan", 1)}),
        ("文档声称能吃 EDS",
         {"README.md": readme.replace(
             "**本项目只读 ESI，不收 CANopen 的 EDS。**",
             "**本项目既读 ESI，也读 CANopen 的 EDS。**", 1)}),
        ("引用一个不存在的文档",
         {"README.md": readme.replace(
             "docs/architecture.md", _needle("docs/requirements/", "layering", "-plan.md"), 1)}),
        ("解析器里去掉 recover_axis 分支",
         {"src/bus/io/command_server.c": Tree().read("src/bus/io/command_server.c").replace(
             '"recover_axis "', '"recover_axiz "', 1)}),
        ("死字段回到活代码里",
         {"include/emaster/bus/control_session.h": Tree().read(
             "include/emaster/bus/control_session.h").replace(
                 "uint64_t target_rejected_count;",
                 "uint64_t target_rejected_count;\n    uint64_t %s;" % DEAD_FIELDS[1], 1)}),
        ("消息资源少一条",
         {"config/messages/zh_CN.json": json.dumps(
             {**json.loads(Tree().read("config/messages/zh_CN.json")),
              "messages": {key: value for key, value in json.loads(
                  Tree().read("config/messages/zh_CN.json"))["messages"].items()
                  if key != "control_session_match"}}, ensure_ascii=False)}),
        ("C 枚举多一项而生成器清单没跟上",
         {"include/emaster/messages.h": header.replace(
             "    EMASTER_TEXT_CONTROL_SESSION_MATCH,",
             "    EMASTER_TEXT_CONTROL_SESSION_MATCH,\n"
             "    EMASTER_TEXT_CONTROL_SESSION_NEVER_REGISTERED,", 1)}),
        ("CI 里跑了某条检查而本机复刻没跑",
         {"tests/checks/ci_replay.sh": re.sub(
             r"^.*tests/checks/device_facts_check\.py.*\n", "",
             Tree().read("tests/checks/ci_replay.sh"), count=1, flags=re.MULTILINE)}),
    ]


def main(argv):
    if argv and argv[0] == "--mutate":
        caught = 0
        variants = mutations()
        for label, overrides in variants:
            failures = run(Tree(overrides), lambda line: None)
            if failures:
                caught += 1
            else:
                print("变异没被抓住：%s" % label)
        if caught == len(variants):
            print("\n%d 个变异全部被抓住" % caught)
            return 0
        print("\n%d/%d 个变异被抓住——这个检查有漏" % (caught, len(variants)))
        return 1

    failures = run(Tree(), print)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
