# EtherCAT Master

面向机器人控制的 EtherCAT 主站工程。C11 + SOEM，支持配置为 1 ms DC 同步的多轴 CiA 402 控制。可靠性结论必须依据本文的测量边界和测试文档判据。

**文档依据**：本文按仓库中的当前实现和实际函数调用说明行为；部署时应使用与源码匹配的构建产物。具体测试条件、判据和结果见测试文档。

**两条边界，先讲清楚：**

- **本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。详见 §23。
- **测试条件、判据和结果见 [`wholetest/采集.md`](wholetest/采集.md)。** 本文件说明**当前怎么用**和**当前怎么实现**。

---

# 第 I 部分 · 使用说明

## 阅读导航与构建入口

本文负责当前实现与使用说明；[测试文档](wholetest/采集.md) 负责测试条件、判据和结果。

- 初次部署：本节 → §1–2 前提与启动 → §3–4 界面 → §10 单位。
- 接入控制器：下方命令接口 → §16 位置入口 → §17 两层状态 → §21 观测。
- 修改实现：§12–15 架构与周期 → §18–22 报告、恢复、配置与同步。
- 执行测试：§5–7 工具操作 → 测试文档的正式矩阵。

### 构建与依赖

硬件主站仅支持 Linux。Windows 可以运行 Python 工具、GUI 和适用的离线单元测试。构建需要 CMake ≥ 3.28、Python ≥ 3.10 和 C11 编译器；预设还需要 Ninja。GUI 另需 PyQt5；`nc -U` 示例需支持 Unix socket 的 netcat。

在仓库根目录执行：

```bash
git submodule update --init --recursive
python3 tools/validate_project.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEMASTER_BUILD_HARDWARE_TOOLS=ON -DEMASTER_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

主站产物为 `build/tools/master/emaster-master`，测试脚本使用这个路径。也可使用 `cmake --preset linux-release` 和 `cmake --build --preset linux-release`，但产物在 `build/linux-release/`，**测试脚本不会自动使用该目录**。

不访问硬件的离线构建：

```bash
cmake -S . -B build-offline -DEMASTER_BUILD_HARDWARE_TOOLS=OFF -DEMASTER_BUILD_TESTS=ON
cmake --build build-offline
ctest --test-dir build-offline --output-on-failure
```

多配置生成器需分别加 `--config Debug` 和 `-C Debug`。Windows 不包含 Linux 专用的时钟、报告测试；离线通过不能替代真实总线和运动验证。

### 配置生效与路径

JSON 是构建输入，生成器将设备、部署、拓扑、运行方案、运动方案、恢复策略和文案生成 C 源码后编译。修改配置后须重新校验、构建、重启；只修改 JSON 而运行旧二进制，不能保证生效。

`--deployment` 接收 JSON 中的 `deployment_id`，不是文件名。网口、实时核、优先级和报告路径由部署决定，换主机时必须核对。测试脚本默认仓库路径为 `/path/to/ethercat-master`，路径不同时显式传入 `REPO`：

```bash
sudo env REPO="$PWD" DEPLOYMENT=<deployment-id> IDLE=60 OBSERVATION=1 TAG=first_idle bash scripts/bench_idle.sh
```

### 命令接口

`src/bus/soem/command_server.c` 通过 AF_UNIX 流套接字接收换行结尾的文本命令。建议复用 `tools/interactive_control.py`、`tools/emaster_client.py` 或控制台 Engine，不要假定一次 socket read 就是一条完整回复。

| 命令 | 用途与约束 |
| --- | --- |
| `status` | 查询主站及逐轴状态；主站 `state=4` 和 AL SAFE-OP 的数值 4 属于不同枚举 |
| `topology` | 查询实际轴集合、位置换算和轴顺序；发送目标前先查询 |
| `dc` | 查询同步信息 |
| `set_external_target <p1> <p2> ...` | 按拓扑顺序提交全轴绝对原始位置计数，不是角度或部分轴集合 |
| `switch <motion_profile_id>` | 切换运动方案，不等于切换 CSP/CSV/CST 控制模式 |
| `stop` | 当前实现与 `shutdown` 一样请求会话有序退出；不只是暂停轨迹，不代表断电 |
| `shutdown` | 请求完整停机并退出 |
| `quick_stop`、`halt <参数>`、`fault_reset` | 解析器已有入口；入口存在不代表相关安全功能已经验收 |

外部目标超时由 `external_target_timeout_ms` 指定，12 轴部署示例为 200 ms。客户端 100 Hz 目标更新率与主站 1 kHz PDO 交换率不同。断连、目标暂停、重连必须分别测试，不能假定自动恢复运动。

命令 socket 位于 `/tmp/emaster-<deployment_id>.sock`。当前 `command_server.c` 对其执行 `chmod 0666`，任何本机用户均可下发运动和停机命令；跨机桥 `tools/socket_bridge.py` 也没有应用层认证。只能在可信、隔离网络和单用户测试环境使用。生产部署前应改为专用 Unix group/最小权限，并为跨机控制增加认证和访问控制。

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
| 远程操作 | SSH / SCP | 从管理机执行目标主机命令、传输文件 |

**四条通路，按你在哪台机器上选：**

```text
同机（目标主机上）     客户端 ──unix──► /tmp/emaster-<部署>.sock
跨机（界面在 Windows） 界面 ──TCP:5001──► socket_bridge.py ──unix──► 命令口
                             ──TCP:5002──►              ──unix──► 观测口
远程执行              管理机 ──SSH──► 目标主机
```

## 1 前提与约定

- **要 root**：主站要 `mlockall` + `SCHED_FIFO`，套接字也由 root 建。
- **要 `ulimit -r unlimited`**：不然实时优先级设不上（脚本里都带了）。
- **选择与物理拓扑匹配的部署**：读取 `config/deployments/` 中 JSON 的 `deployment_id`，核对其引用的拓扑、运行方案、运动方案及网口。部署可以没有运动方案；轴数、设备身份和轴顺序不能凭部署名称猜测。
- 部署按总线位置和配置文件确定轴顺序；更换设备或改变布线后，必须重新执行拓扑查询并核对轴映射。

- **不要用 `[ -x ]` 查本仓库的脚本。** `scripts/` 下的文件都是 `100644`，一律以 `bash <脚本>` 调用；能执行位不是这里的约定。查可读即可。

## 2 起主站

```bash
cd /path/to/ethercat-master
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

**换部署前**先经命令口发送 `shutdown`，或向核实过的主站 PID 发送 SIGTERM，等待退出并检查逐轴停用证据。不要将 SIGKILL 当作正常停机。当前 bench 脚本仍包含全局 `pkill -9` 和 socket 清理，只能在没有其它活动主站的专用测试环境执行，不能并行运行多个部署。

## 3 跨机图形界面

界面在另一台管理机上运行，主站和桥运行在目标控制主机上。

