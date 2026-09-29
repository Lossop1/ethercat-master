# EtherCAT Master

工业级 EtherCAT 主站实现，基于 SOEM，支持 CiA 402 驱动器控制。

**本文件是操作台架的唯一入口**：起主站、跑测试、读报告、跨机界面、远程操作，以及踩过的坑。
设计与取舍的理由不在这里，在 `DEVELOPMENT.md` 与 `docs/`。

---

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

```
同机（都在 Pi 上）     客户端 ──unix──► /tmp/emaster-<部署>.sock
跨机（界面在 Windows） 界面 ──TCP:5001──► socket_bridge.py ──unix──► 命令口
                             ──TCP:5002──►              ──unix──► 观测口
从 Windows 跑 Pi 命令  pi.py ──SSH──► Pi
```

## 1 前提与约定

- **要 root**：主站要 `mlockall` + `SCHED_FIFO`，套接字也由 root 建。
- **要 `ulimit -r unlimited`**：不然实时优先级设不上（脚本里都带了）。
- **部署 ID 必须与物理轴数一致**：

  | 轴数 | 部署 ID |
  | --- | --- |
  | 2 | `orangepi-bench-dual-30deg` |
  | 3 | `orangepi-bench-triple-30deg` |
  | 4 | `orangepi-bench-quad-30deg` |
  | 5 | `orangepi-bench-quint-30deg` |
  | 6 | `orangepi-bench-sext-30deg` |
  | 7 | `orangepi-bench-sept-30deg` |
  | 8 | `orangepi-bench-oct-30deg` |
  | 9 | `orangepi-bench-non-30deg` |

  部署是**按位置**配的，所以**换同型号设备不用改命令**（见 §12.2）。

## 2 起主站

```bash
cd /home/orangepi/ethercat-master
sudo bash -c 'ulimit -r unlimited; exec env EMASTER_OBSERVATION=1 \\
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

界面在**另一台机器**上跑，主站和桥在 Pi 上。**Pi ①：主站** —— 见 §2。

**Pi ②：桥**

```bash
cd /home/orangepi/ethercat-master
sudo python3 tools/socket_bridge.py --deployment <部署ID> --bind 0.0.0.0 --port 5001
```

**另一台机器（Windows）：界面**

```powershell
cd D:\\taili\\ethercat_master
$env:PYTHONPATH='tools'
python -m emaster_gui --tcp 192.168.124.81:5001
```

- 端口约定：**命令 5001、观测 5002**（界面自动用 PORT+1，不用另给）
- `--tcp` **同时开了命令通道**，也就是界面能驱动电机。**只读**就换成 `--obs-endpoint tcp:192.168.124.81:5002`（不给命令口）
- 桥会自己绑到主站实时核**之外**的核（`tools/rt_affinity.py` 从 `/proc` 读主站掩码）
- 桥监听 `0.0.0.0` 会打一句警告：**这两个口没有任何认证，能连上命令口的人就能驱动电机**
- 界面参数看 `python -m emaster_gui --help`（`--jog-speed`、`--range-deg`、`--tick-hz` 等）

## 4 终端面板（同机）

```bash
sudo bash /home/orangepi/ethercat-master/scripts/console.sh --start --obs  # 没在跑就替你拉起来
sudo bash /home/orangepi/ethercat-master/scripts/console.sh                # 接上已在跑的主站
```

控制律只有一份实现（`tools/emaster_console.py` 的 Engine），终端面板与图形界面共用它。
参数：环境变量 `DEPLOYMENT`、`RANGE`（自助软范围，默认 45°）、`JOG_SPEED`（默认 10 °/s）。
**`--obs` 只对本次由它拉起的主站生效**——已经在跑的主站用的是它启动时那个环境。

## 5 单轮台架测试

### 5.1 空转（不发目标、不动轴）

```bash
sudo env DEPLOYMENT=<部署ID> IDLE=60 TAG=idle1 bash scripts/bench_idle.sh
```

用途：把主站单独放在总线上看它稳不稳。改线、扩轴、换设备之后的第一道关。

### 5.2 30 度往返

```bash
sudo env DEPLOYMENT=orangepi-bench-oct-30deg \\
  DURATION=60 TRAVERSE=5 DEGREES=30 RATE=100 WAVE=triangle \\
  ONLY_AXIS=0 OBSERVATION=1 TAG=recip30 \\
  bash scripts/bench_reciprocate_30deg.sh
