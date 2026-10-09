# EtherCAT Master

面向机器人控制的工业 EtherCAT 主站。C11 + SOEM，1 ms 周期，12 台 CiA 402 伺服驱动器。

**两条边界，先讲清楚：**

- **本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。详见第 II 部分 §25。
- **探查记录（跑了什么、得到什么）在 [`wholetest/采集.md`](wholetest/采集.md)。** 本文件讲**怎么用**和**怎么实现的**。

---

# 第 I 部分 · 使用说明

## 0 全景

| 组件 | 位置 | 干什么 |
| --- | --- | --- |
| 主站 | `build/tools/master/emaster-master` | 跑 1 ms 周期，收发过程数据 |
| 命令口 | `/tmp/emaster-<部署>.sock` | AF_UNIX，发目标、查状态 |
| 观测口 | `/tmp/emaster-<部署>-obs.sock` | AF_UNIX，只读；**要 `EMASTER_OBSERVATION=1` 才存在** |
| 单轮测试 | `scripts/bench_idle.sh`、`scripts/bench_reciprocate_30deg.sh` | 跑一轮，产物落 `/tmp/<TAG>_*` |
| 多轮测试 | `scripts/bench_series.sh` | 重复 N 轮 + 把证据收进仓库 |
| 读报告 | `scripts/analysis/*.py` | 报告是覆盖式的，且可能几百 MB |
| 桥 | `tools/socket_bridge.py` | 把两个 unix 套接字搬成 TCP |
| 图形界面 | `tools/emaster_gui/`（PyQt5） | 跨机，跑在**另一台机器**上 |
| 终端面板 | `scripts/console.sh` → `tools/emaster_console.py` | 同机，TUI |
| 远程操作 | `scripts/pi.py` | 从 Windows 跑 Pi 上的命令、传文件 |

**四条通路，按你在哪台机器上选：**

```text
同机（都在 Pi 上）     客户端 ──unix──► /tmp/emaster-<部署>.sock
跨机（界面在 Windows） 界面 ──TCP:5001──► socket_bridge.py ──unix──► 命令口
                             ──TCP:5002──►              ──unix──► 观测口
从 Windows 跑 Pi 命令  pi.py ──SSH──► Pi
```

## 1 前提与约定

- **要 root**：主站要 `mlockall` + `SCHED_FIFO`，套接字也由 root 建。
- **要 `ulimit -r unlimited`**：不然实时优先级设不上（脚本里都带了）。
- **部署 ID 必须与物理轴数一致**：

  | 轴数 | 部署 ID | 轴数 | 部署 ID |
  | --- | --- | --- | --- |
  | 2 | `orangepi-bench-dual-30deg` | 8 | `orangepi-bench-oct-30deg` |
  | 3 | `orangepi-bench-triple-30deg` | 9 | `orangepi-bench-non-30deg` |
  | 4 | `orangepi-bench-quad-30deg` | 10 | `orangepi-bench-dec-30deg` |
  | 5 | `orangepi-bench-quint-30deg` | 11 | `orangepi-bench-undec-30deg` |
  | 6 | `orangepi-bench-sext-30deg` | 12 | `orangepi-bench-duodec-30deg` |
  | 7 | `orangepi-bench-sept-30deg` | 单轴 | `orangepi-bench-single-30deg` |

  部署是**按位置**配的，所以**换同型号设备不用改命令**（见 §12.2）。

- **不要用 `[ -x ]` 查本仓库的脚本。** `scripts/` 下的文件都是 `100644`，一律以 `bash <脚本>` 调用；能执行位不是这里的约定。查可读即可。

## 2 起主站

```bash
cd /home/orangepi/ethercat-master
sudo bash -c 'ulimit -r unlimited; exec env EMASTER_OBSERVATION=1 \
  ./build/tools/master/emaster-master --deployment <部署ID>'
```

**`EMASTER_OBSERVATION=1` 不是可选项**：不开的话观测口根本不建，报告里温度／母线电压／电流那几栏恒为「—」，事后无法回答「那一刻电压是多少」。

**确认起来了**（别只看进程在）：

```bash
pgrep -a -f emaster-master
ls /tmp/emaster-<部署ID>*.sock                 # 命令口 + 观测口，两个
sudo timeout 3 nc -U /tmp/emaster-<部署ID>.sock <<< status | grep -o 'state=[0-9]*'
```

`state=4` 就是 RUNNING。顺便看一眼拓扑（轴数、单步上限、换算）：

```bash
sudo timeout 3 nc -U /tmp/emaster-<部署ID>.sock <<< topology
# OK|axes=8,max_step=63715|a1:bus=1,enc=16384,gear=28/1,torque=0|a2:...
```

**停：Ctrl+C。** 主站注册了 SIGINT／SIGTERM，走的是**和命令口 `shutdown` 完全相同**的那条安全停机路径（安全输出 → 停用各轴 → 回 INIT），不是掐断。

**清场**（换部署、重开之前都该做；`pkill` **要带 sudo**，不然静默失败）：

```bash
sudo pkill -9 -f emaster-master
sudo pkill -9 -f socket_bridge
rm -f /tmp/emaster-*.sock
```

## 3 跨机图形界面

界面在**另一台机器**上跑，主站和桥在 Pi 上。

**Pi ①：主站** —— 见 §2。

**Pi ②：桥**

```bash
cd /home/orangepi/ethercat-master
sudo python3 tools/socket_bridge.py --deployment <部署ID> --bind 0.0.0.0 --port 5001
```

**另一台机器（Windows）：界面**

```powershell
cd D:\taili\ethercat_master
$env:PYTHONPATH='tools'
python -m emaster_gui --tcp 192.168.124.81:5001
```

- 端口约定：**命令 5001、观测 5002**（界面自动用 PORT+1，不用另给）
- `--tcp` **同时开了命令通道**，也就是界面能驱动电机。**只读**就换成 `--obs-endpoint tcp:192.168.124.81:5002`（不给命令口）
- 桥会自己绑到主站实时核**之外**的核（`tools/rt_affinity.py` 从 `/proc` 读主站掩码）
- 桥监听 `0.0.0.0` 会打一句警告：**这两个口没有任何认证，能连上命令口的人就能驱动电机**