**目标主机①：主站** —— 见 §2。

**目标主机②：桥**

```bash
cd /path/to/ethercat-master
sudo python3 tools/socket_bridge.py --deployment <部署ID> --bind 0.0.0.0 --port 5001
```

**另一台机器（Windows）：界面**

```powershell
cd D:\taili\ethercat_master
$env:PYTHONPATH='tools'
python -m emaster_gui --tcp <host>:5001
```

- 端口约定：**命令 5001、观测 5002**（界面自动用 PORT+1，不用另给）
- `--tcp` **同时开了命令通道**，也就是界面能驱动电机。**只读**就换成 `--obs-endpoint tcp:<host>:5002`（不给命令口）
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

## 5 单轮测试

### 5.1 空转（不发目标、**电机不动**）

```bash
sudo env DEPLOYMENT=<部署ID> IDLE=60 TAG=idle1 bash scripts/bench_idle.sh
```

**空转只使能保持，不发任何运动指令 —— 电机不会动，这是设计如此。** 用它把主站单独放在总线上看它稳不稳。改线、扩轴、换设备之后的第一道关。

### 5.2 30 度往返（**电机会动**）

```bash
sudo env DEPLOYMENT=<deployment-id> \
  DURATION=60 TRAVERSE=5 DEGREES=30 RATE=100 WAVE=triangle \
  ONLY_AXIS=0 OBSERVATION=1 TAG=recip30 \
  bash scripts/bench_reciprocate_30deg.sh
```

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `DEPLOYMENT` | `<deployment-id>` | 部署 ID |
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
sudo env DEPLOYMENT=<deployment-id> SERIES=<系列名> ROUNDS=6 GAP=60 \
  MODE=reciprocate DURATION=60 TRAVERSE=5 DEGREES=30 RATE=100 ONLY_AXIS=0 OBSERVATION=1 \
  bash scripts/bench_series.sh