```

长跑挂后台（脚本头部写的那条路）：

```bash
sudo bash -c 'nohup env DEPLOYMENT=orangepi-bench-oct-30deg DURATION=600 TRAVERSE=5 \\
  bash /home/orangepi/ethercat-master/scripts/bench_reciprocate_30deg.sh &'
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
| `REPORT_INTERVAL` | 10 | 客户端打印采样间隔（秒）；调小能看出「轴从哪一刻起不跟目标」 |

**退出码**：0=跑满且命令流连续；1=命令流出现过 200 ms 空档；2=**主站中途中断了命令流**；3=命令口 shutdown 没让主站退出（P12.1 判据）。

**产物**：`/tmp/<TAG>_outcome.txt`（全程输出）、`_done`（退出码）、`_master.log`、`_client.log`。报告在部署配置的 `run_report_path`。

## 6 多轮测试

```bash
cd /home/orangepi/ethercat-master
sudo env DEPLOYMENT=orangepi-bench-oct-30deg SERIES=<系列名> ROUNDS=6 GAP=60 \\
  MODE=reciprocate DURATION=60 TRAVERSE=5 DEGREES=30 RATE=100 ONLY_AXIS=0 OBSERVATION=1 \\
  bash scripts/bench_series.sh
```

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `DEPLOYMENT` | **必填** | 没有默认值：写默认部署等于把某个台架写进工具里 |
| `SERIES` | **必填** | 决定证据目录 `reports/_<SERIES>/`；**换个名字就不会覆盖上一批** |
| `ROUNDS` | 6 | 轮数 |
| `GAP` | 60 | 轮间间隔（秒）。**这个值本身是变量**：背靠背重启会不会让驱动器来不及就绪是待查项 |
| `MODE` | reciprocate | idle 或 reciprocate |

**其余参数原样传给单轮脚本**，这里不重复也不校验。

```
reports/_<SERIES>/
  index.tsv                  一行一轮（表头 = 本脚本的列 + report_row.py 的列）
  r01_<TAG>/
    <TAG>_master.log         主站日志
    <TAG>_outcome.txt        单轮脚本全程输出
    <TAG>_client.log         客户端日志
    summary.txt              report_summary.py 对该轮报告的输出（人读）
    row.tsv                  该轮在 index.tsv 里那一行
    exit-code.txt
    report-name.txt          该轮完整报告在归档里的文件名 ← **完整证据的坐标**
```

完整报告几十到几百 MB，**不入库**，留在 `run_report_path` 所在目录的 `archive/` 下。
**退出码 0 只表示轮次跑完了，不代表每轮都成功**——成败看 `index.tsv`。

## 7 读报告

报告是**单行巨型 JSON**（空转 20 MB，运动轮 500 MB 量级），而且是**覆盖式**的：下一轮一开跑，上一轮的就没了（主站会往 `archive/` 留副本）。

| 工具 | 用途 |
| --- | --- |
| `scripts/analysis/report_summary.py <报告>` | **人读**：跑了多久、故障形态、故障点逐轴 AL、停机收尾 |
| `scripts/analysis/report_row.py <报告> [标签]` | **机读**：一份报告压成 TSV 一行；`--header` 打印列名 |
| `scripts/analysis/series_index.py reports/_<系列>` | 按每轮 `report-name.txt` **从归档重建整张表** |
| `scripts/analysis/extract_report.py <报告>` | 就地抽关键字段（不下载整个 JSON） |

### 7.1 字段住在哪一层（查过的，别猜）

取错层不会报错，只会安静地退化成 `-`。