## 4 终端面板（同机）

```bash
sudo bash scripts/console.sh --start --obs   # 没在跑就替你拉起来
sudo bash scripts/console.sh                 # 接上已在跑的主站
```

控制律只有一份实现（`tools/emaster_console.py` 的 Engine），终端面板与图形界面共用它。
参数：环境变量 `DEPLOYMENT`、`RANGE`（自助软范围，默认 45°）、`JOG_SPEED`（默认 10 °/s）。
**`--obs` 只对本次由它拉起的主站生效**——已经在跑的主站用的是它启动时那个环境。

## 5 单轮台架测试

### 5.1 空转（不发目标、**电机不动**）

```bash
sudo env DEPLOYMENT=<部署ID> IDLE=60 TAG=idle1 bash scripts/bench_idle.sh
```

**空转只使能保持，不发任何运动指令 —— 电机不会动，这是设计如此。** 用它把主站单独放在总线上看它稳不稳。改线、扩轴、换设备之后的第一道关。

### 5.2 30 度往返（**电机会动**）

```bash
sudo env DEPLOYMENT=orangepi-bench-duodec-30deg \
  DURATION=60 TRAVERSE=5 DEGREES=30 RATE=100 WAVE=triangle \
  ONLY_AXIS=0 OBSERVATION=1 TAG=recip30 \
  bash scripts/bench_reciprocate_30deg.sh
```

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `DEPLOYMENT` | `orangepi-bench-quint-30deg` | 部署 ID |
| `DURATION` | 600 | 往返总时长（秒） |
| `TRAVERSE` | 3 | **单程**耗时（秒）。30 度／5 s = 6 度/秒 |
| `DEGREES` | 30 | 单轴位移（输出轴度数） |
| `RATE` | 100 | 目标更新频率（Hz） |
| `WAVE` | triangle | `triangle` 或 `sine` |
| `ONLY_AXIS` | 0 | 0=全部一起动；N=只动第 N 根，其余钉在起点 |
| `OBSERVATION` | **1** | 是否让主站开观测通道 |
| `SETTLE_TICKS` | 10 | 客户端退出后的保持窗口（0.5 s/拍）。**在 shutdown 之前** |
| `TAG` | recip30 | 产物文件名后缀 |
| `REPORT_INTERVAL` | 10 | 客户端打印采样间隔（秒） |

**退出码**：0=跑满且命令流连续；1=命令流出现过 200 ms 空档；2=**主站中途中断了命令流**；3=命令口 shutdown 没让主站退出。

**产物**：`/tmp/<TAG>_outcome.txt`（全程输出）、`_done`（退出码）、`_master.log`、`_client.log`。报告在部署配置的 `run_report_path`。

## 6 多轮测试

```bash
sudo env DEPLOYMENT=orangepi-bench-duodec-30deg SERIES=<系列名> ROUNDS=6 GAP=60 \
  MODE=reciprocate DURATION=60 TRAVERSE=5 DEGREES=30 RATE=100 ONLY_AXIS=0 OBSERVATION=1 \
  bash scripts/bench_series.sh
```

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `DEPLOYMENT` | **必填** | 没有默认值：写默认部署等于把某个台架写进工具里 |
| `SERIES` | **必填** | 决定证据目录 `reports/_<SERIES>/`；**换个名字就不会覆盖上一批** |
| `ROUNDS` | 6 | 轮数 |
| `GAP` | 60 | 轮间间隔（秒）。**这个值本身是变量** |
| `MODE` | reciprocate | `idle` 或 `reciprocate` |

其余参数原样传给单轮脚本，这里不重复也不校验。

```text
reports/_<SERIES>/
  index.tsv                  一行一轮（表头 = 本脚本的列 + report_row.py 的列）
  r01_<TAG>/
    condition.txt            本轮施加的条件
    device-identity.txt      装置身份 + 主机上下文
    nic-before/after/diff.txt  网卡收发计数
    anomalies.tsv            从主站日志抽出的异常表
    notes.md                 人工观察
    <TAG>_master.log         主站日志
    <TAG>_outcome.txt        单轮脚本全程输出
    <TAG>_client.log         客户端日志
    summary.txt              report_summary.py 对该轮报告的输出（人读）
    row.tsv                  该轮在 index.tsv 里那一行
    exit-code.txt
    report-name.txt          该轮完整报告在归档里的文件名 ← 完整证据的坐标
```

完整报告几十到几百 MB，**不入库**，留在 `run_report_path` 所在目录的 `archive/` 下。
**退出码 0 只表示轮次跑完了，不代表每轮都成功**——成败看 `index.tsv`。

## 7 读报告

报告是**单行巨型 JSON**（空转 20 MB，运动轮 500 MB 量级），而且是**覆盖式**的：下一轮一开跑，上一轮的就没了（主站会往 `archive/` 留副本）。

| 工具 | 用途 |
| --- | --- |
| `report_summary.py <报告>` | **人读**：跑了多久、故障形态、故障点逐轴 AL、停机收尾 |
| `report_row.py <报告> [标签]` | **机读**：一份报告压成 TSV 一行；`--header` 打印列名 |
| `series_index.py reports/_<系列>` | 按每轮 `report-name.txt` **从归档重建整张表** |
| `series_verdict.py` | 按容错策略给出每一轮的判定 |
| `extract_report.py <报告>` | 就地抽关键字段（不下载整个 JSON） |
| `anomalies.py` | 主站日志 → `anomalies.tsv` |
| `trace_window.py` | 把异常窗口读成逐拍表 |
| `../nic_counters.py --diff` | 网卡收发计数差分 |
| `../capture_identity.py` | 装置身份 + 主机上下文 |

（都在 `scripts/analysis/` 下，后两条在 `scripts/` 下。）

