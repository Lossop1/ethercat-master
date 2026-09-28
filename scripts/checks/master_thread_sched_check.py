#!/usr/bin/env python3
"""盯住一条不能靠编译器抓的性质：**周期线程之外的每个线程都要自己退出实时调度。**

为什么值得单独一件。主线程在进 OP 之前把自己升成 SCHED_FIFO 并绑核，而 Linux 上
pthread_create 新建的线程默认**继承**创建者的调度策略与优先级——所以"没写这一句"
的后果不是报错，而是新线程悄悄变成 SCHED_FIFO、和周期线程同核同级。同一个核上
同优先级的 SCHED_FIFO 之间不抢占，于是这个新线程只要走进一段不阻塞的执行，就能把
一整拍周期挡住。**出错的方向是"一切看起来正常"**：总线照样通、帧距照样对、状态字
照样对，只有截止期偶尔错过，而那正是最难归因的一类。

2026-09-28 就是这么发现那三个线程的：从台架报告里读 /proc 的实况，观测线程、
命令监听线程、观测服务线程三个的 policy 全是 1、cpus_allowed 全是 "11"，
而它们各自文件顶部都写着"非实时"。代码和它自己的设计说明不一致。

所以这里查的是**结构**，不是行为：src/ 里每一处 pthread_create 的入口函数，
函数体里必须有 emaster_thread_leave_realtime。真实效果由台架报告的 thread_schedstat
来证（那是内核的实况），这一件只负责在**多加一个线程却忘了那一句**的时候叫一声。

跑法：python scripts/checks/master_thread_sched_check.py
      python scripts/checks/master_thread_sched_check.py --mutate
      # ↑ 把一份去掉那句的副本放在临时目录里跑，证明这检查抓得住

--mutate 只在**临时目录的副本**上动手，仓库里的文件一律不碰。
"""

import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

DEMOTION = "emaster_thread_leave_realtime"


class CheckFailed(AssertionError):
    pass


def show(path):
    """给人看的路径。--mutate 的副本落在系统临时目录里，本机是 C: 而仓库在 D:，
    跨盘符时 relpath 直接抛 ValueError——那是讲路径的，不是讲被检查的性质的。"""
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:
        return path


def want(condition, message):
    if not condition:
        raise CheckFailed(message)


def split_arguments(text, start):
    """把 start（左括号的下一位）开始的实参切成顶层逗号分隔的几段。

    要数括号深度是因为 pthread_create 的实参可以带函数调用，也可以换行写——
    台架上那三个调用点里就有一个是换行加三行的写法。
    """
    arguments = []
    depth = 0
    current = []
    index = start
    while index < len(text):
        character = text[index]
        if character in "([{":
            depth += 1
        elif character in ")]}":
            if depth == 0:
                arguments.append("".join(current))
                return arguments, index + 1
            depth -= 1
        elif character == "," and depth == 0:
            arguments.append("".join(current))
            current = []
            index += 1
            continue
        current.append(character)
        index += 1
    raise CheckFailed("括号没配平，解析不了 pthread_create 的实参")


def function_body(text, name):
    """取 `static void *name(` 那个函数的函数体文本；找不到返回 None。"""
    match = re.search(r"static\s+void\s*\*\s*" + re.escape(name) + r"\s*\(", text)
    if match is None:
        return None
    brace = text.find("{", match.end())
    if brace < 0:
        return None
    depth = 0
    index = brace
    while index < len(text):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
        index += 1
    return None


def collect_calls(source_root):
    """返回 [(文件, 行号, 入口函数名)]。"""
    calls = []
    for directory, _dirs, files in os.walk(source_root):
        for name in sorted(files):
            if not name.endswith(".c"):
                continue
            path = os.path.join(directory, name)
            with open(path, "r", encoding="utf-8") as handle:
                text = handle.read()
            for match in re.finditer(r"\bpthread_create\s*\(", text):
                arguments, _end = split_arguments(text, match.end())
                want(len(arguments) == 4,
                     f"{show(path)}：pthread_create 该有 4 个实参，"
                     f"解析到 {len(arguments)} 个——检查器的解析过时了，先修检查器")
                entry = arguments[2].strip()
                line = text.count("\n", 0, match.start()) + 1
                calls.append((path, line, entry, text))
    return calls


def run(source_root):
    calls = collect_calls(source_root)
    want(calls, f"{source_root} 里一个 pthread_create 都没找到——找错目录了？")

    failures = []
    for path, line, entry, text in calls:
        relative = show(path)
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", entry):
            failures.append(f"{relative}:{line}：入口函数不是个简单标识符（{entry}），"
                            f"检查器看不懂，先修检查器")
            continue
        body = function_body(text, entry)
        if body is None:
            failures.append(f"{relative}:{line}：在本文件里找不到 {entry} 的函数定义"
                            f"（跨文件传入？那检查器覆盖不到，先修检查器）")
            continue
        if DEMOTION not in body:
            failures.append(
                f"{relative}:{line}：{entry} 没有调 {DEMOTION}——"
                f"这条线程会继承主线程的 SCHED_FIFO，与周期线程同核同级互不抢占")
        else:
            print(f"ok    {relative}:{line}  {entry} 已退出实时调度")
    return failures


def mutate_into(temp_root):
    """在副本里把那句去掉（模拟"新加线程忘了写"），用来证明检查抓得住。"""
    target = os.path.join(temp_root, "bus", "soem", "session_observer_thread.c")
    with open(target, "r", encoding="utf-8") as handle:
        text = handle.read()
    stripped = re.sub(r"[ \t]*\(void\)" + DEMOTION + r"\([^;]*\);\n", "", text)
    if stripped == text:
        raise CheckFailed(f"变异没生效：{target} 里没找到那句调用")
    with open(target, "w", encoding="utf-8") as handle:
        handle.write(stripped)


def main():
    source_root = os.path.join(ROOT, "src")

    if "--mutate" in sys.argv[1:]:
        print("先在现在的代码上跑一遍：")
        if run(source_root):
            print("\n现在的代码就已经红了，下面证明不了什么——先修好再来。")
            return 2

        print("\n再把去掉那句的一份副本放进临时目录跑（仓库不动）：")
        with tempfile.TemporaryDirectory(prefix="emaster_thread_sched_") as temp:
            copy = os.path.join(temp, "src")
            shutil.copytree(source_root, copy)
            mutate_into(copy)
            failures = run(copy)
        if failures:
            print(f"\n结论：漏掉那句的写法被抓住了（{len(failures)} 处红）——这检查是有效的。")
            return 0
        print("\n结论：漏掉那句的写法全绿，这检查等于白装。")
        return 1

    failures = run(source_root)
    if failures:
        print()
        for failure in failures:
            print(f"FAIL  {failure}")
        print()
        print(f"{len(failures)} 处没退出实时调度")
        return 1
    print()
    print("全部通过：src/ 里每一处 pthread_create 的入口函数都退出了实时调度")
    return 0


if __name__ == "__main__":
    sys.exit(main())