| 层 | 字段 |
| --- | --- |
| `result` | `cycle_count` `expected_wkc` `actual_wkc` `op_reached` `all_axes_enabled_reached` `motion_completed` `wkc_error_count` `wkc_no_frame_count` |
| `process_data_delivery` | `over_budget_cycle_count` `frame_interval_gap_count` `tail_max_receive_ns` `tail_max_receive_ok_ns` `tail_max_error_counter_ns` `error_counter_read_fail_count` `first_mismatch_exchange` |
| `axes[].timing` | **`wkc_mismatch_count`**、**`deadline_missed_count`**（逐轴各一份，取首轴） |
| `axes[].runtime` | `max_observed_following_error_counts` `target_position_readback` `rx_error_counter` 等 |
| `axes[].shutdown_al_entry` | `state`（数「在 OP」用） |

`report_row.py` 里缺字段填 `-` 而不是 0：**0 是「量到的是 0」，`-` 是「这份报告里没有」**。

### 7.2 三个读法陷阱

1. **`wkc_no_frame` 与 `wkc_mismatch` 是两种故障。** 前者＝**一个回帧都没有**（链路／调度侧）；后者里的「计数变短但大于 0」＝**从站不在 OP**（从站侧）。两个数**相等**说明丢的是整帧；**不等**，差的那些就是从站侧掉的。
2. **`AL state=0` 不是「在 INIT」。** SOEM 的 `ecx_readstate` 先把缓冲清零再读（`ec_main.c` 的 984 与 1000 行），所以 0 只可能是**这一站没应答**。8=OP、20=SAFE-OP 加错误位、1=INIT、**0=没读回来**——这三者的下一步完全不同。
3. **收尾那句 `OP=通过` 走地址 0 广播，只照见环上最后一站。** 一根掉队会把整环读成通过或失败。判「哪几根不在 OP」要看逐轴快照（`shutdown_al_entry`）。

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
| `build/tools/emaster_watch/emaster-watch [间隔ms] --deployment <ID>` | 实时监控位置／速度／力矩／状态字 |
| `build/tools/emaster_move_deg/emaster-move-deg --deployment <ID>` | 交互式角度控制（自动换算） |
| `build/tools/emaster_move/emaster-move --deployment <ID>` | 交互式脉冲控制（底层接口） |
| `tools/interactive_control.py <套接字路径>` | 交互式位置控制客户端（较老；日常用 console.sh 或界面） |
| `build/tools/target_probe/emaster-target-probe` | 目标源探针 |
| `build/tools/fingerprint/emaster-fingerprint` | 硬件指纹 |

## 10 单位换算（当前台架）

- 编码器分辨率：16384 脉冲/转（电机侧）
- 齿轮比：28:1
- 负载侧分辨率：458752 脉冲/转
- **1 度 = 1274.31 脉冲**

单步上限由运动方案的 `max_step_millidegrees` 决定，取值看 `topology` 回报的 `max_step`。
**单步超限不会返回错误，而是判定 `MOTION_INVALID` 后中止整个会话、轴失能**——所以客户端要把大角度拆成小步流式发送。

超过 `external_target_timeout_ms`（台架 200 ms）没收到新目标，主站自动切到 HOLD 模式。

## 11 系统架构

```
配置文件（期望） → 扫描总线（真相） → 身份匹配映射 → 运行时API → 工具查询适配
```

**核心理念**：扫描结果是权威，配置文件表达意图。

## 12 踩过的坑

### 12.1 旧桥占着端口 → 界面「连上了但什么都不动」

```
OSError: [Errno 98] Address already in use
LISTEN 192.168.124.81:5001   ← 上一次留下的桥
```

那个桥指向的是**别的部署**的套接字，界面接上去看着正常，实际什么都没接上。**`pkill` 要带 `sudo`**——不是 root 就杀不掉 root 起的进程，而且是静默失败。

### 12.2 换设备被拒时，先分清「读失败」还是「型号不对」

拒绝的日志只有一句：`CiA 402 周期控制会话失败：物理从站身份或 PDO 映射与部署配置不一致，拒绝发布指纹。`