### 7.1 字段住在哪一层（查过的，别猜）

取错层不会报错，只会安静地退化成 `-`。

| 层 | 字段 |
| --- | --- |
| `result` | `cycle_count` `expected_wkc` `actual_wkc` `op_reached` `all_axes_enabled_reached` `motion_completed` `wkc_error_count` `wkc_no_frame_count` |
| `process_data_delivery` | `over_budget_cycle_count` `frame_interval_gap_count` `tail_max_receive_ns` `tail_max_receive_ok_ns` `tail_max_error_counter_ns` `error_counter_read_fail_count` `first_mismatch_exchange` |
| `axes[].timing` | **`wkc_mismatch_count`**、**`deadline_missed_count`**（逐轴各一份，取首轴） |
| `axes[].runtime` | `max_observed_following_error_counts` `target_position_readback` `rx_error_counter` 等 |
| `axes[].shutdown_al_entry` | `state`（数「在 OP」用） |
| `axes[].startup_cia402` | 主站接手时驱动器的状态字与禁用写 |
| `axes[].shutdown_cia402` | 主站退出后驱动器的状态字与禁用写 |

`report_row.py` 里缺字段填 `-` 而不是 0：**0 是「量到的是 0」，`-` 是「这份报告里没有」**。

### 7.2 读法陷阱

1. **`wkc_no_frame` 与 `wkc_mismatch` 是两种故障。** 前者＝**一个回帧都没有**（链路／调度侧）；后者里的「计数变短但大于 0」＝**从站不在 OP**（从站侧）。两个数**相等**说明丢的是整帧；**不等**，差的那些就是从站侧掉的。
2. **`AL state=0` 不是「在 INIT」。** SOEM 的 `ecx_readstate` 先把缓冲清零再读，所以 0 只可能是**这一站没应答**。8=OP、20=SAFE-OP 加错误位、1=INIT、**0=没读回来**。
3. **收尾那句 `OP=通过` 走地址 0 广播，只照见环上最后一站。** 判「哪几根不在 OP」要看逐轴快照（`shutdown_al_entry`）。
4. **`index.tsv` 的列数随工具版本变化。** 旧系列 28 列（字段 18 是 `frame_interval_max`，纳秒），新版 32 列（字段 18 是 `first_mismatch_ex`，交换号）。**混着统计会得到假数据。** 统计前先看表头，或先跑 `series_index.py` 重建。

## 8 远程操作（从 Windows 跑 Pi）

```bash
python scripts/pi.py run '<命令>'          # 执行并回显
python scripts/pi.py run --sudo '<命令>'
python scripts/pi.py upload <本地> <远端>
python scripts/pi.py download <远端> <本地>
```

连接参数走环境变量，**不写死**：`EMASTER_PI_HOST`（默认 `192.168.124.81`）、`EMASTER_PI_USER`（`orangepi`）、`EMASTER_PI_PASSWORD`。
**注意 `pi.py run` 的工作目录是家目录**，不是仓库——脚本里要自己 `cd`。

## 9 其它命令行工具

| 工具 | 说明 |
| --- | --- |
| `emaster-scan` | 只读扫描：逐台身份、AL 状态、`0x6041`、故障码、换算 |
| `emaster-disable` | **走邮箱把驱动器失能**（主站起不来时也能用） |
| `emaster-watch [间隔ms] --deployment <ID>` | 实时监控位置／速度／力矩／状态字 |
| `emaster-move-deg --deployment <ID>` | 交互式角度控制（自动换算） |
| `emaster-move --deployment <ID>` | 交互式脉冲控制（底层接口） |
| `emaster-fingerprint` | 硬件指纹（最高只到 PRE-OP） |
| `emaster-dc-prepare` | PRE-OP DC 准备（不改状态） |
| `emaster-target-probe` | 目标源探针 |

都在 `build/tools/<目录>/` 下。

**`emaster-scan` 会把驱动器拉出 OP 再恢复**（它要发 INIT→PRE-OP 请求），**运行中不要用它**；**`emaster-disable` 会让电机失去保持力矩。**

## 10 单位换算（当前台架）

- 编码器分辨率：16384 脉冲/转（电机侧）
- 齿轮比：**第 8、12 席 25:1，其余 28:1**
- 负载侧分辨率：458752 脉冲/转（28:1）
- **1 度 = 1274.31 脉冲**（28:1）

单步上限由运动方案的 `max_step_millidegrees` 决定，取值看 `topology` 回报的 `max_step`。
**单步超限不会返回错误，而是判定 `MOTION_INVALID` 后中止整个会话、轴失能** —— 所以客户端要把大角度拆成小步流式发送。

超过 `external_target_timeout_ms`（台架 200 ms）没收到新目标，主站自动切到 HOLD 模式。

## 11 调试手册

这一节回答"手里的工具怎么用来回答一个具体问题"。全部只读或只用配置，不改主逻辑。

### 11.1 读一台驱动器的全部运行期读数（最快）

```bash
sudo bash -c 'ulimit -r unlimited; exec env EMASTER_OBSERVER_VERBOSE=1 EMASTER_OBSERVATION=1 \
  ./build/tools/master/emaster-master --deployment <部署ID>'
```

观测线程每约 1 秒在 stderr 打一条，含**全部 12 项**慢速遥测（当前值 + 单位）：

```text
[P4.3] 轴0: actual_current=9 0.001 A, cia402_error_code=0, bus_voltage=48104 mV,
       mosfet_temperature=391 0.1 degC, motor_temperature=344 0.1 degC,
       actual_motor_speed=0 rpm, speed_command=0 rpm,
       target_position_readback=3895 count, max_torque=1000, max_motor_speed=841045  (读取次数=221)
```

- **这些是 OP（CSP）运行期间的 SDO 读数**，不是停机快照
- 清单在设备配置的 `slow_telemetry`（`config/devices/cyberbeast_*.json`）
- 加了这个开关还会打出每条读数的对象地址与 wkc，用来确认"到底读到没有"

