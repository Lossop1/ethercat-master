#!/usr/bin/env python3
"""把"别跟主站抢核"收成一份：桥和图形界面都要按同一条规矩躲开主站的实时核。

**为什么要有这一层。** 主站的周期线程绑在固定的核上（台架上就是 CPU 11）。任何
在旁边跑、又要频繁醒来的进程——转发套接字的桥、画曲线的界面——如果落到同一个核，
就是在跟一个 1 ms 周期的实时线程争 CPU。这件事不该每写一个工具就重推一遍，也
不该有两份会各自漂移的 /proc 扫描。所以抽到这里，tools/socket_bridge.py 与
tools/emaster_gui 共用。

**绑的是线程，不是进程——这一条是实测出来的，不是推的。** Linux 上
`sched_setaffinity(0, ...)` 只改**调用它的那个线程**的掩码，不碰同进程里已经在
跑的其他线程；而**新建**的线程会继承创建者的掩码。2026-09-18 在香橙派上验过：
主线程绑成 {0} 之后，一个先起的工作线程仍然看得到全部 12 个核，随后新起的线程
才继承到 {0}。

这条差别对**多线程**的调用方是致命的：在一个线程里绑一次，然后以为整个程序都
躲开了主站，实际上是"只有那一个线程躲开了"，其余线程照样跟主站抢。所以这里
给出的接口是 `pin_current_thread()`，要求**每个会自己醒来的线程各叫一次**；
process_affinity() 那个"一把全绑"的便利接口在 Linux 上不存在，不要自己写一个。

**边界。** 这套东西只在 Linux 上有意义（本机 Windows 既没有 sched_setaffinity
也没有 /proc/*/comm），在别的平台上每个函数都安静地什么都不做，并返回一句说明。
"""

import glob
import os
import threading
import time

# 主站进程的主线程名（main.c 里设的）。拿它去 /proc 里认人。
MASTER_COMM = "emaster-master"

# 主站可能比我们晚起来，所以判断要反复做：只在开局算一次的话，先起的那个进程
# 会以为"主站没在跑"而不绑核，等主站起来两者就落到同一个核上了。
RECHECK_S = 5.0


def parse_cpu_list(text):
    """把 "0-3,11" 解成 {0,1,2,3,11}。解不动就返回空集，不猜。"""
    cpus = set()
    for part in (text or "").split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            low, _, high = part.partition("-")
            try:
                cpus.update(range(int(low), int(high) + 1))
            except ValueError:
                return set()
        else:
            try:
                cpus.add(int(part))
            except ValueError:
                return set()
    return cpus


def allowed_cpus():
    """本进程能用的核。非 Linux（或不许问）时返回空集。"""
    try:
        return os.sched_getaffinity(0)
    except (AttributeError, OSError):
        return set()


def master_thread_affinity():
    """扫 /proc 找主站的线程，返回 {cpuset} 的列表（没找到就返回空列表）。

    读的是 Cpus_allowed_list 而不是 status 里那个十六进制掩码——前者是"0-3,11"
    这样的区间写法，机器核多的时候也不用数逗号。
    """
    masks = []
    for entry in glob.glob("/proc/[0-9]*/comm"):
        try:
            with open(entry, "r") as handle:
                if handle.read().strip() != MASTER_COMM:
                    continue
        except OSError:
            continue
        root = os.path.dirname(entry)
        for status_path in glob.glob(os.path.join(root, "task", "*", "status")):
            try:
                with open(status_path, "r") as handle:
                    for line in handle:
                        if line.startswith("Cpus_allowed_list:"):
                            masks.append(parse_cpu_list(line.split(":", 1)[1]))
                            break
            except OSError:
                continue
        break
    return [mask for mask in masks if mask]