**这句话把两种完全不同的情况说成了一句**（登记为 P12.44，未修）：

| 报告里 `identity.observed` | 是什么 | 该去哪 |
| --- | --- | --- |
| 三对全 `0x00000000` | **EEPROM／SII 读失败** | 查那台设备的供电／上电时序／接线 |
| 是个真值但与 `expected` 不同 | **真的型号不对** | 这才该动配置 |

### 12.3 不要用 `[ -x ]` 查本仓库的脚本

`scripts/` 下 64 个文件都是 `100644`，一律以 `bash <脚本>` 调用；**能执行位不是这里的约定**。查可读即可。

### 12.4 离线干跑只验装配，不验真报告

`bench_series.sh` 用 `ROUNDS=0` 可以在**不碰总线**的前提下验完参数回显、报告路径解析、证据目录与表头。但**它验不到字段取对了没有**——`report_row.py` 就出过一次两列取错层、安静退化成 `-` 的事，是在看真实输出时才发现的。

### 12.5 报告里「整帧未回」与「计数变短」要分开看

见 §7.2 第 1 条。12 轮实测里 11 轮这两个数相等（丢的是整帧），唯一的例外那一轮同时掉了一根轴。

## 13 调试手册

这一节回答的是"手里的工具怎么用来回答一个具体问题"。全部只读或只用配置，不改主逻辑。

### 13.1 读一台驱动器的全部运行期读数（最快）

```bash
cd /home/orangepi/ethercat-master
sudo bash -c 'ulimit -r unlimited; exec env EMASTER_OBSERVER_VERBOSE=1 EMASTER_OBSERVATION=1 \
  ./build/tools/master/emaster-master --deployment <部署ID>'
```

观测线程每约 1 秒在 stderr 打一条，含**全部 12 项**慢速遥测（当前值 + 单位）：

```
[P4.3] 轴0: actual_current=9 0.001 A, cia402_error_code=0, bus_voltage=48104 mV,
       mosfet_temperature=391 0.1 degC, motor_temperature=344 0.1 degC,
       actual_motor_speed=0 rpm, speed_command=0 rpm,
       target_position_readback=3895 count, max_torque=1000, max_motor_speed=841045  (读取次数=221)
```

- **这些是 OP（CSP）运行期间的 SDO 读数**，不是停机快照
- 清单在设备配置的 `slow_telemetry`（`config/devices/cyberbeast_*.json`）
- 加了这个开关还会打出每条读数的对象地址与 wkc，用来确认"到底读到没有"

### 13.2 按阶段读指定对象（键值对）

```bash
sudo python3 scripts/analysis/extract_report.py runtime/reports/<部署>-latest.json \
  | grep -E '^axis[0-9]+\.(status_word|mode_display|target_position|max_torque|max_motor_speed)\.'
```

```
axis1.status_word.cyclic_operation=4663
axis1.status_word.cyclic_operation.hex=0x1237
axis1.mode_display.cyclic_operation=8
axis1.target_position.cyclic_operation=462644
axis1.max_torque.safeop_initialization=1000
```

键的形状：`axis<N>.<对象名>.<阶段>=<十进制>`，另有 `.hex=<原始十六进制>`。

**为什么必须按阶段看**：`0x6041` 在停机后 bit2／bit10 会掉。运行期是 `0x1237`、停机后是
`0x1233`——只看最后一条会把停机态当成运行态。

**读法边界**：某个对象没有 `cyclic_operation` 这一项，说明**这份二进制在 OP 期间没有读它**
（审计段记录的是主站真的读过的对象），**不是"值没变"**。OP 期间读哪些由 `slow_telemetry` 决定。

### 13.3 状态字 0x6041 怎么读

| 位 | 含义 | 运行期典型 |
| --- | --- | --- |
| bit2 | 运行使能 | 1 |
| bit3 | 故障 | 0 |
| bit10 | 目标到达 | 1 |
| bit11 | **内部限幅生效** | 0 |