### 11.2 按阶段读指定对象

```bash
sudo python3 scripts/analysis/extract_report.py runtime/reports/<部署>-latest.json |
  grep -E '^axis[0-9]+\.(status_word|mode_display|target_position)\.'
```

```text
axis1.status_word.cyclic_operation=4663
axis1.status_word.cyclic_operation.hex=0x1237
axis1.mode_display.cyclic_operation=8
axis1.target_position.cyclic_operation=462644
```

键的形状：`axis<N>.<对象名>.<阶段>=<十进制>`，另有 `.hex=<原始十六进制>`。

**为什么必须按阶段看**：`0x6041` 在停机后 bit2／bit10 会掉。运行期是 `0x1237`、停机后是 `0x1233` —— 只看最后一条会把停机态当成运行态。

**读法边界**：某个对象没有 `cyclic_operation` 这一项，说明**这份二进制在 OP 期间没有读它**（审计段记录的是主站真的读过的对象），**不是"值没变"**。

### 11.3 状态字 `0x6041` 怎么读

| 位 | 含义 | 运行期典型 |
| --- | --- | --- |
| bit2 | 运行使能 | 1 |
| bit3 | 故障 | 0 |
| bit10 | 目标到达 | 1 |
| bit11 | **内部限幅生效** | 0 |

- `0x1237` = 使能、无故障、目标到达、限幅未生效
- `0x1233` = 同上但 bit2／bit10 为 0（SAFE-OP 或停机后）
- `0x1250` = Switch on disabled（已失能）
- `0x1210` = Not ready to switch on —— **驱动器没被主站配置时的正常静止态**

**译名依据只有 CiA 402 标准，没有厂商状态表佐证**（见第 II 部分 §19）。

### 11.4 单轴档的护栏比多轴档紧得多 —— 先拿它验可疑的驱动器

| 档 | `max_following_error_millidegrees` |
| --- | --- |
| 单轴 `orangepi-current-bench` | **2000（2°）** |
| 多轴各档 | **50000（50°）** |

**后果**：同一根"收到目标却不执行"的轴 ——

- 在多轴档上：一步不走也够不到 50°，**报告显示成功**
- 在单轴档上：**0.4 秒就按 `FOLLOWING_ERROR` 中止**

### 11.5 freerun（不用 DC）档：把同步从嫌疑里划掉

```bash
sudo env DEPLOYMENT=orangepi-bench-single-freerun DURATION=40 TRAVERSE=5 DEGREES=30 \
  RATE=100 ONLY_AXIS=0 OBSERVATION=1 TAG=fr30 bash scripts/bench_reciprocate_30deg.sh
```

配置已备好（`config/operation_profiles/cyberbeast_freerun_csp_1ms.json`，`strategy: "sm"`、`assign_activate: 0x00000000`、`sm2/sm3_sync_type: 0x0000`），**不需要改代码**。同一个现象在 DC 与 freerun 下逐项一致，就把 DC／SYNC0 从嫌疑里划掉了。

### 11.6 "轴不动"的排除表

| 假设 | 拿什么排除 |
| --- | --- |
| 主站没发目标 | 命令被拒 0 条、单步 < `max_step`、`0x607A` 读回 == 主站写的值 |
| 状态机没使能 | `0x6041` 运行期 bit2=1、bit3=0；控制字 `0x000F` |
| 限幅卡住 | OP 期间 `0x6072`／`0x6080` 的值、`0x6041` bit11=0 |
| 驱动器报故障 | `0x6041` bit3=0、`603F=0x0000`、`fault_timeline` 四项全 false |
| 过热／母线 | `slow_telemetry` 的温度与母线电压 |
| 链路／CRC | `rx_error_counter` 全 0、期望 WKC == 实际 WKC |
| DC／SYNC0 同步 | 同一现象在 freerun 档下复现（§11.5） |

## 12 踩过的坑

### 12.1 旧桥占着端口 → 界面"连上了但什么都不动"

```text
OSError: [Errno 98] Address already in use
LISTEN 192.168.124.81:5001   ← 上一次留下的桥
```

那个桥指向的是**别的部署**的套接字，界面接上去看着正常，实际什么都没接上。**`pkill` 要带 `sudo`** —— 不是 root 就杀不掉 root 起的进程，而且是静默失败。

### 12.2 换设备被拒时，先分清"读失败"还是"型号不对"

拒绝的日志只有一句：`CiA 402 周期控制会话失败：物理从站身份或 PDO 映射与部署配置不一致，拒绝发布指纹。`

**这句话把两种完全不同的情况说成了一句：**

| 报告里 `identity.observed` | 是什么 | 该去哪 |
| --- | --- | --- |
| 三对全 `0x00000000` | **EEPROM／SII 读失败** | 查那台设备的供电／上电时序／接线 |
| 是个真值但与 `expected` 不同 | **真的型号不对** | 这才该动配置 |

**"主站拒绝运行"看起来像"系统故障"。** 汇总表里两者长得一样 —— 逐轮回看日志才能分开。

### 12.3 `job_kill` 停不了远端的工作

从 Windows 用 `pi.py run` 起的命令，本地包装被取消**不会**停掉板子上的进程。停远端要用 `sudo kill <PID>`（进程是 root 的，不带 sudo 会**静默失败**），而且要**先杀驱动脚本、再让当前轮走完自己的停机** —— 直接 `-9` 会把驱动器留在使能状态。

### 12.4 离线干跑只验装配，不验真报告

`bench_series.sh` 用 `ROUNDS=0` 可以在**不碰总线**的前提下验完参数回显、报告路径解析、证据目录与表头。但**它验不到字段取对了没有** —— `report_row.py` 就出过一次两列取错层、安静退化成 `-` 的事，是在看真实输出时才发现的。

### 12.5 空转脚本必须盯着主站

空转窗口里主站可能自己退出（实测第 227 秒因 WKC 崩塌退出过一次）。脚本若只 `sleep` 满整段时间，就**既不报错也不落盘**，从外面看是卡死。现在的脚本逐秒检查 `kill -0`，并记下**实到秒数** —— "计划 900、实到 227"这个差值本身就是最有用的读数。


