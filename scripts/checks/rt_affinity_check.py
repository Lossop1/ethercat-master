#!/usr/bin/env python3
"""盯住 tools/rt_affinity.py 的那个缓存：**同一个线程重复问，不该重复绑、重复报。**

为什么值得单独一件：这里省掉的是系统调用，而**系统调用的代价不会自己喊出来**。
缓存失效的时候，程序照样跑、掩码照样是对的、日志上那句"已绑核"看起来也完全正常
——它只是每秒重复 122 遍（2026-09-18 台架实测：桥的日志 657 KB，全是一句话）。
所以出错的方向是"看起来一切正常"，正需要一个会失败的检查钉住。

根因长得也很安静：`_ThreadState` 是 `threading.local` 的子类，而线程隔离是**按实例**
分的。`pin_current_thread()` 里每次 `state = _ThreadState()` 新建一个，读到的就永远是
类属性那份初值——缓存一眼都没生效。所以这个检查数的是**系统调用的次数**，
不是"有没有绑上"：后者在 bug 版本里也照样成立。

跑法：python scripts/checks/rt_affinity_check.py
      python scripts/checks/rt_affinity_check.py --mutate   # 证明这检查抓得住旧写法

--mutate **只在内存里**把旧写法装回去（tmp/mutate_stdout_check.py 的同一套路），
不动仓库里的任何文件：改真文件再改回来这种事，工具调用一旦在"还没还原"的当口被
掐断，变异就留在工作区里了，而它长得跟正常代码一模一样。
"""

import os
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import rt_affinity  # noqa: E402


class CheckFailed(AssertionError):
    pass


def want(condition, message):
    if not condition:
        raise CheckFailed(message)


class FakeDecision:
    """足够骗过 pin_current_thread 的最小替身：给一个目标、收日志。

    目标是可改的——"主站换核"这件事只能这样演：同一个 decision 先给一个核集合，
    再给另一个。第一次写错这条时我给了个不可变的目标，于是"force 重新绑"那项
    全靠两个检查之间残留的状态才动，测的根本不是 force。
    """

    def __init__(self, target, log):
        self.target = target
        self.log = log

    def get(self, force=False):
        return self.target


def in_fresh_thread(function, *args):
    """在**新线程**里跑一段，异常原样抛回调用者。

    为什么非要这样：线程状态（`_ThreadState` 那份 per-thread 缓存）是**跨检查留着的**
    ——同一个线程里，前一项检查绑过核之后，下一项检查第一次调用就不会再绑、也不会
    再报，于是"该有几次调用"这个数全靠检查之间的先后顺序才说得通。第一版就是这么
    写歪的：两项检查报红，而代码是对的。

    换新线程跑，每一项都从没绑过的干净状态开始，数出来的次数才有意义。
    """
    outcome = {}

    def body():
        try:
            outcome["value"] = function(*args)
        except BaseException as error:  # noqa: BLE001  要原样带回主线程
            outcome["error"] = error

    worker = threading.Thread(target=body)
    worker.start()
    worker.join(timeout=10.0)
    want(not worker.is_alive(), f"检查线程没结束：{getattr(function, '__name__', function)}")
    if "error" in outcome:
        raise outcome["error"]
    return outcome.get("value")


def spy_syscall():
    """把 os.sched_setaffinity 换成记账版，返回 (次数列表, 还原函数)。"""
    calls = []
    original = getattr(os, "sched_setaffinity", None)

    def fake(pid, mask):
        calls.append((pid, set(mask)))

    os.sched_setaffinity = fake
    return calls, original


def restore_syscall(original):
    if original is None:
        del os.sched_setaffinity
    else:
        os.sched_setaffinity = original


def check_repeat_call_does_not_rebind_or_respam(context):
    """同一线程连问三次：只该有一次系统调用、一行日志。"""
    lines = []
    calls, original = spy_syscall()
    try:
        decision = FakeDecision({1, 2, 3}, lines.append)
        got = in_fresh_thread(lambda: [rt_affinity.pin_current_thread(decision)
                                       for _ in range(3)])[-1]
    finally:
        restore_syscall(original)

    want(calls == [(0, {1, 2, 3})],
         f"同一个线程问三次，系统调用发生了 {len(calls)} 次（该只 1 次）——"
         f"per-thread 缓存没生效，这就是桥日志刷屏 122 行/秒的那个毛病")
    want(got == {1, 2, 3}, f"返回的掩码不对：{got!r}")
    want(len(lines) == 1,
         f"绑核说明打了 {len(lines)} 行（该只 1 行）：{lines[:3]}")


def check_each_thread_binds_itself(context):
    """换个线程问，**必须**再绑一次：新线程不会继承别的线程的掩码。

    这条是在拦一种"看起来更简单"的修法——把状态挂到一个普通模块级对象上。
    那样确实不刷屏了，代价是第二个线程会以为自己已经绑过：它绑的是别人。
    所以这里要的是"每线程各自一次"，不是"整个进程一次"。
    """
    lines = []
    calls, original = spy_syscall()
    try:
        decision = FakeDecision({7, 8}, lines.append)

        def two_threads():
            rt_affinity.pin_current_thread(decision)
            worker = threading.Thread(target=rt_affinity.pin_current_thread,
                                      args=(decision,))
            worker.start()
            worker.join(timeout=5.0)
            return not worker.is_alive()

        finished = in_fresh_thread(two_threads)
    finally:
        restore_syscall(original)

    want(finished, "工作线程没结束")
    want(len(calls) == 2,
         f"两个线程各绑一次应该是 2 次系统调用，实际 {len(calls)} 次——"
         f"绑少了说明状态被跨线程共用了，那个线程其实没躲开主站")
    want(len(lines) == 2,
         f"两个线程各该报一行，实际 {len(lines)} 行")