`0x1237` = 使能、无故障、目标到达、限幅未生效。
`0x1233` = 同上但 bit2／bit10 为 0（SAFE-OP 或停机后）。

### 13.4 单轴档的护栏比多轴档紧得多——先拿它验可疑的驱动器

| 档 | `max_following_error_millidegrees` |
| --- | --- |
| 单轴 `orangepi-current-bench` | **2000（2°）** |
| 五轴～九轴各档 | **50000（50°）** |

**后果**：同一根"收到目标却不执行"的轴——

- 在多轴档上：一步不走也够不到 50°，**报告显示成功**（P12.40 那个坑）
- 在单轴档上：**0.4 秒就按 `FOLLOWING_ERROR` 中止**，不用人眼从几个跟随误差里挑

```bash
sudo env DEPLOYMENT=orangepi-current-bench DURATION=40 TRAVERSE=5 DEGREES=30 \
  RATE=100 ONLY_AXIS=0 OBSERVATION=1 TAG=single30 \
  bash scripts/bench_reciprocate_30deg.sh
```

### 13.5 freerun（不用 DC）档：用来把同步从嫌疑里划掉

配置已备好，**不需要改代码**：

```bash
sudo env DEPLOYMENT=orangepi-bench-single-freerun DURATION=40 TRAVERSE=5 DEGREES=30 \
  RATE=100 ONLY_AXIS=0 OBSERVATION=1 TAG=fr30 \
  bash scripts/bench_reciprocate_30deg.sh
```

- `config/operation_profiles/cyberbeast_freerun_csp_1ms.json` —— `strategy: "sm"`（SyncManager
  同步即 FreeRun）、`assign_activate: 0x00000000`、`sm2/sm3_sync_type: 0x0000`
- `config/deployments/orangepi_bench_single_freerun.json` —— 单轴 + 上面那份

主站里 SM 分支本来就有（`session_setup.c` 只在 DC 时置 `dc_required`，SM 时整段 DC 配置跳过），
校验器也认（`strategy in ("sm", "dc")`）。**SM 只要求四个参数**：`assign_activate`、`cycle_ns`、
`sm2_sync_type`、`sm3_sync_type`；`sync0_shift_ns` 那几个在 SM 下不要求。

**用途**：同一个现象在 DC 与 freerun 下**逐项一致**，就把 DC／SYNC0 同步从嫌疑里划掉了。

### 13.6 "轴不动"的排除表

| 假设 | 拿什么排除 |
| --- | --- |
| 主站没发目标 | 命令被拒 0 条、单步 < `max_step`、`0x607A` 读回 == 主站写的值 |
| 状态机没使能 | `0x6041` 运行期 bit2=1、bit3=0；控制字 `0x000F` |
| 限幅卡住 | OP 期间 `0x6072`／`0x6080` 的值、`0x6041` bit11=0 |
| 驱动器报故障 | `0x6041` bit3=0、`603F=0x0000`、`fault_timeline` 四项全 false |
| 过热／母线 | `slow_telemetry` 的温度与母线电压 |
| 链路／CRC | `rx_error_counter` 全 0、期望 WKC == 实际 WKC |
| DC／SYNC0 同步 | 同一现象在 freerun 档下复现（§13.5） |

**`target_position_readback` 是那个关键判别器**（P12.41）：它是**驱动器自己持有的**目标位置。

- 读回跟着主站一起扫 ⇒ 目标到了驱动器，是它不执行（功率级／使能／电机侧）
- 读回一直停在旧值 ⇒ 值在"过程映像"到"驱动器应用"之间就断了，与电机无关

主站写进过程映像的 `target_position` 证不了这件事——主站永远知道自己写了什么。

**2026-09-29 单轴台架实测（`orangepi-current-bench`，100 Hz）**——这一节现在有两组数，
**第二组推翻了第一组的结论**，两组都留着：

① **命令只有 1° 时，轴一步不动**（`obj`，60 s，起点 3909）：