---

# 第 II 部分 · 项目实现

## 13 项目目标与工程规范

**最终目标**：开发一个供机器人控制使用的功能完善的 EtherCAT 主站。

- **机器人控制**：主站接收外部控制器的实时位置指令，周期性发送给 EtherCAT 从站，读取反馈并提供给外部控制器
- **功能完善**：满足实时性要求、支持多轴同步、处理状态管理和故障检测、提供监控和安全机制

**工程性规范**（硬约束，不是偏好）：

| 条 | 内容 |
| --- | --- |
| 不临时性 | 代码和配置是持久的、可维护的，**不创建临时脚本或测试文件** |
| 中文 | 代码注释用中文，Git 提交信息用中文说明改了什么、为什么 |
| 解耦 | 模块职责清晰，按功能划分，避免跨层依赖 |
| 不绑主机 | 不硬编码主机名、路径、网卡名称 |
| **配置驱动** | 参数通过配置文件管理，**代码不包含硬编码的业务参数** |
| 启动简洁 | 启动命令不堆砌参数，不在命令行传递大量参数 |
| 验证 | 通过真实硬件验证；**不维护独立的测试程序、不写专门的测试文档** |
| 版本管理 | 所有代码和配置提交到版本库，提交信息说明改动内容和原因 |
| 原子问题 | 任务分解到不可再分的最小单元；顺序服从依赖逻辑，无依赖时功能优先 |
| **正向功能优先** | 先实现理想环境下的完整功能；错误／边界处理除最基本的，放在"完成"之后 |

## 14 架构与所有权

目录按职责组织，CMake target 约束依赖。**公共接口不暴露 SOEM 类型**，业务参数只来自配置。

**协议标准索引和数据类型属于实现知识；设备特性、拓扑数量、时序和运动限值属于配置。**

```text
部署 + 设备 + 拓扑 + 运行方案 + 运动方案
                    |
                会话计划
                    |
        总线配置 -> 周期启动 -> 周期控制 -> 退出
                    |           |
                  PDO编解码    CiA 402 + 多轴协调 + 轨迹
                    |
                 执行记录 -> 报告与中文展示
```

| 所有者 | 职责 |
| --- | --- |
| `config/`、`src/config/`、`src/catalog/` | 配置输入及其构建期生成目录 |
| `src/session/` | 按部署组合轴计划，检查实际布局和模式条件 |
| `src/protocol/` | 通用 PDO 发现模型、字段位置和整数编解码 |
| `src/cia402/` | 根据状态字与控制目标生成控制字，**不访问总线或时钟** |
| `src/multiaxis/` | 完整轴集合、序号与截止时间检查 |
| `src/motion/` | 基于各轴真实换算因子的相对轨迹、速度轨迹和位置目标转换 |
| `src/bus/soem/` | **唯一 SOEM 集成边界**，串行完成网络收发及状态切换 |
| `src/audit/` | 保存访问事实、连续同值合并、退出后序列化 |
| `tools/master/`、`src/messages/` | 入口、信号处理、报告发布和中文展示 |

`src/cyclic/` 保留总线无关的传输回调契约，`src/safety/` 保留通用条件汇总；**不能把库存在等同于当前会话已经接入其全部能力。**

## 15 主线：从配置到周期

**核心理念：扫描结果是权威，配置文件表达意图。**

```text
配置文件（期望） → 扫描总线（真相） → 身份匹配映射 → 运行时API → 工具查询适配
```

会话启动分六个阶段（`include/emaster/audit/run_audit.h:9-17`）：

| 阶段 | 做什么 |
| --- | --- |
| `PREOP_CONFIGURATION` | 等全体到 PRE-OP → 读换算 SDO → 选 PDO 布局 → 分配 PDO → 配 DC |
| `SAFEOP_INITIALIZATION` | 进 SAFE-OP，读回实际位置作为起点（**此时输出不生效**） |
| `OPERATION_REQUEST` | 请求 OP |
| `CYCLIC_OPERATION` | 1 ms 周期交换 |
| `SAFE_STOP` | 安全停机 |
| `FINAL_DIAGNOSTIC` | 退出 OP 后走邮箱读诊断 |

**每一步的动作、现象、以及"依据什么往下走"，见 [`wholetest/采集.md`](wholetest/采集.md) 第 2–3 节。**

## 16 总线内部结构

`control_session.c` 只分配资源、调用各生命周期阶段并释放资源。`session_internal.h` 是总线层私有的会话存储，**不跨越 SOEM 边界**。

| 文件 | 执行阶段与职责 |
| --- | --- |
| `session_setup.c` | 发现、身份核对、PDO 选择和映像、SDO 初始化、DC 与 SAFE-OP |
| `session_start.c` | 初始化输出，锁定实际位置，建立周期、启动参考时钟跟随并请求 OP |
| `session_exchange.c` | 定时、SOEM 过程数据交换、WKC 检查、DC 反馈和首次异常记录 |
| `session_control.c` | 解码反馈，调用独立控制器和轨迹模块，生成下一周期全轴输出 |
| `session_shutdown.c` | 逐级撤销使能并确认完整 CiA 402 停用状态，采集 AL 状态、关闭 Sync0、诊断并恢复 INIT |
| `cycle_clock.c` | Linux 单调时钟截止点与参考 DC 相位的 PI 修正 |
| `cia_process_image.c` | 已核对 PDO 布局的字段绑定、缓冲区编解码和直接索引审计 |
| `session_observer.c` | 类型明确的只读诊断及审计容量准备 |

总线层负责生命周期编排，**不复制 CiA 402 状态规则或轨迹算法**。不得为每台电机创建一套会话实现。

## 17 周期与并发

