#!/usr/bin/env python3
"""采集"这一轮跑的是什么装置、什么代码、什么配置"，落成一份 key=value 文本。

用法:
    python3 scripts/capture_identity.py <输出文件> [--deployment ID] [--no-scan]

    --deployment  不给则取环境变量 DEPLOYMENT
    --no-scan     跳过总线扫描（链路没起或不想占用总线时用）

为什么要有它。探查的每一条结论都要跨轮次、跨天、跨人比较，而比较的前提是"除被测变量
外其余一致"。现有记录只覆盖了"发生了什么"（激励与响应），没有覆盖"它当时是什么"——
代码是什么版本、跑的二进制是不是同一份、配置是哪四份、总线上插的是哪几台。缺了这些，
两次运行的差异无法归因。

三个设计取舍：

  · 二进制摘要，不只是源码提交。源码相同不等于装置上跑的是同一份二进制（配置是构建期
    编进去的，改配置要重编）。
  · 配置按**引用链**解析而不是按文件名猜：部署档给出 topology_id / motion_profile_id /
    operation_profile_ids，逐个在该类目录里按 id 字段找到文件并取摘要。文件名叫什么无关。
  · 扫描失败不算错，写 scan_error= 并继续。链路没起时扫不到是正常现象，不该让整份身份
    采集失败——那会诱使人跳过这一步。

缺失一律写 unknown / na，不写 0：0 在多数读数上是合法值。
"""