```
[client] t+54s 往返 5.42 次 phase=0.846 目标=[4987] 实际=[3919] 已发=5424 被拒=0 最大间隔=11.0ms
[client]   a1 6041=0x1237 6061=8 607A=4982 6072=1000 6080=841045
```

三十块采样每一块都是这个形状：`6041=0x1237`（bit2 使能、bit3 无故障、bit10 目标到位=1）、
`6061=8`、`6072`／`6080` 没卡、`607A` 逐块跟着主站扫，而 `6064` 只在 3909–3919 之间挪了
10 counts。当时据此写下"目标到了驱动器、驱动器却不执行"。

② **命令换成 30° 时，轴全程跟着走**（`real30`，40 s、单程 5 s、4.00 次往返、被拒 0、退出码 0）：

```
[client] t+16s 往返 1.60 次 phase=0.796 目标=[109477] 实际=[109664] 已发=1603 被拒=0 最大间隔=11.0ms
[client] t+20s 往返 2.00 次 phase=0.006 目标=[79277] 实际=[78977] 已发=2004 被拒=0 最大间隔=11.0ms
```

跟随误差 200–300 counts（约 0.2°），单步最大 77 counts（上限 2548）。**所以①不是
"驱动器不执行"，而是"1° 这个量级它不动"**——差别在命令量级，不在链路、不在映射、
也不在使能。①里"读回跟着走"确实排掉了链路与映射（PDO 真的到了驱动器的对象字典），
这一点仍然成立；**被推翻的是由它推出"驱动器不执行"这一步**——`6064` 不动只说明
"这个量级的命令它没执行"，不说明它执行不了。

**`607A` 这一栏始终是驱动器自己报的值**：主站写进过程映像的目标在上一行的 `目标=` 里。
两个并排看才分得开"目标没送到驱动器"和"送到了却不执行"。

### 13.7 那些只能由厂商上位机设的东西

`docs/lz-joint/特别注意.txt` 第 1 条：

> 使用 ethercat 控制时需要在上位机上将 `enable_ecat` 赋为 1，`enable_can_a` 赋为 0

**它不是 EtherCAT 对象**——审计段那 65 个已读对象里没有它，主站读不到也写不了，只能通过厂商
上位机设。同文件第 2 条记着 STO 暂不支持。

### 13.8 观测/审计这两条通路的边界

- **观测通道**（`-obs.sock`，256 槽）在**内存**里，不落盘；没人读就被覆盖
- **审计段**（报告里的 `accesses`）只记**配置期与停机诊断**的 SDO，**不含运行期遥测**——
  运行期遥测（`slow_telemetry` 那一份）不进审计段，要读它得走 §13.1 或加报告字段
- 报告写盘是**停机时一次**，且是覆盖式的；旧的在 `archive/`

### 13.9 运行期到底能读到什么（一份完整清单）

排查时问"还有没有可以读的"，答案分三层。下面这份清单是**唯一权威来源**——它由
`config/devices/<驱动型号>.json` 生成，改配置就是改清单。

| 层 | 节拍 | 对象 | 在终端哪里看 |
| --- | --- | --- | --- |
| 快帧（PDO）主站→驱动 | 每拍 | `6040` 控制字、`607A` 目标位置、`60FF` 目标速度、`6071` 目标力矩、`6060` 模式 | `6040`／`607A`；其余写死的常量 |
| 快帧（PDO）驱动→主站 | 每拍 | `6041` 状态字、`6064` 实际位置、`606C` 实际速度、`6077` 实际力矩、`6061` 模式显示 | 客户端对象行（`6041` `6064` `606C` `6077` `6061`） |
| 慢速遥测（SDO） | 50 ms 轮询 | `6078` 电流、`6079` 母线电压、`200B:01` MOSFET 温度、`200B:02` 电机温度、`200B:08` 实际转速、`200B:09` 命令转速、`607A` 驱动器自报目标、`6072` 最大力矩、`6080` 最大转速、`603F` CiA402 错误码、`203E` 扩展伺服错误码、`203F` 伺服错误码 | 客户端对象行；`EMASTER_OBSERVER_VERBOSE=1` 时另有一份逐条 stderr |
| 会话级计数 | 每拍累加 | `wkc`／`wkc_expect`／`wkc_err`／`no_frame`／`over_budget`／`deadline_missed`／`cycle` | 客户端会话行 |