主线程按**绝对截止点**唤醒后立即发送，接收完成后再记录审计、计算相位修正、处理反馈与生成下一帧。参数化的过程帧相位与 Sync0 偏移具有不同意义，**周期线程不能以"固定休眠 1 ms"代替相位跟随**。期限已错过时退出正常控制，**不连续补发历史帧，不自动放宽 WKC 判据**。

PDO 审计游标按实际字段数量分配，每个字段直接查找上次记录。周期前预留并触及记录存储，**周期中不扩容、不写文件**；容量耗尽后明确记录截断。**周期线程不执行邮箱访问。**

每个物理从站还分别聚合主机单调时间、参考 DC 时间、发送/往返耗时、发送迟到量和距 Sync0 的理论裕量；这些数值用于定位时序，**不能替代从站提供的 SM2 完成时间戳**。

动态 PDO 直接提供模式反馈。固定 PDO 缺少模式反馈时，在 SAFE-OP 初始化阶段通过 SOEM SDO API 读回一次并在报告中保留；**OP 周期不再访问邮箱**，避免 SDO 与过程数据争用同一 SOEM 上下文。

**当前控制会话只实现 CSP 的位置目标语义**；设备目录声明的 CSV/CST 能力不会被伪装成可运行控制模式，在对应目标量、反馈量、限幅和上层命令模型接入前，选择这些模式会被会话计划拒绝。

截止时间检查和 WKC 检查属于执行条件。诊断性模式反馈策略**只允许保持位置**；配置了变化轨迹的轴必须匹配请求模式后才开始运动。**所有轴在同一次协调步骤中处理，不发布部分目标集合。**

## 18 位置命令入口

CSP 的最小产品入口必须把**位置目标输入**和**周期 PDO 发送**分开。应用层通过 `emaster_control_session_callbacks_t.position_target_source` 提交完整轴集合的目标位置，主站周期线程只接受明确的当前目标并检查已读回的软件位置范围；**目标单位是已按当前从站换算完成的原始位置计数**，单位换算、绝对零点和轨迹限制由独立的上层命令或轨迹模块负责。

位置目标来源回调只提供当前周期的完整目标快照，返回 `HOLD` 时保持上一目标，返回 `UPDATED` 时由主站检查所有轴的已读回软件位置范围后写入下一帧 CSP PDO。回调为空时仍使用部署引用的固定运动方案，因此 `CSP36° 成功` 基线的行为不变；**固定方案只是一个命令来源，不是最终产品的上层接口**。

序号、时间戳和外部传输协议不属于这个最小周期目标契约，待上层接口确定后再单独接入，**不得在本层预设输入频率**。


## 19 两层状态：AL 与 CiA 402

**这是本项目最容易搞错的地方，单独成节。**

| | EtherCAT AL（ESC 层） | CiA 402（驱动器层） |
| --- | --- | --- |
| 载体 | 寄存器 `0x0120` / `0x0130` / `0x0134` | 对象 `0x6040` / `0x6041` / `0x603F` |
| 状态 | INIT 1｜PRE-OP 2｜SAFE-OP 4｜OP 8 | 八个状态，见 `include/emaster/cia/controller.h:10-18` |
| 通道 | INIT 无邮箱无 PDO｜PRE-OP 有邮箱｜SAFE-OP 有输入无输出｜OP 双向 | 邮箱（PRE-OP 及以上）或 PDO（仅 OP） |
| 谁管 | 主站的状态请求 | 驱动器的内部状态机 |

**两层互不决定。** 实测（2026-10-08）：AL 状态已是 **INIT**，而 12 台驱动器的 `0x6041` 全是 **`0x1237`**（Operation enabled + Voltage enabled）—— 电机保持着力矩，而总线这一层早已退出 OP。

**四条推论，都吃过亏：**

1. **不能用 AL 状态推断驱动器使不使能**，两个都要读
2. **不能用 PDO 去送"停止周期通道"的命令** —— 从站掉出 OP 后控制字就到不了它，**禁用必须走邮箱**
3. **AL 在 INIT 时读不到 `0x6041`**（邮箱关闭），这时只能说"读不到"
4. **`0x0134` 只在 `0x0130` 的 bit4（Error Indicator）置起时才有意义**；没有 error 位时读它等于没读。SOEM 只在看到 error 位时才去读，这是照规范做

**对象编号的权威链**：CiA 402（IEC 61800-7-201）规定编号 → 厂商在 ESI/EDS 里声明（`docs/lz-joint/ds402.eds`、`ECAT_CIA402.xml`）→ 本项目在 `config/devices/` 的 PDO 映射表里引用 → 主站按表建过程映像。

**状态译名的依据只有标准**，没有厂商状态表佐证 —— 厂商那张状态图是图片，当前模型读不了；手册文字部分给的位名与标准不同，且与它自己的读数不自洽。

**为什么 AL 状态码不能当"为什么失败"的入口**：它满足的是"这次请求的转移失败了"，而 error 位没置起就说明从站不认为它拒绝过。**要问"请求到底发到没有"，看的是写操作的工作计数器，不是状态码。**

## 20 失败与报告

- **首次周期失败**保留状态、阶段、交换号、WKC 是否取得及最后参考时间，**不被后续停机交换覆盖**
- **退出时的 AL 状态是退出快照，不冒充失败瞬间**；过程数据访问成功也不等于电机完成指令
- 停机尝试之后，先退出 OP，再关闭 Sync0 和读取诊断。报告以 `post_cycle_diagnostic` 标明阶段，**不由末尾计数器反推首次异常顺序**
- 退出状态切换失败时跳过批量 SDO，继续关闭总线，**绝不把缺少反馈当作停用成功**
- 报告文件在周期结束后**原子发布**。中文正文由消息目录管理，执行模块只返回结构化结果

**报告里与状态有关的段**：

| 段 | 回答 |
| --- | --- |
| `startup_cia402` | **主站接手时，驱动器在不在使劲** |
| `shutdown_cia402` | **主站退出后，它还在不在使劲** |
| `shutdown_al_entry` / `_pre_stop` / `_exit` | ESC 层三个坐标的 AL 状态与状态码 |
| `timing_excursions` | 每一拍时序越界的分布 |
| `cycle_trace` | 首个异常前后各 200 拍的逐拍记录 |