```

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `DEPLOYMENT` | **必填** | 没有默认值：写默认部署等于把某个测试环境写进工具里 |
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
| `report_digest.py` | 报告摘要（比 summary 更短） |
| `report_size.py` | 报告体积构成 |
| `shutdown_dump.py` | 只看停机那一段 |
| `ring_dump.py` | 现场环原样导出 |
| `trace_tail.py` / `trace_busy_windows.py` | 尾部时序／繁忙窗口 |
| `ab_compare.py` / `ab_verdict.py` | 两组对照的比较与判定 |
| `inline_verdict.py` | 就地判定单份报告 |
| `json_probe.py` | 不下载整份 JSON 就探字段 |
| `panel_transcript.py` | 终端面板会话转录 |
| `uprobe_offsets.py` | uprobe 偏移计算（开发用） |

**`scripts/analysis/` 共 19 个脚本**，上面是常用的；其余看目录。
另有两条在 `scripts/` 顶层：`nic_counters.py --diff`（网卡收发计数差分）、
`capture_identity.py`（装置身份 + 主机上下文）。

### 7.1 报告字段层级

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

1. **`wkc_no_frame` 与 `wkc_mismatch` 的计数口径不同。** 前者记录未取得有效回帧，后者记录过程数据 WKC 与预期不符。计数可帮助区分整帧缺失与非预期 WKC，但不能单独定位到链路、主机调度或某个从站；非预期 WKC 也不能直接等同于从站掉出 OP。
2. **`AL state=0` 不是「在 INIT」。** SOEM 的 `ecx_readstate` 先把缓冲清零再读，所以 0 只可能是**这一站没应答**。8=OP、20=SAFE-OP 加错误位、1=INIT、**0=没读回来**。
3. **收尾那句 `OP=通过` 走地址 0 广播，只照见环上最后一站。** 判「哪几根不在 OP」要看逐轴快照（`shutdown_al_entry`）。
4. **按 `index.tsv` 的表头读取字段，不按固定列号统计。** 可运行 `series_index.py` 从关联报告重建索引，并核对时间字段的单位。

## 8 远程操作（管理机连接目标主机）

```bash
ssh <user>@<host>
scp <local-file> <user>@<host>:<remote-directory>
scp <user>@<host>:<remote-file> <local-directory>
```

尖括号内容须替换为实际值，不要原样执行。登录后先进入项目部署目录，再执行 §2 的主站命令。连接地址、用户和凭据由部署环境管理，项目不限定测试机器。

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

> **启动使用 §2 的入口。** `scripts/start_master.sh` 使用固定默认部署、
> **不设 `EMASTER_OBSERVATION=1`**（观测口不建，报告的温度／电压／电流栏恒为「—」）、
> **不设 `ulimit -r unlimited`**（实时优先级设不上）。**起主站用 §2 那串命令。**

**`emaster-scan` 会把驱动器拉出 OP 再恢复**（它要发 INIT→PRE-OP 请求），**运行中不要用它**；**`emaster-disable` 会让电机失去保持力矩。**

## 10 单位换算（由设备配置确定）

换算由每轴启动读回的编码器分辨率和齿轮比确定，不能把某个拓扑的比例用于全部设备。`topology` 返回运行时换算；位置换算算法见 `src/motion/position_target.c`，SDO 读回与配置见 `session_setup.c`。

```text
电机每转计数 = encoder_increments / encoder_motor_revolutions
输出轴每转计数 = 电机每转计数 × gear_motor_revolutions / gear_shaft_revolutions
相对位移计数 = 输出轴角度 / 360 × 输出轴每转计数
绝对位置目标 = 确认的起点位置计数 + 相对位移计数
```

例如编码器 16384 counts/电机转、齿轮比 28:1，对应 458752 counts/输出轴转，约 1274.31 counts/度；25:1 对应约 1137.78 counts/度。还须核对正方向、机械零点和整数舍入。原始位置零点不是自动校准出的机械零点。

单步上限由运动方案的 `max_step_millidegrees` 决定，取值看 `topology` 回报的 `max_step`。
**单步超限不会返回错误，而是判定 `MOTION_INVALID` 后中止整个会话、轴失能** —— 所以客户端要把大角度拆成小步流式发送。

超过 `external_target_timeout_ms`（默认 200 ms）没收到新目标，CSP 入口回调返回 `HOLD`，保留最后目标；它不等于驱动器失能，CSV/CST 的行为边界见 §28。

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
- 清单在设备配置的 `slow_telemetry`（`config/devices/cyberbeast_isvd90rc_300b_100_70.json`），
  共 12 项：`0x6078` 电流、`0x603F` 故障码、`0x6079` 母线电压、`0x200B:01` MOSFET 温度、
  `0x200B:02` 电机温度、`0x200B:08` 实际转速、`0x200B:09` 转速指令、扩展伺服故障码、读回目标、
  最大力矩、最大转速等
- 加了这个开关还会打出每条读数的对象地址与 wkc，用来确认"到底读到没有"

**主站真正认的环境变量只有这 8 个**（代码里查过的，别按猜的用）：

| 变量 | 作用 |
| --- | --- |
| `EMASTER_OBSERVATION` | **建不建观测口，默认关** |
| `EMASTER_OBSERVER_VERBOSE` | 观测线程每约 1 秒打一条全部 12 项读数 |
| `EMASTER_FRAME_TIMEOUT_US` | 覆盖帧超时（默认周期/4 = 250 µs） |
| `EMASTER_AUDIT_MAX_ACCESSES` | 审计访问记录容量上限 |
| `EMASTER_SHUTDOWN_FAST` | 停机快模式 |
| `EMASTER_SHUTDOWN_LEGACY_PROLOGUE` | 选择阻塞式停机序言 |
| `EMASTER_CMD_SERVER_VERBOSE` | 命令服务器详细日志 |
| `EMASTER_NO_FIRST_MISMATCH_AL_READ` | 首次不符时不读 AL |

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

**为什么必须按阶段看**：例如 `0x1237` 与 `0x1233` 的 bit2 不同，分别对应 Operation enabled 与 Switched on 的标准状态掩码。只看停机后的状态字不能推断运行期间是否使能。

**读法边界**：某个对象没有 `cyclic_operation` 这一项，说明**这份二进制在 OP 期间没有读它**（审计段记录的是主站真的读过的对象），**不是"值没变"**。

### 11.3 状态字 `0x6041` 怎么读

| 位 | 含义 | 运行期典型 |
| --- | --- | --- |
| bit2 | 运行使能 | 1 |
| bit3 | 故障 | 0 |
| bit10 | 目标到达 | 1 |
| bit11 | **内部限幅生效** | 0 |

- `0x1237`：标准状态为 Operation enabled；bit3=0、bit10=0、bit11=0，不应解读为“目标到达”
- `0x1233`：标准状态为 Switched on；bit2=0、bit10=0，不能据此推断 AL 是否 SAFE-OP
- `0x1250` = Switch on disabled（已失能）
- `0x1210`：标准状态为 Not ready to switch on；出现原因仍需结合启动阶段和设备诊断

**译名依据是 CiA 402 状态掩码；厂商扩展位应按对应固件手册解释**（见 §17）。

### 11.4 跟随误差与单步保护

限幅在 `config/motion_profiles/*.json` 的 `axes[].max_following_error_millidegrees`，**每一份单独写**：

`session_control.c` 在请求下一目标之前，用本周期实际位置与当前保留目标的差检查跟随误差；超过阈值触发 `FOLLOWING_ERROR`。这是主站采样的误差，不是驱动器内部伺服每个子周期的最大误差。CST 跳过位置误差比较，因为力矩目标和位置反馈量纲不同。

单步保护由 `session_target.c` 比较候选目标与上次提交目标，超限触发 `MOTION_INVALID`。单步上限约束离散目标变化，不等于连续速度、加速度或加加速度约束；客户端还要自行生成受限轨迹。外部目标路径保护换算取首轴的限制见 §28。

保护阈值须依据允许误差、负载和机械限制设置。例如运动行程 30° 而误差护栏为 50°，不动的轴也可能不触发保护；没有触发故障不等于运动精度合格。

### 11.5 freerun（不用 DC）运行方案

```bash
sudo env DEPLOYMENT=<freerun-deployment-id> DURATION=40 TRAVERSE=5 DEGREES=30 \
  RATE=100 ONLY_AXIS=0 OBSERVATION=1 TAG=fr30 bash scripts/bench_reciprocate_30deg.sh
```

运行方案位于 `config/operation_profiles/cyberbeast_freerun_csp_1ms.json`：`strategy: "sm"`、`assign_activate: 0x00000000`、`sm2/sm3_sync_type: 0x0000`。选择引用该方案且与实际拓扑匹配的部署。它可用于同步策略对照；相同现象在两种策略下出现，只说明该现象不专属于 DC 路径，不能排除两种策略共有的配置、数据或驱动器问题。

### 11.6 "轴不动"的检查项与证据边界

| 检查项 | 证据与边界 |
| --- | --- |
| 目标传递 | 比较客户端发送值、主站 PDO 输出审计与有效的 `0x607A` 读回；读回一致不证明固件已经在预期周期执行 |
| 使能与模式 | 按状态掩码解码 `0x6041`，并核对 `0x6061`；只检查 bit2 或控制字 `0x000F` 不充分 |
| 限幅 | 核对有效的 `0x6072`、`0x6080` 与状态字 bit11；扩展保护仍需厂商定义 |
| 驱动器故障 | 比较状态字、`0x603F` 与厂商故障对象；慢速读取可能漏掉短暂故障 |
| 温度／母线 | 检查慢速遥测的有效位、年龄和厂商限值；退出快照不证明故障瞬间正常 |
| 链路 | 联合 WKC、网卡计数、ESC 错误计数及读取有效性；零计数不能排除调度延迟或未采集事件 |
| 同步 | 比较 DC/freerun 行为，并按 §22 的测量边界解释 Sync0；软件估计不能替代锁存测量 |

## 12 架构与所有权

目录按职责组织，CMake target 约束依赖。**公共接口不暴露 SOEM 类型**；设备、部署与方案由配置表达，入口中仍有缺省值和兜底参数。

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
| `src/bus/soem/` | **SOEM 集成边界**；周期交换、配置/停机及观测 SDO 分属对应路径，不能将其描述为全部串行 |
| `src/audit/` | 保存访问事实、连续同值合并、退出后序列化 |
| `tools/master/`、`src/messages/` | 入口、信号处理、报告发布和中文展示 |

`src/cyclic/` 提供总线无关的时序、直方图和滑窗逻辑；`src/safety/` 提供条件汇总与决策，当前由 `session_supervisor.c` 接入控制循环。库中存在的能力是否适用于具体部署，仍需检查调用路径和配置。

## 13 主线：从配置到周期

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

各阶段的验收步骤与证据要求见 [测试文档](wholetest/采集.md) 的正式测试矩阵。

## 14 总线内部结构

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

## 15 周期与并发

主线程按**绝对截止点**唤醒后立即发送，接收完成后再记录审计、计算相位修正、处理反馈与生成下一帧。参数化的过程帧相位与 Sync0 偏移具有不同意义，**周期线程不能以"固定休眠 1 ms"代替相位跟随**。期限已错过时退出正常控制，**不连续补发历史帧，不自动放宽 WKC 判据**。

PDO 审计游标按实际字段数量分配，每个字段直接查找上次记录。周期前预留并触及记录存储，**周期中不扩容、不写文件**；容量耗尽后明确记录截断。**周期线程不发起慢速遥测 SDO 请求，但仍处理 SOEM 邮箱。**

每个物理从站还分别聚合主机单调时间、参考 DC 时间、发送/往返耗时、发送迟到量和距 Sync0 的理论裕量；这些数值用于定位时序，**不能替代从站提供的 SM2 完成时间戳**。

动态 PDO 直接提供模式反馈。固定 PDO 缺少模式反馈时，在 SAFE-OP 初始化阶段通过 SOEM SDO API 读回并在报告中保留；**周期控制线程不执行这些阻塞 SDO**。观测线程仍在 OP 期间用同一会话 context 访问邮箱；`session_exchange.c` 也调用 SOEM 邮箱处理器。不能将“控制线程不执行 SDO”写成“OP 期间没有邮箱流量”。

**当前源码已接受 CSP/CSV/CST（模式 8/9/10）**：`src/session/session_plan.c` 允许这三种模式，`control_session.c` 按模式选择位置、速度或力矩目标/反馈，`session_target.c` 做目标检查。接口仍沿用 `position_target_source` 命名，但模式不同，目标量纲不同；不能把位置客户端直接用于 CSV/CST。当前正式测试方案仍只覆盖 CSP，源码路径存在不代表其它模式已完成真机验收。

截止时间检查和 WKC 检查属于执行条件。诊断性模式反馈策略**只允许保持位置**；配置了变化轨迹的轴必须匹配请求模式后才开始运动。**所有轴在同一次协调步骤中处理，不发布部分目标集合。**

## 16 位置命令入口

CSP 的最小产品入口必须把**位置目标输入**和**周期 PDO 发送**分开。应用层通过 `emaster_control_session_callbacks_t.position_target_source` 提交完整轴集合的目标位置，主站周期线程只接受明确的当前目标并检查已读回的软件位置范围；**目标单位是已按当前从站换算完成的原始位置计数**，单位换算、绝对零点和轨迹限制由独立的上层命令或轨迹模块负责。

位置目标来源回调只提供当前周期的完整目标快照，返回 `HOLD` 时保持上一目标，返回 `UPDATED` 时由主站检查所有轴的已读回软件位置范围后写入下一帧 CSP PDO。回调为空时使用部署引用的固定运动方案；`tools/master/main.c` 安装外部目标回调，未收到目标时保持初始位置，不自动运行固定轨迹。

这个回调契约不包含外部发送序号或客户端时间戳。命令服务器在主机接收目标时记录本机单调时间，入口回调用它检查目标年龄；这不能识别客户端发送前的排队延迟或重放目标。多轴协调器的 sequence 是主站内部周期号，也不能证明外部命令顺序。


## 17 两层状态：AL 与 CiA 402

**这是本项目最容易搞错的地方，单独成节。**

| | EtherCAT AL（ESC 层） | CiA 402（驱动器层） |
| --- | --- | --- |
| 载体 | 寄存器 `0x0120` / `0x0130` / `0x0134` | 对象 `0x6040` / `0x6041` / `0x603F` |
| 状态 | INIT 1｜PRE-OP 2｜SAFE-OP 4｜OP 8 | 八个状态，见 `include/emaster/cia402/controller.h:8-19` |
| 通道 | INIT 无邮箱无 PDO｜PRE-OP 有邮箱｜SAFE-OP 有输入无输出｜OP 双向 | 邮箱（PRE-OP 及以上）或 PDO（仅 OP） |
| 谁管 | 主站的状态请求 | 驱动器的内部状态机 |

**两层状态必须分别验证。** AL OP 表示过程数据通信状态，不等于驱动器已使能；请求 INIT 也不构成驱动器撤使能的确认。停机路径先尝试撤销 CiA 402 使能并读回状态，再结束总线会话。

**使用边界：**

1. **不能用 AL 状态推断驱动器使不使能**，两个都要读
2. **不能用 PDO 去送"停止周期通道"的命令** —— 从站掉出 OP 后控制字就到不了它，**禁用必须走邮箱**
3. **AL 在 INIT 时读不到 `0x6041`**（邮箱关闭），这时只能说"读不到"
4. **`0x0134` 只在 `0x0130` 的 bit4（Error Indicator）置起时才有意义**；没有 error 位时读它等于没读。SOEM 只在看到 error 位时才去读，这是照规范做

**对象编号的权威链**：CiA 402（IEC 61800-7-201）规定编号 → 厂商在 ESI/EDS 里声明（`docs/lz-joint/ds402.eds`、`ECAT_CIA402.xml`）→ 本项目在 `config/devices/` 的 PDO 映射表里引用 → 主站按表建过程映像。

`src/cia402/controller.c` 按标准状态掩码解码状态字；设备配置另提供停用确认掩码。标准状态名不能代替厂商扩展位定义，`Voltage enabled` 等状态位也不等于独立的电气断能测量。

**为什么 AL 状态码不能当"为什么失败"的入口**：它满足的是"这次请求的转移失败了"，而 error 位没置起就说明从站不认为它拒绝过。**要问"请求到底发到没有"，看的是写操作的工作计数器，不是状态码。**

## 18 失败与报告

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

**慢速遥测会写入报告**：当前 `src/audit/report_sections.c` 保存逐轴慢速遥测快照、读取有效标志和年龄。开启观测才有采样；快照不是完整时间序列，也不是故障瞬间同步测量。读取失败、过期和有效零值必须区分。`0x0134` 需结合 AL Error Indicator 与读取有效性解释。

## 19 错误恢复策略

**阈值不写在代码里**，读 `config/error_recovery_policies/<id>.json`（`default` / `strict` / `tolerant`），由部署档的 `error_recovery_policy_id` 引用。策略未找到时用保守默认（**首次错误即停机**）。

| 块 | 默认 | 含义 |
| --- | --- | --- |
| `wkc_recovery` | **连续 5**、60 秒窗口内 50 | 短帧 |
| `no_frame_recovery` | 同上 | 整帧未回 |
| `deadline_recovery` | 连续 10 | 周期截止点错失 |
| `al_state_recovery` / `cia402_fault_recovery` / `dc_late_recovery` | `enabled=false` | 未启用（键名不能简写，配置校验按全名） |

**连续阈值与滑动窗口阈值都参与判定。** 连续段是联合计数（整帧未回也计入），窗口分别统计短帧和整帧缺失，因此 `wkc_short_window_max` 可能小于 `wkc_max_consecutive`，两者不是同一口径。

**累计量按类取**：整帧缺失用自己的累计，短帧用"联合累计 − 整帧缺失累计"。`wkc_*` 三个字段的语义不变，仍是联合计数。

滑动窗口在会话建立时就建好，**不等到第一次出错时才惰性初始化** —— 那次初始化恰好发生在已经出错、最不该多做事的时刻。

**`no_frame_recovery` 未另设 `enabled`，沿用 `wkc_recovery.enabled`** —— 避免出现"wkc 恢复开着、整帧缺失却立刻停机"这种组合。

## 20 配置体系

**配置分类**，全部在 `config/` 下（文件数量以工作副本为准）：

| 目录 | 份数 | 内容 |
| --- | ---: | --- |
| `deployments/` | 18 | 部署：轴数、引用哪套拓扑／设备／运行方案／运动方案、报告路径、容错策略 |
| `topologies/` | 13 | 拓扑：环上有哪些从站、顺序、每台的身份期望与位置 |
| `devices/` | 1 | 设备：身份、PDO 映射、`slow_telemetry` 清单、状态字掩码 |
| `operation_profiles/` | 6 | 运行方案：周期、DC 策略（`dc`/`sm`）、SM2/SM3 同步类型、相位与偏移 |
| `motion_profiles/` | 19 | 运动方案：单步上限、跟随误差限幅、轨迹 |
| `error_recovery_policies/` | 3 | 容错阈值 |
| `messages/zh_CN.json` | 1 | 中文文案 |

轴数来自部署、硬件换算会读回、恢复阈值来自策略；`tools/master/main.c` 同时包含 200 ms 默认超时、2°缺省限值和无运动配置时 6400 counts 兜底。外部保护限值取第一轴的预期换算后统一用于全轴，详见 §28。

## 21 观测通道

**独立的只读 unix 套接字**（`/tmp/emaster-<部署>-obs.sock`），**要 `EMASTER_OBSERVATION=1` 才存在**。

- 观测线程做**邮箱 SDO 往返**，读的是设备配置里 `slow_telemetry` 列的对象（电流 `6078h`、母线电压 `6079h`、温度 `200Bh:01h`／`200Bh:02h`、转速、读回目标、限幅等）
- 慢速 SDO 读取由观测线程发起；周期线程拷贝快照，同时交换路径仍会调用 SOEM 邮箱处理器
- 停机序言的 AL 快照与 join 耗时写进报告（`shutdown_al_entry` / `shutdown_al_pre_stop` / `shutdown_observer_join_ns`）

报告中的 join 时间是单次运行测量值，不能作为停机耗时上界。

**电流／电压／温度的当前通路**：设备配置将这些对象放在 `slow_telemetry`，所选 PDO 布局未包含这些字段，因此通过 SDO 读取。固定 PDO 布局限制主站重映射；对象是否支持其它映射须另查设备定义，不能只凭模块的 `Fixed` 属性推出。SDO 往返时间受设备、总线与线程调度影响，不是固定上界。

## 22 DC 与同步

### 22.1 Sync0 由谁产生

Sync0 是从站 **ESC 的分布式时钟（DC）硬件事件**。主站配置起始时刻、周期和激活位，由 ESC 按自己的 DC 时钟产生事件；Linux 线程没有逐周期发送一个“Sync0 命令”。驱动器内部控制或目标锁存怎样使用这个事件，还取决于设备固件与 SM2/SM3 同步设置。

Sync0、主机截止点和报告时间不是同一个时间域：

| 量 | 来源和采样点 | 含义 |
| --- | --- | --- |
| 主机时间 H | Linux `clock_gettime(CLOCK_MONOTONIC)`；发送调用前后、接收与诊断完成后等软件点 | 主机软件事件时间，不是网卡硬件时间戳，不是从站事件时间 |
| 参考 DC 时间 R | 过程帧附加 FRMW 数据报在参考 ESC 读取 `DCSYSTIME`（0x0910），SOEM 解包保存到 `context.DCtime` | 参考站处理该数据报时的 DC 系统时间，不是每轴目标锁存时刻 |
| 周期 T、Sync0 相位 S、过程帧目标相位 P | 运行方案的 `cycle_ns`、`sync0_shift_ns`、`process_data_phase_ns` | 配置意图；不是实测边沿 |

H 与 R 不能直接相减。周期计时使用单调时钟，其绝对值不等于 UTC；DC 的初始纪元由 SOEM 配置，运行期相位比较使用 R 对 T 取模。

### 22.2 DC 参考与传播延迟从哪里来

`external/SOEM/src/ec_dc.c::ecx_configdc()` 广播触发 DC 端口接收时间锁存，读取各端口硬件时间，结合拓扑估算从站传播延迟 `pdelay`，写入 DC 系统延迟和偏移寄存器。传播计算包含正向/返回延迟近似相等的假设；pdelay 是配置期估计，不是每帧测量。

SOEM 将扫描到的组内第一个支持 DC 的从站作为参考，其实际总线编号保存在 `grouplist[0].DCnext`，主站报告为 `dc_reference_slave`。参考站未必等于逻辑轴 1。SOEM 配置还用 `osal_current_time()` 生成 EtherCAT 纪元的初始系统时间偏移；这不是主机周期等待用的 H。

`external/SOEM/src/ec_main.c` 给过程帧追加 FRMW：读取参考站的 DC 时间并向后续 DC 站传播，返回帧解包更新 R。主站未逐周期读出每轴本地时钟与参考的残差；一个参考值不能证明全部从站偏差小于某个保证值。

### 22.3 Sync0 的绝对启动时刻

`ecx_dcsync0()` 读取待配置从站本地 DC 系统时间 t，先停止周期事件，再计算（T>0）：

```text
start = floor((t + SyncDelay) / T) × T + T + S
```

SOEM 将 start 写 `DCSTART0`（0x0990），将 T 写 `DCCYCLE0`（0x09A0），通过 `DCCUC/DCSYNCACT`（0x0980/0x0981）选择 EtherCAT 配置权限并激活周期和 Sync0。此后按该 DC 网格由 ESC 硬件触发。

`session_setup.c` 读回周期和组合激活值，与方案比较。这个检查支持“这些寄存器读回一致”；当前不完整读回 DCSTART0，SOEM `DCactive/DCshift` 是软件镜像，且 `ecx_dcsync0()` 内若干访问返回值未检查。因此不能只凭调用完成或软件镜像证明实际 Sync0 起始时刻正确。

### 22.4 主机怎样跟随参考时钟

`cycle_clock.c` 用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` 等待绝对截止点。每次过程交换后按 R 的周期相位更新 PI：

```text
e = -centered((R - P) mod T)
integral = saturating_add(integral, e)
correction = e / 100 + integral / 50000
correction 限制在 [-T/2, T/2]
下一等待间隔 = T + correction
```

centered 将相位差归一化到约 [-T/2,T/2]，积分累加用饱和避免溢出。PI 改变下一次**主机等待间隔**，不直接修改从站 Sync0。它可跟随相位漂移，但不会消除 Linux 调度延迟、网卡排队和通信故障；增益常数本身不是可靠性保证。

### 22.5 报告的 Sync0 裕量不是边沿时间戳

`src/cyclic/timing.c` 按每轴传播估计计算：

```text
arrival_phase = positive_mod(R + pdelay, T)
sync0_phase = positive_mod(S, T)
sync0_margin = centered(sync0_phase - arrival_phase, T)
phase_error = centered(arrival_phase - P, T)
```

该值是**参考 DC 样本＋传播模型与配置相位的差**。它没有直接测到各站 SM2/PDO 接收完成、驱动器目标锁存或 Sync0 实际边沿。报文中 PDO 与 FRMW 的偏移、参考站到目标站的实际延迟、残余时钟差和驱动器内部处理均未成为完整的逐帧测量误差项。

centered 只保留半周期内相位差，不能区分相差整数个周期的事件。因此 `margin≥0` 不是“目标在正确控制周期的 Sync0 前可靠锁存”的证明，负值也应先作为模型诊断线索解释。配置 P=50 µs、S=250 µs 只有名义 200 µs 相位差，不能自动推出最坏保护余量≥100 µs。

真实锁存余量的证明需要同一时钟域的 PDO/SM2 完成事件与 Sync0 事件，或可关联的外部硬件测量，并给出同步残差、报文偏移、传播、锁存和测量误差的上界。当前报告不提供这套完整证明。

### 22.6 DC 样本与启动判据

`session_exchange.c` 以 `dc_required && context.DCtime>0` 标记 DC 统计有效。SOEM 收不到相关响应时可能保留旧 R；大于零不能证明本周期是新样本，当前也未单独核验 DC 数据报的 WKC。正常过程 WKC 后的 PI 更新同样缺独立新鲜度标志。

`session_start.c` 在有限启动交换次数内要求连续 `dc_startup_cycles` 个各 DC 轴估计 margin≥0 的有效样本，失败清零。`dc_startup_stable` 是这项软件条件达标；没有独立边沿测量、最小正余量或全部从站同步残差检查，不能将它等同于同步可靠性已经证明。

freerun 运行方案选择 `strategy:"sm"`，不配置 DC/Sync0，相应 DC 推导无有效样本；主机仍按周期交换。不同同步策略须分别验证，不能从一种模式的结果推断另一种。


## 23 安全边界

**本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。

- **指纹工具和 PRE-OP DC 准备工具的最高 AL 状态仍为 PRE-OP**，分别只执行受限的只读采集或参数准备
- **`emaster-master` 不在 PRE-OP 指纹流程范围内**：它会请求 SAFE-OP/OP 并把驱动器推进到运行使能
- 进入 CSP 前以 `6064` 实际位置初始化 `607A`
- 停止和失败路径按状态反馈**逐级撤销使能**，并以设备配置中的 `6041` 掩码和值确认停用，随后关闭 Sync0 并恢复 INIT
- **获得 PRE-OP 指纹运行许可，不代表获得 SAFE-OP、OP、CiA 402 使能或运动授权。** 每个更高边界都需要独立流程和批准

**实现与使用约束：**

- 任何过程数据交换开始前，启动输出必须完全初始化
- **硬件使能是生命周期状态，不能是编译开关**
- 网络进入更高状态前，拓扑和身份必须匹配
- 当前外部命令没有发送序号和失效时间；仅接收时刻超时检查的局限见 §16
- 通信丢失、命令过期、反馈无效和进程退出都必须有明确停机反应
- 恢复过程必须有界、可记录，且**不能静默复位驱动器故障**
- 单轴和整机停机策略必须明确且可测试
- 报告文件在周期外写入；周期内仍有有预算的寄存器诊断和命令处理，须计入实时验证

**每份硬件测试记录必须说明**：操作者、观察者、物理装置、功率限制、机械约束、STO/急停验证、网络接口、软件版本、配置哈希、ESI 哈希和全部从站序列号。

## 24 实时基线

### 主机要求

- 使用 PREEMPT_RT 内核，并记录内核与配置哈希
- 服务账号具备 `SCHED_FIFO` 权限，进程内存锁定
- 周期线程使用**专用 CPU**，EtherCAT 网卡 IRQ 亲和性经过明确配置
- EtherCAT 使用**专用网卡**，网卡无 IP、路由、PTP、激光雷达或 NetworkManager 业务
- 网卡中断合并和卸载参数已记录并经过有意配置
- CPU 频率策略、温度限制和后台服务负载已记录

### 1 ms 设计目标

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
| 丢帧/迟帧策略 | 按选定恢复策略执行；稳定性测试要求无异常，容错测试另行验证阈值行为 |

供应商给出的最小周期（单从站 41.1 µs、五从站 46.5 µs）只是实验室设备能力，不能证明目标控制主机或实际多轴网络满足上述指标。

**必须记录原始直方图和极值，不能只记录平均值。** 实时报告必须绑定内核、CPU/IRQ 亲和性、网卡驱动/固件、卸载设置、拓扑、从站固件、构建版本、编译参数、测试时长和后台负载，并说明时钟源与测量方法。


### 可靠性的分层证据

本项目的“可靠”必须针对明确行为、配置、负载和观察时长判断，不能由单个状态或统计量给出。以下检查构成逐层证据，后一层不能由前一层自动推出。

| 要验证的行为 | 当前检查及源码 | 能支持的结论 | 仍需补充的证据 |
| --- | --- | --- | --- |
| 接入了期望设备 | `topology_mapper.c`、`session_setup.c`：扫描身份、匹配轴、核对 PDO 布局与换算 | 本次扫描与所用配置的匹配条件成立 | 同型号物理轴绑定、布线和外部轴标签核对 |
| 本周期过程访问完整 | `session_setup.c` 从映像计算预期 WKC；`session_exchange.c` 比较 SOEM 返回 WKC | 回帧的访问计数与预期一致 | 逐轴目标锁存、内部执行、机械动作；WKC 不是目标值校验码或运动完成确认 |
| 主机没有越过软件期限 | `cycle_clock.c`、`session_exchange.c`：单调时间与下一截止点比较，统计发送迟到与各段耗时 | 已采样的软件执行点符合期限条件 | NIC 发出时刻、驱动器接收截止点、未覆盖代码段及真实最坏执行时间 |
| 目标集合满足输入约束 | `session_target.c`：轴数、软件位置范围和单步变化验证；`coordinator.c`：内部序号与期限检查 | 通过检查的候选集合可进入下一帧输出 | 外部发送顺序、端到端年龄、重放防护、连续轨迹速度/加速度约束 |
| 驱动器可接受运动目标 | `controller.c`、`session_control.c`：状态字解码、模式反馈、全轴使能条件 | 所读反馈满足控制路径的使能/模式条件 | 反馈新鲜度与设备固件定义；已使能不证明目标已执行 |
| 机械动作符合要求 | `session_control.c`：跟随误差、固定轨迹终点和方向检查；观测输出实际位置 | 主站采样点内的误差未超所配置护栏 | 产品精度阈值、完整目标/反馈时间序列、外部位置测量和负载条件 |
| 停止动作被确认 | `session_shutdown.c`：撤使能写入、逐轴状态字读取及设备停用掩码 | 有效读回满足软件停用条件 | 停止时间上界、机械停止与电气断能；总线完全中断时依赖外部保护 |
| Sync0 同步满足锁存要求 | §22 的参考 DC、相位跟随和裕量估计 | 软件同步模型与配置条件达标 | 各站同步残差及 PDO/SM2 与 Sync0 的关联测量，当前尚不足以证明真实锁存余量 |

恢复策略决定发现异常后的软件动作，不是可靠性合格线。稳定性验收按测试文档要求无异常；故障注入另验证检测、记录、停止和恢复语义。短时零故障或平均延迟较小，都不能证明最坏延迟、长期故障率或功能安全等级。

## 25 功能状态与验证边界

| 能力 | 当前实现与证据边界 |
| --- | --- |
| 发现、身份校验、PDO、CiA 402、DC、CSP | 已有实现；是否满足验收以测试文档矩阵为准 |
| 多轴外部位置目标 | 已有 socket、客户端与 GUI；需逐轴验证运动、目标超时和重连 |
| 位置护栏、跟随误差、WKC/截止期监控 | 已有配置与处理路径；保护值不是精度验收线 |
| 停机与诊断 | 已有 PDO 停用及邮箱补偿路径；缺少逐轴有效读回不能证明禁用成功 |
| 慢速遥测、时序统计、异常窗口、报告 | 已有实现；观测负载需 A/B 验证，快照不是完整历史 |
| GUI、TUI、TCP 桥 | 已有工具；桥无认证，使用受控管理网络 |
| CST/CSV/混合模式 | 配置或枚举存在不代表完整控制链可用，不纳入本次 CSP 验收 |
| 长时稳定性、精度、故障恢复 | 尚无充分统一验收证据，见测试文档 |
| 热插拔、动态轴数、多主站协同、功能安全 | 不作为当前已支持能力承诺 |

## 26 构建、目录与产物

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

现有 `tests/` 通过 CTest 运行，`scripts/checks/` 验证配置链、报告与工具行为；构建检查不能替代真机协议和运动验证。

---

## 27 端到端数据流与动作时序

### 启动：配置到 OP

```text
config/*.json
  └─ CMake 配置期生成器（tools/generate_*.py）→ build/generated/*.c
       └─ emaster-master 读取编译期配置 → 查 deployment_id → 构造 session plan
            └─ SOEM 扫描从站 → 校验身份、拓扑、PDO 与能力
                 └─ PRE-OP：读身份/换算/初始 CiA 402；选 PDO、写启动 SDO、配 DC
                      └─ SAFE-OP：建立 process image，读取实际位置并初始化目标
                           └─ OP：确认全轴运行模式和使能 → 按周期开始控制
```

入口和文件对应关系：

| 步骤 | 主要源码 |
| --- | --- |
| 参数解析、部署查找、实时调度、回调装配、报告发布 | `tools/master/main.c`、`tools/master/console.c` |
| JSON 到生成 C | `CMakeLists.txt`、`tools/generate_config_apply.py`、`generate_catalog_apply.py`、`generate_runtime_apply.py`、`generate_deployment_apply.py`、`generate_slave_catalog.py`（被前者 import）、`generate_error_recovery_config.py`、`generate_messages.py` |
| 部署与轴计划校验 | `src/session/session_plan.c`、`src/config/`、`src/catalog/` |
| SOEM 初始化、身份与 PDO/DC 配置 | `src/bus/soem/control_session.c`、`session_setup.c`、`soem_common.c` |
| SAFE-OP 到 OP、起始位置与控制字初始值 | `session_start.c`、`cia_process_image.c` |
| PDO 描述与字节编解码 | `src/protocol/pdo_layout.c`、`pdo_codec.c`、`cia_process_image.c` |

配置层表达期望，从站扫描结果表达现场事实；身份或 PDO 不匹配应拒绝建立周期控制。安全门在 `src/safety/gate.c` 汇总拓扑、PDO、输出初始化、通信、反馈、模式、同步、目标、驱动器限位、使能授权、停止和故障锁存条件；`session_supervisor.c` 将决策接入主循环。

### 每 1 ms 周期：反馈、目标与下一帧

```text
CLOCK_MONOTONIC 绝对截止点唤醒
  → SOEM send/receive（按计划相位交换 PDO）
  → 检查 WKC、截止期、参考 DC/Sync0 时序并保存首故障现场
  → 按轴解码 PDO：状态字、模式、位置/速度/力矩反馈、限位与故障
  → CiA 402 controller 计算下一控制字
  → 多轴 coordinator 统一检查轴集合与轨迹进度
  → 安全门批准后检查外部/固定运动目标、限位和单步增量
  → 编码完整 PDO 输出，进入下一周期
```

主要实现位于 `session_control.c`（周期控制总编排）、`session_exchange.c`（定时和 SOEM 交换）、`cia_process_image.c`（轴 PDO）、`src/cia402/controller.c`（状态机）、`src/multiaxis/coordinator.c`（全轴一致性）、`src/motion/`（相对位置/速度轨迹）和 `session_target.c`（运行时目标验证）。`cycle_clock.c` 负责绝对时基与 DC 参考相位修正。截止点已错过时不补发旧帧；恢复行为需结合选定策略和实际检查路径判断。周期内 ESC 错误计数器 FPRD 诊断读有抽样和预算限制，它不是邮箱 SDO。

控制生命周期与数据源必须分清：配置运动方案时由 `src/motion/` 生成目标；外部控制时 `tools/master/main.c` 的 `position_target_source()` 从互斥锁保护的目标缓冲取完整快照。`session_target.c` 在所有轴通过检查后才提交目标，随后 PDO 编码才对总线生效。命令“成功”只代表已接收/进入处理，不等于驱动器已到位；应继续看反馈、following error 与逐轴结果。

### 外部命令与观测数据

```text
GUI/TUI/CLI
  → tools/emaster_client.py / tools/interactive_control.py
  → AF_UNIX SOCK_STREAM 命令 socket
  → command_server.c 监听线程解析并排队
  → 周期控制线程取命令，读状态/更新目标/请求停机
  → 非阻塞 send 回应 OK|... 或 ERROR|...
```

命令线程主动退出实时策略；响应使用 `MSG_DONTWAIT`，短写/阻塞视作慢客户端并关闭连接，避免实时线程被 socket 写阻塞。状态类命令会在周期线程执行；客户端请求频率不等于周期频率。

观测分为快、慢两路。快路由周期线程把 PDO 反馈组为帧写入 `src/bus/observation/ring.c`，独立观测 socket 供 `tools/observation_client.py` 解码；发布耗时计入报告。慢路由 `session_observer_thread.c` 通过邮箱 SDO 读设备 `slow_telemetry` 配置项，写线程安全快照；周期线程只拷贝已采样值，不执行这些 SDO。停机时请求 observer 退出，在内联停机序言各周期继续发送 PDO 等待线程退出，然后 join 并把快照写入报告。

报告由 `src/audit/run_audit.c` 累积总线访问和有效性信息，`src/audit/report_sections.c`、`run_report.c` 序列化 JSON，入口在会话完全收尾后原子发布。实时周期内不写报告文件。分析脚本位于 `scripts/analysis/`，采集编排脚本位于 `scripts/bench_*.sh`。

### 停止与异常动作

```text
SIGINT / stop / shutdown / 应用 stop callback
  → session_supervisor / 安全门锁存停止请求与来源
  → 请求 observer 停止，在可通信时逐周期 drain 并采集停机 AL 快照
  → SAFE_STOP：撤销各轴使能、检查停用状态
  → 停止 Sync0、邮箱最终诊断、请求从站回 INIT
  → 汇总审计并发布报告
```

停机实现主要在 `session_shutdown.c`；`main.c` 的 SIGINT handler 只设置 `sig_atomic_t` 标志，不在信号上下文中做复杂 EtherCAT 操作。命令 `shutdown` 请求同一有序退出路径。`quick_stop` 设置 CiA 402 quick-stop goal，但具体驱动器反应与最大时间仍需硬件验证；软件命令不能替代 STO/急停。掉出 OP 后 PDO 不再可靠，最终停用需要可用的邮箱和从站响应；总线完全中断时软件不能保证电气断能。

## 28 当前实现限制与风险边界

下表描述当前代码可直接确认的限制和风险。它们属于使用边界；是否满足产品需求仍需按测试文档验证。

| 风险 | 代码证据与影响 | 建议 |
| --- | --- | --- |
| 本机命令口权限过宽 | `src/bus/soem/command_server.c` 将 socket `chmod 0666`；无认证，可由本机任意用户控制运动、故障复位或停机 | 改专用组和 `0660`，明确服务用户边界；TCP 桥增加认证/访问控制 |
| 命令接收依赖一次 `read()` | 同一文件对 `SOCK_STREAM` 单次 read 缓冲后立即按命令比较；TCP/Unix stream 不保留写入消息边界，拆包会拒绝合法命令，粘包可能把多条合为一条。目标缓冲还可能将超长输入截断后按局部有效值处理 | 做换行累积解析、最大行长拒绝和一连接多命令测试；完整一行验证后才更新全轴快照 |
| 外部目标年龄未证实为安全停机 | `main.c` 注释说明超时后 CSP/HOLD 保持上个位置；函数将 `available` 清零后也会保持最后位置。若产品要求断连后撤使能或受控减速，现行为不满足需求 | 冻结断连动作需求；实施并验证超时后停止/减速，CST 单独确认归零力矩和限幅 |
| 每轴保护统一使用首轴比例 | `main.c` 用 `plan.axes[0]` 的运动配置/换算计算 following error 与 max step counts，再作为统一回调限制 | 每轴分别换算并检查；异齿轮比/异型号拓扑加入配置校验和测试 |
| 同型号从站不能唯一确定物理轴 | `topology_mapper.c` 按 vendor/product/revision 选择首个未分配匹配项；同型号站没有序列号或物理标签参与映射 | 轴顺序变化、换线或同型号互换后，软件轴可能对应不同物理关节；必须使用固定拓扑和外部轴标识验证 |
| 部分 DC/停机访问使用轴序号 | PDO 路径使用映射后的总线位置，但 `session_setup.c` 的 Sync0 配置及 `session_shutdown.c` 的关闭 Sync0/部分邮箱路径仍有 `axis_index + 1` | 配置轴与总线位置不连续时可能读写错误从站；此类拓扑不能直接宣称支持 |
| DC 样本没有独立新鲜度标记 | `session_exchange.c` 以 `context.DCtime > 0` 判断有效；SOEM 未收到新响应时可能保留上一值 | `dc_startup_stable` 和 PI 反馈可能包含旧时间；需要按数据报有效性和周期序号复核 |
| DC/Sync0 可靠性是模型估计 | `sync0_margin_ns` 使用参考 DCtime、启动期 `pdelay` 和配置 Sync0 相位，没有直接采集 Sync0 边沿或 SM2 完成时间 | 非负 margin 不能证明目标已在正确 Sync0 前由每个驱动器锁存 |
| 观测线程共享普通 `volatile bool` | `session_internal.h` 的 `observer_running` / `observer_exited` 在 observer 与周期线程间读写；`volatile` 不提供 C 线程同步或原子语义，构成潜在数据竞争 | 改 C11 `_Atomic` 或明确的 mutex/condition 同步，并以 ThreadSanitizer/并发测试验证 |
| 命令线程生命周期与连接并发 | `command_server.c` 的 `running` 是跨线程普通 bool；响应取 fd 快照后解锁再 send，监听线程同时可能 close/accept，数值相同的 fd 可能已代表新连接；队列项也未绑定连接代次 | 生命周期标志用原子或锁；明确连接所有权、代次与响应归属，覆盖快速断连/重连场景 |
| 默认软限位与首次单步检查存在例外 | `session_target.c` 读回 min=max 时放行位置范围；首目标未提交时跳过单步检查。当前 master 会先提交初始位置，但其它回调用户不能依赖这一入口行为 | 对无有效限位给出明确配置和报告；初始化已提交目标基准，验证首条外部目标的大跳变 |
| 模式之间的超时语义不统一 | `main.c` 仅按第一轴是否 CST 选择归零策略，其余模式走保持目标分支。CSV 若继续保持非零速度目标，不能据 CSP 的“保持位置”说明判定已停止；混合模式也不能由首轴决定全轴语义 | 按每轴模式定义失联动作；本次仅 CSP 验收，CSV/混合模式使用前需专项验证 |
| 脚本使用全局 `pkill -9` | `scripts/bench_*.sh`、`scripts/test_*.sh` 多处按名称杀进程，会跳过有序停机并可能杀错部署 | 优先按部署 socket 发送 shutdown，保存 PID 并 TERM/等待；只在人工确认后用 KILL 作为恢复措施 |

审查时特别区分代码与注释：`main.c` 回调注释仍写“从未收到外部目标生成正弦波”，但实际分支保持初始位置，没有自动启动 demo；本文按实现说明。测试文档的 F7/F8 可用于安排流协议、断连行为和有序停机验证。

# 附：文档

**本项目只有两份文档：**

| 文件 | 内容 |
| --- | --- |
| `README.md` | 本文件：使用说明 + 项目实现 |
| [`wholetest/采集.md`](wholetest/采集.md) | 最终测试方案、执行判据、测试采集与待解决问题 |

厂商资料的版本、限值和来源须与实际设备固件对应；仓库中的配置值本身不构成厂商保证。