def choose_cpus(requested=None):
    """算出该躲到哪些核上。返回 (集合, 说明)；集合为 None 表示这次不绑。

    说明那半句是给人看的：没绑核时必须能说出**为什么**没绑，否则"没报错"会被
    当成"已经躲开了"。
    """
    allowed = allowed_cpus()
    if not allowed:
        return None, "本机不支持 sched_setaffinity，不绑核"
    if requested is not None:
        overlap = set(requested) & allowed
        if not overlap:
            return None, (f"指定的核都不在可用集合里（可用 {sorted(allowed)}），不绑核")
        return overlap, None

    masks = master_thread_affinity()
    if not masks:
        return None, (f"没找到在跑的 {MASTER_COMM}，这次不绑核；主站起来后会自动重算")
    busy = set()
    for mask in masks:
        busy |= mask
    free = allowed - busy
    if not free:
        return None, (f"主站的线程占满了可用核（{sorted(busy)}），没得躲，不绑核")
    return free, None


def _log_or_print(log, message):
    if log:
        log(message)
    else:
        print(message, flush=True)


class _Decision:
    """进程级共享的判断缓存。

    每个线程各自去扫一遍 /proc 是浪费，而且线程越多浪费越大；但判断又不能只在
    开局做一次（主站可能还没起来）。所以算出来的结果在这里按 RECHECK_S 共享，
    谁来问都拿同一份。
    """

    def __init__(self, requested=None, log=None):
        self.requested = requested
        self.log = log
        self.lock = threading.Lock()
        self.at = 0.0
        self.target = None
        self.last_note = None

    def get(self, force=False):
        now = time.monotonic()
        with self.lock:
            if force or now - self.at >= RECHECK_S:
                self.at = now
                self.target, note = choose_cpus(self.requested)
                # 同一句"为什么没绑核"只报一次：它每 5 秒重算一遍，不拦着会刷屏。
                if note and note != self.last_note:
                    self.last_note = note
                    _log_or_print(self.log, f"CPU 亲和：{note}")
            return self.target


class _ThreadState(threading.local):
    """每个线程自己记"我当前绑在哪儿"，免得每轮循环都白做一次系统调用。"""

    applied = None
    announced = False


# **必须是这一个实例，不能每次调用新建一个。** threading.local 的线程隔离是**按实例**
# 分的：新实例有自己独立的每线程存储，读到的永远是类属性那份初值。所以
# `pin_current_thread()` 里写成 `state = _ThreadState()` 的话，缓存一眼都没生效——
# 每次调用 applied 都是 None，于是每次都真做一次 sched_setaffinity、并且每次
# 都打一行"已绑核"。
#
# 2026-09-18 台架上量到过：桥的日志里这句话一秒钟 122 行、单文件 657 KB；而这里
# 省掉那次系统调用正是 _ThreadState 存在的理由（桥每轮 select 都调 reconcile）。
_STATE = _ThreadState()


def pin_current_thread(decision, force=False):
    """把**当前线程**绑到主站用不着的核上。返回真的绑上的集合，或 None。

    **每个会自己醒来的线程各叫一次**，且要在它开始干活之前叫：已经跑起来的
    线程不会因为别的线程改了掩码而跟着变（模块开头那段实测）。
    """
    state = _STATE
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
            _log_or_print(decision.log, f"CPU 亲和：设置失败（{error}），本线程不绑核")
        state.applied = None
        return None
    if not state.announced:
        state.announced = True
        _log_or_print(decision.log,
                      f"CPU 亲和：本线程绑到 {sorted(target)}（主站在其余核上）")
    state.applied = target
    return target


class AffinityKeeper:
    """单线程程序（桥）的便利外壳：一个叫 reconcile() 的循环钩子。

    多线程的调用方不要用它——它只绑**调用它的那个线程**。请直接建一个 _Decision，
    然后让每个线程自己 pin_current_thread()（模块开头那段实测就是这条的由来）。
    """

    def __init__(self, requested, log):
        self.decision = _Decision(requested=requested, log=log)
        self.applied = None

    def reconcile(self, force=False):
        self.applied = pin_current_thread(self.decision, force=force)