**报告里没有的**：温度、母线电压、电流（走观测通道，主站一退就读不到）；`0x0134`（只在 error 位置起时才读）。

## 21 错误恢复策略

**阈值不写在代码里**，读 `config/error_recovery_policies/<id>.json`（`default` / `strict` / `tolerant`），由部署档的 `error_recovery_policy_id` 引用。策略未找到时用保守默认（**首次错误即停机**）。

| 块 | 默认 | 含义 |
| --- | --- | --- |
| `wkc_recovery` | **连续 5**、60 秒窗口内 50 | 短帧 |
| `no_frame_recovery` | 同上 | 整帧未回 |
| `deadline_recovery` | 连续 10 | 周期截止点错失 |
| `al_state` / `cia402_fault` / `dc_late` | `enabled=false` | 未启用 |

**判据是"连续"，不是"累计"。** 实测里"散落 2 次"跑完、"连续 5 次"中止，区别只在连续数上。**连续段是联合的**（整帧未回也计入连续），**窗口是分类的**（各推各的）—— 所以 `wkc_short_window_max` 可能小于 `wkc_max_consecutive`，那是两个口径，不是矛盾。

**累计量按类取**：整帧缺失用自己的累计，短帧用"联合累计 − 整帧缺失累计"。`wkc_*` 三个字段的语义不变，仍是联合计数。

滑动窗口在会话建立时就建好，**不等到第一次出错时才惰性初始化** —— 那次初始化恰好发生在已经出错、最不该多做事的时刻。

**`no_frame_recovery` 未另设 `enabled`，沿用 `wkc_recovery.enabled`** —— 避免出现"wkc 恢复开着、整帧缺失却立刻停机"这种组合。

## 22 配置体系

**五个维度**，全部在 `config/` 下：

| 目录 | 份数 | 内容 |
| --- | ---: | --- |
| `deployments/` | 18 | 部署：轴数、引用哪套拓扑／设备／运行方案／运动方案、报告路径、容错策略 |
| `topologies/` | 13 | 拓扑：环上有哪些从站、顺序、每台的身份期望与位置 |
| `devices/` | 1 | 设备：身份、PDO 映射、`slow_telemetry` 清单、状态字掩码 |
| `operation_profiles/` | 6 | 运行方案：周期、DC 策略（`dc`/`sm`）、SM2/SM3 同步类型、相位与偏移 |
| `motion_profiles/` | 16 | 运动方案：单步上限、跟随误差限幅、轨迹 |
| `error_recovery_policies/` | 3 | 容错阈值 |
| `messages/zh_CN.json` | 1 | 中文文案 |

**代码里没有硬编码的业务参数**：轴数来自部署档，换算来自 SDO 实测，阈值来自策略文件。

## 23 观测通道

**独立的只读 unix 套接字**（`/tmp/emaster-<部署>-obs.sock`），**要 `EMASTER_OBSERVATION=1` 才存在**。

- 观测线程做**邮箱 SDO 往返**，读的是设备配置里 `slow_telemetry` 列的对象（电流 `6078h`、母线电压 `6079h`、温度 `200Bh:01h`／`200Bh:02h`、转速、读回目标、限幅等）
- **周期线程不执行邮箱访问**；观测在周期外读，周期线程只做加锁拷贝
- 停机序言要停观测线程（`pthread_join`）。**停止标志必须每次邮箱读之前检查**，邮箱超时也要收紧 —— 否则 join 会等完整一轮，那段窗口主线程一个过程数据帧都不发，**驱动器会因缺帧掉出 OP**。这个缺陷在台架上复现并修过：join 耗时从数毫秒降到 **0.064 ms**
- 停机序言的 AL 快照与 join 耗时写进报告（`shutdown_al_entry` / `shutdown_al_pre_stop` / `shutdown_observer_join_ns`）

**为什么电流／电压／温度走 SDO 而不进 PDO**：ESI 把对应模块声明为 `Fixed="true"` 且 `supports_pdo_configuration=false`，无法映射进 TxPDO，只能走慢速通道（约 3 ms 往返）。

## 24 DC 与同步

- 周期线程按**绝对截止点**唤醒并立即发送，参数化的**过程帧相位**与 **Sync0 偏移**是两件事
- 参考时钟跟随用 PI 修正（`cycle_clock.c`）
- 每个从站聚合**发送迟到量**与**距 Sync0 的理论裕量**，用于定位时序
- 错误计数器用 `FPRD(0x0300)` 读，**抽样**（每 50 个交换一次）+ **承压跳过**（本周期 WKC 已经不符、或收包段耗时超过 150 µs 时不读）
- 跳过与读失败分开记账：`error_counter_read_attempt_count` / `error_counter_skip_count` / `first_error_counter_skip_exchange`；`error_counter_read_fail_count` 从此只表示"想读但没读到"
- 单周期阻塞预算：诊断读的每轴超时从剩余预算里除出来（`(周期 − 帧超时) ÷ 轴数`，上限 250 µs，低于 50 µs 则整块跳过），算出的最坏值写进报告 `tail_blocking_budget_ns`

**freerun（`strategy: "sm"`）**：`assign_activate = 0x00000000`、`sm2/sm3_sync_type = 0x0000`，整段 DC 配置跳过。用途是把同步从嫌疑里划掉（§11.5）。**SM 只要求四个参数**：`assign_activate`、`cycle_ns`、`sm2_sync_type`、`sm3_sync_type`。

供应商要求 SM2 过程数据事件先于 Sync0，并建议 Sync0 偏移为周期的四分之一。当前方案计划在约 **+50 µs** 发送，Sync0 位于约 **+250 µs**，保留至少 **100 µs** 的最坏情况保护时间。


## 25 安全边界

**本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。