def check_force_rebinds(context):
    """主站换核之后要能重新绑；核没变就不该白绑一次。

    两半都要：只验"变了能重绑"，就得不出"没变时不重绑"——而后者正是这个缓存的
    全部价值（桥每轮 select 都叫一次 reconcile）。第一版只写了前半句，还写错了：
    目标不变时 `target == state.applied` 会短路，force 根本不会再调系统调用，
    于是那项检查红在一个**正确**的行为上。
    """
    lines = []
    calls, original = spy_syscall()
    try:
        decision = FakeDecision({5}, lines.append)

        def run():
            before = len(calls)
            rt_affinity.pin_current_thread(decision)
            first = len(calls) - before
            before = len(calls)
            rt_affinity.pin_current_thread(decision, force=True)   # 核没变
            same = len(calls) - before
            decision.target = {6, 7}                               # 主站挪了
            before = len(calls)
            rt_affinity.pin_current_thread(decision, force=True)
            moved = len(calls) - before
            return first, same, moved

        first, same, moved = in_fresh_thread(run)
    finally:
        restore_syscall(original)

    want(first == 1, f"第一次调用就该绑一次，实际 {first} 次")
    want(same == 0,
         f"核没变、被 force 重算之后又绑了 {same} 次——白做系统调用，"
         f"这正是缓存要省掉的那一次")
    want(moved == 1, f"主站换核后该重新绑一次，实际 {moved} 次")
    want(len(lines) == 1,
         f"绑核说明同一线程只该有一行，实际 {len(lines)} 行：{lines}")


def check_target_none_does_nothing(context):
    """算不出该躲哪儿时（本机就是这样）安静返回，不绑也不报——理由由 _Decision 说。"""
    lines = []
    calls, original = spy_syscall()
    try:
        decision = FakeDecision(None, lines.append)
        got = in_fresh_thread(lambda: [rt_affinity.pin_current_thread(decision)
                                       for _ in range(3)])
    finally:
        restore_syscall(original)

    want(got == [None, None, None], f"没目标时该返回 None，实际 {got!r}")
    want(not calls, f"没目标时不该调系统调用，实际 {len(calls)} 次")
    want(not lines, f"没目标时不该打说明（那由 _Decision 负责），实际 {lines!r}")


CHECKS = [
    ("同一线程重复问：不重复绑、不重复报", check_repeat_call_does_not_rebind_or_respam),
    ("换线程：每个线程各自绑一次（不许跨线程共用）", check_each_thread_binds_itself),
    ("force=True：主站换核后能重新绑", check_force_rebinds),
    ("算不出目标：不绑、不报、返回 None", check_target_none_does_nothing),
]

NOT_COVERED = [
    "真实的核掩码：本机没有 sched_setaffinity 也没有 /proc 里的主站线程，"
    "扫 /proc、算躲哪些核这段走的是「不支持」分支。台架上要看启动日志里的核列表。",
    "确认真绑上了：这里数的是调用次数，`os.sched_setaffinity` 被换成了记账版，"
    "内核有没有接受这个掩码没验。",
]


def install_old_form():
    """把 2026-09-18 之前那份写法装回内存：每次调用新建 _ThreadState。"""
    def old_pin(decision, force=False):
        state = rt_affinity._ThreadState()          # ← 每调一次一个新实例，缓存全废
        target = decision.get(force=force)
        if target is None:
            return None
        if target == state.applied:
            return target
        try:
            os.sched_setaffinity(0, target)
        except (AttributeError, OSError) as error:
            if not state.announced:
                state.announced = True
                rt_affinity._log_or_print(decision.log, f"CPU 亲和：设置失败（{error}），本线程不绑核")
            state.applied = None
            return None
        if not state.announced:
            state.announced = True
            rt_affinity._log_or_print(
                decision.log, f"CPU 亲和：本线程绑到 {sorted(target)}（主站在其余核上）")
        state.applied = target
        return target

    rt_affinity.pin_current_thread = old_pin


def run(verbose_ok=True):
    failures = []
    for name, function in CHECKS:
        try:
            function({})
        except Exception as error:  # noqa: BLE001
            if verbose_ok:
                print(f"FAIL  {name}")
                print(f"      {type(error).__name__}: {error}")
            failures.append(name)
        else:
            if verbose_ok:
                print(f"ok    {name}")
    return failures


def main():
    if "--mutate" in sys.argv[1:]:
        print("先在现在的代码上跑一遍：")
        before = run()
        if before:
            print("现在的代码就已经红了，下面证明不了什么——先修好再来。")
            return 2
        print("\n再把旧写法装回内存里（只为这次验证）：")
        install_old_form()
        after = run()
        if after:
            print(f"\n结论：旧写法被抓住了（{len(after)} 项红）——这检查是有效的。")
            return 0
        print("\n结论：旧写法全绿，这检查等于白装。")
        return 1

    failures = run()
    print()
    print(f"{len(CHECKS) - len(failures)}/{len(CHECKS)} 项通过")
    if failures:
        print("失败：")
        for name in failures:
            print(f"  - {name}")
        print()
        print("没证到的：")
        for note in NOT_COVERED:
            print(f"  - {note}")
        return 1
    print("（想证明这个检查抓得住那个 bug：加 --mutate 再跑一次）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