客户端每次例行报告打两行（会话行 + 一行一轴），键就是对象号本身：

```
[client] t+54s 往返 5.42 次 phase=0.846 目标=[4987] 实际=[3919] 已发=5424 被拒=0 最大间隔=11.0ms
[client]   state=4 cycle=54001 axes=1 enabled=1 completed=1 wkc=3 wkc_expect=3 wkc_err=0 no_frame=0 over_budget=0 deadline_missed=0
[client]   a1 6040=0x000F 6041=0x1237 6061=8 6064=3919 606C=0 6077=47 6078=48 6079=48101 607A=4982 6072=1000 6080=841045 200B:01=392 200B:02=346 200B:08=0 200B:09=0 203E=0x00000000 203F=0x00000000
```

**读法上的三件事**：

1. **`6078`／`6079`／`200B:*`／`607A`／`6072`／`6080`／`203E`／`203F` 是慢速通道**，比快帧旧
   （快照 50 ms 一轮，客户端采样是秒级）。慢速通道没读到时主站印 `na`，客户端照原样印出来，
   **不要当成 0**——这几个数的 0 全是合法读数（0 A、0 rpm、`0x00000000` 就是"没有错误"）。
2. **`607A` 是驱动器自报的那个值**，主站写进过程映像的目标在上一行的 `目标=` 里。
   两个并排看才分得出"目标没送到驱动器"和"送到了却不执行"。
3. **`203E`／`203F` 是厂商自己的错误码**，CiA402 的 `603F` 只是标准错误码。
   它们原先只在停机诊断里读（P12.52 改成运行期也读），因为最可能写着"为什么不动"的原因
   就在这两条里，而停机后再读到的值分不清是运行时就在报、还是停机过程里才置起来的。
   代价是每轴每轮的 SDO 从 12 条增到 14 条；若 A/B 显示它本身加重了 SM2 事件丢失，
   应当把语义从设备配置的 `slow_telemetry` 里删掉。

**还没接进终端的**（要的话再加，都在报告里）：`fault_timeline` 四项、`rx_error_counter`、
`epu`／`pdi`、`tail_max_receive_ok_ns` 这类时序极值。它们回答的是"这一轮健康吗"，
报告粒度就够；上面这一份回答的是"这一刻轴在干什么"，所以要进终端。

### 13.10 运行期看 DC／SYNC0（命令 `dc`）

DC 的量比过程数据还多，所以单独一条命令，与 `status` 分开取。客户端每次例行报告会跟着打印：

```
[client]   dc required=1 configured=1 ref=1 stable=1 startup_done=1000 startup_req=1000 startup_exchanges=1000 startup_phase_err_ns=-8914 late_threshold=0 late_exceeded=0 late_axis=-1
[client]   a1 dc aa_req=0x0300 aa_obs=0x0300 sync0_req=1 sync0=1 cycle_req_ns=1000000 cycle_obs_ns=1000000 shift_req_ns=250000 shift_soem_ns=250000 samples=45994 margin_ns=200246 margin_min_ns=163966 phase_err_ns=-246 late=0 late_run=0
```

**会话行**：`required`／`configured`（这一档要不要求 DC、配没配上）、`ref`（参考从站位置）、
`stable`（启动相位是否收敛）、`startup_done/startup_req`（收敛用掉的交换数）、
`startup_phase_err_ns`（收敛后的相位误差）、`late_threshold`／`late_exceeded`／`late_axis`。

**每轴行**（键就是协议字段名）：