import argparse
import glob
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 构建目录由环境变量 BUILD 给，默认与 scripts/*.sh 同一个约定。二进制落在哪个子目录
# 由 apps/<名字>/ 决定，所以这里跟着 apps 布局走。
BUILD = os.environ.get("BUILD") or os.path.join(REPO, "build", "linux-debug")

# 配置的类别 -> (目录, id 字段名)
CONFIG_KINDS = (
    ("deployment", "deployments", "deployment_id"),
    ("topology", "topologies", "topology_id"),
    ("motion_profile", "motion_profiles", "motion_profile_id"),
    ("operation_profile", "operation_profiles", "operation_profile_id"),
    ("device_profile", "devices", "profile_id"),
)


def run(cmd, timeout=60):
    """跑一条命令，返回 (rc, stdout)。命令不存在或超时都当失败处理，不抛。"""
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
        return done.returncode, (done.stdout or "").strip()
    except (OSError, subprocess.SubprocessError):
        return 127, ""


def run_merged(cmd, timeout=60):
    """跑一条命令，stderr 并进 stdout。用于既要结果又要失败原因的场合，
    避免为了拿一句错误话而把同一条命令跑两遍（扫描一次要十几秒）。"""
    try:
        done = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=timeout, check=False)
        return done.returncode, (done.stdout or "").strip()
    except (OSError, subprocess.SubprocessError):
        return 127, ""


def sha256(path):
    """文件的 sha256。用 hashlib 而不是调外部命令，省一次进程且不受 PATH 影响。"""
    import hashlib

    try:
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for block in iter(lambda: handle.read(1 << 20), b""):
                digest.update(block)
        return digest.hexdigest()
    except OSError:
        return "unknown"


def find_config(kind_dir, id_field, wanted):
    """在该类目录里按 id 字段找到那一份，返回 (path, data)。找不到返回 (None, None)。

    不按文件名匹配：文件名用下划线、id 用连字符，两者经常不一致。
    """
    if not wanted:
        return None, None
    for path in sorted(glob.glob(os.path.join(REPO, "config", kind_dir, "*.json"))):
        try:
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            continue
        if data.get(id_field) == wanted:
            return path, data
    return None, None


def collect_config_chain(deployment_id):
    """按引用链解析配置，返回 (lines, interface, rt_core)。

    链：部署档 -> 拓扑 / 运动方案 / 操作方案(可多份) -> 设备档(来自拓扑里的 profile_id)。
    rt_core 是部署档的 realtime.cpu_core —— 实时线程绑定的核号，宿主上下文要按它取。
    """
    lines = []
    deployment_path, deployment = find_config("deployments", "deployment_id", deployment_id)
    if deployment is None:
        lines.append("config_error=部署档未找到(%s)" % deployment_id)
        return lines, None, None

    lines.append("config_deployment_id=%s" % deployment_id)
    lines.append("config_deployment_file=%s" % os.path.relpath(deployment_path, REPO))
    lines.append("config_deployment_sha256=%s" % sha256(deployment_path))

    interface = deployment.get("ethercat_interface")

    topology_path, topology = find_config("topologies", "topology_id", deployment.get("topology_id"))
    lines.append("config_topology_id=%s" % (deployment.get("topology_id") or "unknown"))
    lines.append("config_topology_sha256=%s" % (sha256(topology_path) if topology_path else "unknown"))

    motion_path, _ = find_config("motion_profiles", "motion_profile_id",
                                 deployment.get("motion_profile_id"))
    lines.append("config_motion_profile_id=%s" % (deployment.get("motion_profile_id") or "unknown"))
    lines.append("config_motion_profile_sha256=%s" % (sha256(motion_path) if motion_path else "unknown"))

    # 操作方案可能有多份，逐份列出；顺序即配置里的顺序，不要排序。
    for index, operation_id in enumerate(deployment.get("operation_profile_ids") or [], start=1):
        path, _ = find_config("operation_profiles", "operation_profile_id", operation_id)
        lines.append("config_operation_profile_%d_id=%s" % (index, operation_id))
        lines.append("config_operation_profile_%d_sha256=%s"
                     % (index, sha256(path) if path else "unknown"))

    # 设备档来自拓扑里第一台从站引用的 profile_id（台架上通常全同）。
    device_id = None
    if topology:
        for slave in topology.get("slaves") or []:
            if slave.get("profile_id"):
                device_id = slave["profile_id"]
                break
    path, _ = find_config("devices", "profile_id", device_id)
    lines.append("config_device_profile_id=%s" % (device_id or "unknown"))
    lines.append("config_device_profile_sha256=%s" % (sha256(path) if path else "unknown"))

    realtime = deployment.get("realtime") or {}
    rt_core = realtime.get("cpu_core")
    lines.append("config_rt_core=%s" % (rt_core if rt_core is not None else "unknown"))
    lines.append("config_rt_required=%s" % ("yes" if realtime.get("required") else "no"))
    return lines, interface, rt_core


def collect_interrupts(interface, rt_core):
    """按 CPU 汇总中断，并单列网卡与实时核。

    为什么记这个。2026-10-08 查到网卡 14 个队列里只有 0 号在用、且全落在核 0，
    而实时线程绑在核 11；irqbalance 未安装，所以落点是内核默认值，没人管过。
    中断会抢占 SCHED_FIFO 线程，所以"实时核上来了多少中断"直接决定周期抖动。

    记的是**每轮开始前**的快照，不是运行中的峰值 —— 差分两次快照就能得到本轮的中断数，
    而运行中的峰值需要采样线程，那本身就会挤占实时核。
    """
    try:
        with open("/proc/interrupts", encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError as error:
        return ["interrupts_error=读不了 /proc/interrupts（%s）" % error]
    if not lines:
        return ["interrupts_error=/proc/interrupts 是空的"]

    cpu_count = len(lines[0].split())
    per_cpu = [0] * cpu_count
    nic_total = 0
    nic_queues = 0
    for line in lines[1:]:
        _, sep, rest = line.partition(":")
        if not sep:
            continue
        fields = rest.split()
        if len(fields) < cpu_count:
            continue
        counts = []
        for value in fields[:cpu_count]:
            try:
                counts.append(int(value))
            except ValueError:
                counts = None
                break
        if counts is None:
            continue
        for index, value in enumerate(counts):
            per_cpu[index] += value
        # 网卡的中断行名字形如 enp49s0-0、enp49s0-1…… 逐队列各一行。
        if interface and interface in rest:
            nic_total += sum(counts)
            nic_queues += 1

    out = ["interrupts_cpu_count=%d" % cpu_count,
           "interrupts_total_all_cpus=%d" % sum(per_cpu)]
    for index, value in enumerate(per_cpu):
        out.append("interrupts_cpu_%d=%d" % (index, value))
    if interface:
        out.append("nic_irq_queues=%d" % nic_queues)
        out.append("nic_irq_total=%d" % nic_total)
        if nic_queues == 1:
            # 单队列意味着全部收包处理挤在一个核上；多队列而只有一个在用是浪费，
            # 但更重要的是它决定了"谁在处理这一拍的回包"。
            out.append("nic_irq_note=只有一个队列在用")
    else:
        out.append("nic_irq_total=unknown")

    if rt_core is not None and 0 <= rt_core < cpu_count:
        out.append("interrupts_on_rt_core=%d" % per_cpu[rt_core])
    return out


def collect_rt_core_threads(rt_core):
    """实时核上当时住着哪些线程。中断抢占实时线程，线程本身也抢（内核线程尤其）。"""
    if rt_core is None:
        return []
    rc, out = run(["ps", "-eLo", "psr,comm", "--sort=psr"], timeout=30)
    if rc != 0:
        return ["rt_core_threads_error=ps 失败"]
    names = []
    for line in out.splitlines()[1:]:
        fields = line.split(None, 1)
        if len(fields) != 2:
            continue
        try:
            core = int(fields[0])
        except ValueError:
            continue
        if core == rt_core:
            names.append(fields[1].strip())
    # 去重后计数：同一个程序多个线程时，只关心"有哪些东西"。
    unique = sorted(set(names))
    return ["rt_core=%d" % rt_core,
            "rt_core_thread_count=%d" % len(names),
            "rt_core_threads=%s" % ",".join(unique)]


def collect_scan(interface):
    """跑扫描工具，返回 (lines, error)。扫不到时 error 非空而 lines 为空。"""
    if not interface:
        return [], "接口未知（部署档里没有 ethercat_interface）"
    tool = os.path.join(BUILD, "apps", "diagnose_slave", "emaster-scan")
    if not os.path.isfile(tool):
        return [], "扫描工具未构建（%s）" % os.path.relpath(tool, REPO)

    # 合并 stderr：失败原因（"错误：一台从站都没扫到"之类）在那里，而 stdout 只有
    # interface= 一行。只取 stdout 会得到一句没有信息量的错误。
    rc, out = run_merged([tool, interface], timeout=120)
    if rc != 0 or "slaves=" not in out:
        # 合并 stderr 之后，错误行**不一定在最后**：stderr 无缓冲先到，stdout 有缓冲
        # 最后才刷出来，所以"取最后一行"拿到的常是那句没有信息量的 interface=。
        # 先找带"错误"的行，找不到才退回最后一行。
        lines_out = [line for line in out.splitlines() if line.strip()]
        detail = next((line for line in lines_out if "错误" in line), None)
        if detail is None:
            detail = lines_out[-1] if lines_out else "无输出"
        return [], "扫描失败 rc=%d：%s" % (rc, detail)

    lines = []
    for line in out.splitlines():
        if line.startswith("slaves="):
            lines.append("scan_" + line.strip())
        elif line.startswith("slave="):
            # 原样保留：身份三件套、状态、换算参数都在这一行里，重排反而会丢信息。
            lines.append("scan_" + line.strip())
    return lines, None


def main():
    parser = argparse.ArgumentParser(description="采集装置身份（装置可比性的前提）")
    parser.add_argument("output", help="输出文件")
    parser.add_argument("--deployment", default=os.environ.get("DEPLOYMENT", ""),
                        help="部署 ID（默认取环境变量 DEPLOYMENT）")
    parser.add_argument("--no-scan", action="store_true", help="跳过总线扫描")
    args = parser.parse_args()

    lines = []
    rc, stamp = run(["date", "+%Y-%m-%d %H:%M:%S"])
    lines.append("captured_at=%s" % (stamp if rc == 0 else "unknown"))

    rc, commit = run(["git", "-C", REPO, "log", "-1", "--format=%H"])
    lines.append("git_commit=%s" % (commit if rc == 0 else "unknown"))
    rc, dirty = run(["git", "-C", REPO, "status", "--porcelain"])
    # 工作区脏意味着"源码提交"不足以标识这一轮跑的代码，必须一起记。
    lines.append("git_dirty=%s" % ("yes" if (rc == 0 and dirty) else "no"))

    binary = os.path.join(BUILD, "apps", "master", "emaster-master")
    lines.append("binary=%s" % os.path.relpath(binary, REPO))
    lines.append("binary_sha256=%s" % (sha256(binary) if os.path.isfile(binary) else "missing"))

    config_lines, interface, rt_core = collect_config_chain(args.deployment)
    lines.extend(config_lines)

    lines.append("interface=%s" % (interface or "unknown"))
    try:
        with open("/proc/loadavg", encoding="utf-8") as handle:
            load = handle.read().split()[:3]
        lines.append("loadavg=%s" % " ".join(load))
    except OSError:
        lines.append("loadavg=unknown")
    if interface:
        try:
            with open("/sys/class/net/%s/carrier" % interface, encoding="utf-8") as handle:
                lines.append("carrier=%s" % handle.read().strip())
        except OSError:
            lines.append("carrier=unknown")

    rc, kernel = run(["uname", "-r"])
    lines.append("host_kernel=%s" % (kernel if rc == 0 else "unknown"))
    rc, nproc = run(["nproc"])
    lines.append("host_nproc=%s" % (nproc if rc == 0 else "unknown"))
    # rlimit 是主站能否拿到实时优先级的前提，拿不到它会**拒绝进入运行态**。
    #
    # 要记的是**主站实际拿到的值**，不是采集进程继承到的值：单轮脚本自己会提这个上限
    # （bench_reciprocate_30deg.sh / bench_idle.sh 里那段守卫）。只记继承值会得到 0，
    # 而 0 看起来像"主站拿不到实时优先级"，与它实际跑得好好的相矛盾 —— 一个会把人
    # 引向错误方向的读数，比没有这个读数更糟。所以两个都记，并标明哪个是哪个。
    # 先记身份：非 root 时提不动硬上限，after_raise 那个 0 说明的是"这个用户提不动"，
    # 而不是"主站拿不到"。两个数必须连着 uid 一起读，否则会得出相反的结论。
    rc, uid = run(["id", "-u"])
    lines.append("uid=%s" % (uid if rc == 0 else "unknown"))
    rc, before = run(["bash", "-c", "ulimit -r"])
    rc, after = run(["bash", "-c", "ulimit -r unlimited 2>/dev/null; ulimit -r"])
    lines.append("ulimit_r_inherited=%s" % (before if rc == 0 else "unknown"))
    lines.append("ulimit_r_after_raise=%s" % (after if rc == 0 else "unknown"))

    # 宿主上下文。与装置身份同放一份：两者回答的是同一类问题 —— "这一轮是在什么条件下跑的"。
    rt_core = None
    for line in lines:
        if line.startswith("config_rt_core="):
            value = line.split("=", 1)[1]
            rt_core = int(value) if value.isdigit() else None
    lines.extend(collect_interrupts(interface, rt_core))
    lines.extend(collect_rt_core_threads(rt_core))

    if args.no_scan:
        lines.append("scan=skipped")
    else:
        scan_lines, error = collect_scan(interface)
        if error:
            lines.append("scan_error=%s" % error)
        else:
            lines.extend(scan_lines)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".", exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("已写 %s（%d 项）" % (args.output, len(lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
