#!/usr/bin/env python3
"""把"某线程一次连续占用 CPU"的长窗口摊开看，事件带人话标签。

用来回答一个特定问题：周期那一拍停了 4 ms，是**自己的活慢**，还是**线程被拿走**。
判据是这一段连续占用窗口的长度——窗口够长说明 CPU 一直在自己手里，慢的原因在窗口内；
窗口短而中间有大片空白，才是被抢占/被阻塞。

窗口里认得两类事件：
  - 探针（交换入口 / 计时统计 / 审计输出 / 命令接收）——由 scripts/bench_trace_cycle.sh 注册；
  - 系统调用，按 arm64 的号翻译；表里没有的就原样打号，不猜名字。

用法：python3 scripts/analysis/trace_busy_windows.py <trace 文本> [最短期毫秒，默认 0.3] [tid...]
"""
import re
import sys
from collections import defaultdict

LINE = re.compile(
    rb"^\s*(\S+)-(\d+)\s+\[(\d+)\]\s+\S+\s+(\d+)\.(\d+):\s+(\w+):\s*(.*)$"
)
SWITCH = re.compile(
    rb"prev_comm=(\S+) prev_pid=(\d+) prev_prio=(\d+) prev_state=(\S+) "
    rb"==> next_comm=(\S+) next_pid=(\d+) next_prio=(\d+)"
)
IRQ = re.compile(rb"irq=(\d+) name=(\S+)")
VEC = re.compile(rb"vec=(\d+)")
SYS = re.compile(rb"(?:NR|id=) ?(\d+)")
MASTER = b"emaster-master"

PROBE = {
    b"cyc_exch": "探针·交换入口",
    b"cyc_tstat": "探针·计时统计",
    b"cyc_audit": "探针·审计输出",
    b"cyc_cmd": "探针·命令接收",
}

# arm64 的号是按 ABI 固定的；只列这批代码真会用的那几个。
SYSCALL = {
    19: "eventfd2", 20: "epoll_create1", 21: "epoll_ctl", 22: "epoll_pwait",
    25: "fcntl", 29: "ioctl", 56: "openat", 57: "close", 62: "lseek",
    63: "read", 64: "write", 72: "pselect6", 73: "ppoll", 78: "readlinkat",
    93: "exit", 94: "exit_group", 96: "set_tid_address", 98: "futex",
    99: "set_robust_list", 101: "nanosleep", 113: "clock_gettime",
    115: "clock_nanosleep", 117: "ptrace", 124: "sched_yield", 129: "kill",
    130: "tkill", 131: "tgkill", 134: "rt_sigaction", 135: "rt_sigprocmask",
    167: "prctl", 172: "getpid", 178: "gettid", 206: "sendto", 207: "recvfrom",
    211: "sendmsg", 212: "recvmsg", 215: "munmap", 226: "mprotect",
    233: "madvise", 261: "prlimit64", 276: "renameat2", 278: "getrandom",
    280: "memfd_create", 291: "statx", 293: "rseq",
}


def label(event, payload):
    if event in PROBE:
        return PROBE[event]
    if event == b"irq_handler_entry":
        m = IRQ.match(payload)
        if m is not None:
            return f"硬中断 irq={m.group(1).decode()} {m.group(2).decode()}"
    if event == b"softirq_entry":
        m = VEC.match(payload)
        if m is not None:
            return f"软中断 vec={m.group(1).decode()}"
    if event in (b"sys_enter", b"sys_exit"):
        m = SYS.match(payload)
        if m is not None:
            number = int(m.group(1))
            name = SYSCALL.get(number, f"号{number}")
            return f"系统调用{'进' if event == b'sys_enter' else '出'} {name}"
    return event.decode()


def main():
    path = sys.argv[1]
    floor_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 0.3
    wanted = set(int(a) for a in sys.argv[3:])
    floor = floor_ms / 1000.0

    on_since = {}
    windows = []
    pending = {}
    first_ts = last_ts = None
    switches = defaultdict(int)
    seen_pids = set()
    probes_total = defaultdict(int)

    with open(path, "rb") as handle:
        for raw in handle:
            if raw.startswith(b"#"):
                continue
            match = LINE.match(raw)
            if match is None:
                continue
            task, pid_raw, _cpu, sec, usec, event, payload = match.groups()
            ts = int(sec) + int(usec) / 1000000.0
            pid = int(pid_raw)
            if first_ts is None:
                first_ts = ts
            last_ts = ts
            if task == MASTER:
                seen_pids.add(pid)
            if event in PROBE:
                probes_total[event] += 1

            if event == b"sched_switch":
                sw = SWITCH.match(payload)
                if sw is None:
                    continue
                prev_pid = int(sw.group(2))
                next_pid = int(sw.group(6))
                if sw.group(1) == MASTER and prev_pid in on_since:
                    start = on_since.pop(prev_pid)
                    events = pending.pop(prev_pid, [])
                    if ts - start >= floor:
                        windows.append((prev_pid, start, ts, events))
                if sw.group(5) == MASTER:
                    switches[next_pid] += 1
                    on_since[next_pid] = ts
                    pending[next_pid] = []
            else:
                text = label(event, payload)
                for other in on_since:
                    pending[other].append((ts, text))

    print(f"记录跨度 {first_ts:.3f} → {last_ts:.3f}（{last_ts - first_ts:.1f} s）")
    print(f"emaster-master 线程：{sorted(seen_pids)}")
    for pid in sorted(switches):
        print(f"   tid={pid} 上 CPU {switches[pid]} 次")
    hits = "  ".join(
        f"{PROBE[k].split('·')[1]}={probes_total[k]}" for k in PROBE if probes_total[k])
    print("探针命中：" + (hits if hits else "一次都没有"))
    print()

    if wanted:
        windows = [w for w in windows if w[0] in wanted]
    windows.sort(key=lambda w: w[1])
    print(f"== 一次连续占用 CPU 超过 {floor_ms} ms 的窗口（{len(windows)} 个）==")
    for pid, start, end, events in windows:
        print(f"---- tid={pid}  {start:.6f} → {end:.6f}  连续 {((end - start) * 1000):.3f} ms"
              f"  窗口内事件 {len(events)} 条")
        for ets, text in events:
            print(f"      +{(ets - start) * 1000:8.3f} ms  {text}")


main()