| 键 | 是什么 |
| --- | --- |
| `aa_req` | 主站请求写进 `0x1C32:01` 的 `assign_activate` |
| `aa_obs` | **驱动器写回后实际生效的那一份**；读失败印 `na` |
| `sync0_req`／`sync0` | 请求的与 SOEM 生效的 SYNC0 |
| `cycle_req_ns`／`cycle_obs_ns` | 请求的与驱动器报的周期 |
| `shift_req_ns`／`shift_soem_ns` | 请求的与 SOEM 生效的 SYNC0 移位 |
| `samples` | 一共记了多少个 DC 样本 |
| `margin_ns` | **本周期 SYNC0 裕量**，为负说明输出对驱动器已经迟到 |
| `margin_min_ns` | 全程最小裕量（最危险的那一拍） |
| `phase_err_ns` | 本周期相位误差 |
| `late`／`late_run` | 迟到累计次数／**连续迟到最长的一段** |

**`late_run` 是这一节里最该看的数**：驱动器判"这一拍帧来晚了"用的是它自己的 SM 事件丢失
计数，攒到 6 次就置 AL `0x001A` 掉出 OP。主站手上这个数回答的正是"离那个阈值还有多远"。

**两个数不一致就是配置没落进驱动器**：`aa_req` 与 `aa_obs` 不同、或 `cycle_req_ns` 与
`cycle_obs_ns` 不同，说明写下去的值被驱动器改了或压根没接受——这是 P12.47 那条空缺
（"驱动器自己的同步配置主站一条读数都没有"）的答案。`aa_obs` 读失败时印 `na`，
**不许当成"驱动器回的配置就是这个"**。

**没有 DC 样本时裕量印 `na` 而不是 0**：0 ns 裕量看起来像"刚好卡在边界上"，比"没测到"
严重得多。判据是 `has_sync0_margin`／`last_dc_sample_valid`。

## 14 速查

```bash
# 台架干净吗
sudo pkill -9 -f emaster-master; sudo pkill -9 -f socket_bridge; rm -f /tmp/emaster-*.sock

# 主站在跑吗、到 RUNNING 没有
pgrep -a -f emaster-master
sudo timeout 3 nc -U /tmp/emaster-<部署>.sock <<< status | grep -o 'state=[0-9]*'

# 拓扑（轴数、单步上限、换算）
sudo timeout 3 nc -U /tmp/emaster-<部署>.sock <<< topology
```

**主站未启动／连不上**：`pgrep -x emaster-master || echo '主站未运行'`、`ls -l /tmp/emaster-<部署>.sock`。
日志位置取决于你怎么起的：`bench_idle.sh` → `/tmp/<TAG>_master.log`；`bench_series.sh` → `reports/_<SERIES>/rNN_*/`；直接起则在你重定向的地方。

## 15 编译构建

```bash
cd ~/ethercat-master
mkdir -p build && cd build
cmake ..
make -j8
```

产物在 `build/tools/<工具名>/` 下（例如 `build/tools/master/emaster-master`）。

**配置是构建期编进二进制的**（`config/` 下的 JSON 由 CMake 生成 C 代码），所以改了配置**必须在 Pi 上重编**。

## 16 技术栈

- **SOEM** — Simple Open EtherCAT Master
- **CiA 402** — CANopen 驱动器和运动控制设备规范
- **DC (Distributed Clocks)** — 亚微秒级时钟同步
- **PDO (Process Data Objects)** — 实时周期数据交换

## 17 文档索引

| 文档 | 内容 |
| --- | --- |
| `DEVELOPMENT.md` | 项目最终目标与开发要求 |
| `docs/requirements/layering-plan.md` | **分层计划与原子问题登记表**（P 编号、判据、状态） |
| `docs/monitoring-guide.md` | 报告字段怎么读 |
| `docs/dc-timing-guide.md` | DC 时序统计解读 |
| `docs/protocol/command-protocol-v1.md` | 命令协议规格 |
| `docs/MOTOR_PARAMETERS.md` | 编码器、齿轮比、单位换算 |
| `config/README.md` | 配置体系 |
| `docs/USAGE_GUIDE.md` | 已过时（旧的 build/bench 路径与 2 轴部署名），看本文件 |

## 许可证

根据项目实际许可证填写。

## 贡献

根据项目实际贡献指南填写。