- **指纹工具和 PRE-OP DC 准备工具的最高 AL 状态仍为 PRE-OP**，分别只执行受限的只读采集或参数准备
- **`emaster-master` 不在 PRE-OP 指纹流程范围内**：它会请求 SAFE-OP/OP 并把驱动器推进到运行使能
- 进入 CSP 前以 `6064` 实际位置初始化 `607A`
- 停止和失败路径按状态反馈**逐级撤销使能**，并以设备配置中的 `6041` 掩码和值确认停用，随后关闭 Sync0 并恢复 INIT
- **获得 PRE-OP 指纹运行许可，不代表获得 SAFE-OP、OP、CiA 402 使能或运动授权。** 每个更高边界都需要独立流程和批准

**不变量（适用于所有后续阶段）：**

- 任何过程数据交换开始前，启动输出必须完全初始化
- **硬件使能是生命周期状态，不能是编译开关**
- 网络进入更高状态前，拓扑和身份必须匹配
- 命令帧必须携带序号和失效时间
- 通信丢失、命令过期、反馈无效和进程退出都必须有明确停机反应
- 恢复过程必须有界、可记录，且**不能静默复位驱动器故障**
- 单轴和整机停机策略必须明确且可测试
- **日志和诊断禁止在实时路径中执行**

**每份硬件测试记录必须说明**：操作者、观察者、物理装置、功率限制、机械约束、STO/急停验证、网络接口、软件版本、配置哈希、ESI 哈希和全部从站序列号。

## 26 实时基线

### 主机要求

- 使用 PREEMPT_RT 内核，并记录内核与配置哈希
- 服务账号具备 `SCHED_FIFO` 权限，进程内存锁定
- 周期线程使用**专用 CPU**，EtherCAT 网卡 IRQ 亲和性经过明确配置
- EtherCAT 使用**专用网卡**，网卡无 IP、路由、PTP、激光雷达或 NetworkManager 业务
- 网卡中断合并和卸载参数已记录并经过有意配置
- CPU 频率策略、温度限制和后台服务负载已记录

### 1 ms 指标

| 指标 | 门槛 |
| --- | ---: |
| 标称周期 | 1,000,000 ns |
| 唤醒延迟 p99.9 | ≤ 20 µs |
| 唤醒延迟 p99.999 | ≤ 50 µs |
| 实测最大唤醒延迟 | ≤ 100 µs |
| 周期执行时间 p99.999 | ≤ 250 µs |
| 实测最大周期执行时间 | ≤ 400 µs |
| 1 ms 截止时间错失 | 8 小时门槛测试中为 0 |
| 有效过程数据 WKC | 每个稳态周期都精确等于预期值 |
| 丢帧/迟帧策略 | 首个无效周期即进入故障，禁止静默继续 |

供应商给出的最小周期（单从站 41.1 µs、五从站 46.5 µs）**只是实验室设备能力**，不能证明 Orange Pi 主机或 12 轴网络满足上述指标。

**必须记录原始直方图和极值，不能只记录平均值。** 实时报告必须绑定内核、CPU/IRQ 亲和性、网卡驱动/固件、卸载设置、拓扑、从站固件、构建版本、编译参数、测试时长和后台负载，并说明时钟源与测量方法。

> **当前台架的实际状况**（实测，见 [`wholetest/采集.md`](wholetest/采集.md) §5 T1）：
> 实时核上有 21 个线程，`interrupts_on_rt_core` 中 `IPI1` 占 52.7%，网卡中断全在 CPU 0，`irqbalance` 未安装，32 个网卡队列只用了一个。
> **上面 7 条主机要求里有 4 条明显未满足。**

## 27 功能状态

| 优先级 | 内容 | 状态 |
| --- | --- | --- |
| **P0** 基础通信与控制 | 设备发现、PDO 映射、DC 同步、WKC 验证；CiA 402 到 OPERATIONAL ENABLED、CSP、位置目标接口、反馈；软限位、跟随误差、故障锁存、安全停机 | **完成** |
| **P1** 核心监控与保护 | 电流 `6078h`／母线电压 `6079h`／温度 `200Bh` 监控、位置速度力矩反馈、跟随误差监控、WKC 错误、deadline 检测 | **完成** |
| | 碰撞／卡死／过载／过热／欠压过压／编码器故障检测；命令队列管理 | **未实现** |
| **P2** 多模式 | CSP | **完成** |
| | CSV／CST／混合／运行时切换 | 接口存在，**未真机验证** |
| **P3** 故障处理与恢复 | 正常停止 | **完成** |
| | 快速停止／急停；故障日志、清除流程、自动重试、降级运行 | **未实现** |
| **P4** 诊断 | 运行报告、命令服务器 | **完成** |
| | 周期时序详细统计、通信质量趋势、实时数据流、历史记录、性能分析 | **未实现** |
| **P5** 配置与扩展 | 多机器人配置管理 | **完成** |
| | 运行时配置调整、周期时间可调、轴数动态、热插拔、多主站协同 | **未实现** |
| **P6** 性能优化 | 零拷贝、多核利用、优先级调度、内存池 | **未实现** |

## 28 构建、目录与产物

```text
build/      构建产物（CMake）
runtime/    日常运行记录（报告按 run_report_path 落这里）
reports/    采集证据（脚本收进来的，入 git）
tmp/        临时调查
records/    装置指纹
docs/       厂商资料
wholetest/  采集记录
```

```bash
cmake -S . -B build && cmake --build build -j4
```

**不维护持久化测试程序、不创建独立测试工程；构建检查不能替代真机协议和运动验证。**

---

# 附：文档

**本项目只有两份文档：**

| 文件 | 内容 |
| --- | --- |
| `README.md` | 本文件：使用说明 + 项目实现 |
| [`wholetest/采集.md`](wholetest/采集.md) | 采集记录：跑了什么、怎么采的、得到了什么、还有什么没解决 |

厂商资料在 `docs/lz-joint/`（PDF 手册、EDS、ESI XML、状态图），**不属于项目文档**；其中两张图当前模型读不了（见采集文档 §2）。

