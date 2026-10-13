# EtherCAT Master

面向机器人控制的 EtherCAT 主站。C11 + SOEM。支持配置为 1 ms DC 同步的多轴 CiA 402 控制。

**本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。详见第 11 节。

可靠性结论必须依据本文的测量边界和[测试方案](docs/test-plan.md)的判据。

## 文档

| 文件 | 内容 |
| --- | --- |
| `README.md` | 本文件：交付内容、上手、运行、配置、命令接口、工具、排查 |
| [`docs/architecture.md`](docs/architecture.md) | 实现说明：架构、周期、两层状态、报告、DC 与同步 |
| [`docs/test-plan.md`](docs/test-plan.md) | 测试条件、判据、已采集的证据、未决项 |
| [`docs/time-and-space.txt`](docs/time-and-space.txt) | 对象、空间、时间与读写的运行期资料 |
| `esi/README.md` | 设备描述放哪、从哪来 |
| `config/README.md` | 配置各族逐个字段的语义 |
| `gen/device/README.md` | 设备上手工具的边界 |

---

## 1 交付内容

交付的是一棵源码树。**树里不含任何电机的配置。**

你必须自己带进来两样东西：

- 电机厂商的设备描述原文件。EtherCAT 是 ESI（XML），CANopen 是 EDS。
- 现场参数：网口名、轴数与总线顺序、正方向、零点和限幅。

```text
产品
  src/              实现，按功能分模块
  include/emaster/  公共头，与 src/<模块> 一一对应
  apps/             可执行程序
  config-runtime/   配置的运行时形态。构建期由 gen/ 生成，主站启动时读它

配置（三层：设备描述 → 出厂设计 → 现场）
  esi/              设备描述。**出厂是空的**，放入你自己那台电机的 ESI/EDS
  config/           配置源。六族出厂为空：
                    devices/ device_policies/ topologies/ operation_profiles/
                    motion_profiles/ deployments/
                    另有随产品交付的两份：error_recovery_policies/、messages/

构建期
  gen/              生成器与校验器
  gen/device/       设备上手工具

运行期工具
  tools/            Python 工具：客户端、控制台、图形界面、观测、跨机桥

资料
  docs/             实现说明、测试方案、运行期资料
```

台架脚本、报告分析工具和测试证据属于开发侧，不在交付树里。它们的用法和已采集的结果见[测试方案](docs/test-plan.md)。

## 2 系统要求与构建

主站只支持 Linux。Windows 可以运行 Python 工具、图形界面和离线单元测试。

**主机要求**

- 使用 PREEMPT_RT 内核，并记录内核与配置哈希。
- 服务账号具备 `SCHED_FIFO` 权限，进程内存锁定（`mlockall`）。
- 周期线程使用专用 CPU。EtherCAT 网卡的中断亲和性经过明确配置。
- EtherCAT 使用**专用网卡**。网卡不配 IP，不跑路由、PTP、激光雷达或 NetworkManager。
- 网卡的中断合并和卸载参数已记录并经过有意配置。
- CPU 频率策略、温度限制和后台服务负载已记录。

`ulimit -r` 必须是 `unlimited`，否则主站拿不到实时优先级。

**构建依赖**：CMake ≥ 3.28、Python ≥ 3.10、C11 编译器。预设需要 Ninja。图形界面另需 PyQt5。`nc -U` 示例需要支持 Unix socket 的 netcat。

在仓库根目录执行：

```bash
git submodule update --init --recursive
python3 gen/validate_project.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEMASTER_BUILD_HARDWARE_TOOLS=ON -DEMASTER_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

主站产物是 `build/apps/master/emaster-master`。

不访问硬件的离线构建：

```bash
cmake -S . -B build-offline -DEMASTER_BUILD_HARDWARE_TOOLS=OFF -DEMASTER_BUILD_TESTS=ON
cmake --build build-offline
ctest --test-dir build-offline --output-on-failure
```

多配置生成器需分别加 `--config Debug` 和 `-C Debug`。Windows 不包含 Linux 专用的时钟测试和报告测试。

**离线通过不能替代真实总线和运动验证。**

## 3 快速上手：从 ESI 到主站跑起来

```bash
# 1 放入 ESI。一台型号一个目录，目录名随意
cp <厂商给的>/*.xml esi/<你的型号>/

# 2 从 ESI 生成设备档草稿和策略档草稿
python3 gen/emaster_config.py add-device esi/<你的型号>/*.xml

# 3 人工补上推不出来的字段。<文件名> 是第 2 步打印的
$EDITOR config/device_policies/<文件名>.json

# 4 生成拓扑、运行方案与部署。<轴数> 是环上的从站数，不给就是 1
#   设备档 ID 和第 4 步这条命令都由第 2 步打印
python3 gen/emaster_config.py init-site --device <设备档 ID> \
  --interface <网卡> --slaves <轴数>

# 5 写每轴换算与限幅
$EDITOR config/motion_profiles/<你的>.json

# 6 生成 config-runtime/*.flat
python3 gen/emaster_config.py build

# 7 起主站
sudo bash -c 'ulimit -r unlimited; exec env EMASTER_OBSERVATION=1 \
  ./build/apps/master/emaster-master --config-dir config-runtime'
```

**第 3 步是唯一推不出来的地方。** 第 2 步会打印一张"还需要人工确认"的清单，逐条处理它。

各步的产出与失败点：

| 步 | 产出 | 失败看哪 |
| --- | --- | --- |
| 2 | `config/devices/`、`config/device_policies/` | 退出码非 0，并打印冲突的 PDO 集合。原因：ESI 里既没有默认分配（`#x1C12`/`#x1C13` 的 `DefaultData`），也没有 `Sm="2"`/`Sm="3"` 标记 |
| 4 | `config/topologies/`、`config/operation_profiles/`、`config/deployments/` | 报"厂商没声明 DC"。原因：ESI 里没有 `<Dc><OpMode>` 段。两条路：补 ESI，或照实测手写一份 SM 方案。忘了 `--slaves` 会得到一份 1 轴的拓扑 |
| 6 | `config-runtime/*.flat` | 校验失败会逐个指名字段 |

`init-site` 的 `assign_activate` 取自 ESI，不猜。**不要自己编一个同步参数填上。** 填错会把从站的同步生成配成另一套，主机这边只看到 DC 对不上。

第 2、4、6 步都可以重复执行：

- 改了策略档，必须重跑第 2 步。只有它会合并策略档。
- 改了设备档或现场参数，必须重跑第 4 步。
- 任何配置改动，都必须重跑第 6 步并重启主站。

轴数来自部署档，硬件换算在主站启动时读回，恢复阈值来自错误恢复策略。

## 4 运行与停止

```bash
cd /path/to/ethercat-master
sudo bash -c 'ulimit -r unlimited; exec env EMASTER_OBSERVATION=1 \
  ./build/apps/master/emaster-master --deployment <部署ID>'
```

`--deployment` 接收部署 JSON 里的 `deployment_id`，不是文件名。也可以直接给 `--config-dir config-runtime`，它是默认值。

**`EMASTER_OBSERVATION=1` 不是可选项。** 不开的话观测口不建，报告里温度、母线电压、电流那几栏恒为 `—`，事后无法回答"那一刻电压是多少"。

**确认主站起来了。** 不要只看进程在。

```bash
pgrep -a -f emaster-master
ls /tmp/emaster-<部署ID>*.sock          # 命令口和观测口，两个
sudo timeout 3 nc -U /tmp/emaster-<部署ID>.sock <<< status | grep -o 'state=[0-9]*'
```

`state=4` 是 RUNNING。

```bash
sudo timeout 3 nc -U /tmp/emaster-<部署ID>.sock <<< topology
# OK|axes=8,max_step=63715|a1:bus=1,enc=16384,gear=28/1,torque=0|a2:...
```

**停止：Ctrl+C。** 主站注册了 SIGINT 和 SIGTERM，两者走**和命令口 `shutdown` 完全相同**的安全停机路径：安全输出 → 停用各轴 → 回 INIT。它不是掐断。

换部署前先经命令口发 `shutdown`，或向核实过的主站 PID 发 SIGTERM，等它退出并检查逐轴停用证据。**不要把 SIGKILL 当正常停机。**

网口、实时核、优先级和报告路径由部署决定。换主机时必须核对部署档。

## 5 命令接口

命令口是 AF_UNIX 流套接字，路径 `/tmp/emaster-<部署ID>.sock`。它接收换行结尾的文本命令。

**权限警告：`src/bus/io/command_server.c` 对套接字执行 `chmod 0666`，而且没有应用层认证。任何本机用户都能下发运动和停机命令。** 跨机桥 `tools/socket_bridge.py` 同样没有认证。只能在可信、隔离网络、单用户测试环境使用。

| 命令 | 用途与约束 |
| --- | --- |
| `status` | 查询主站及逐轴状态。主站 `state=4` 和 AL SAFE-OP 的数值 4 属于不同枚举 |
| `topology` | 查询实际轴集合、位置换算和轴顺序。发送目标前先查询 |
| `dc` | 查询同步信息 |
| `set_external_target <p1> <p2> ...` | 按拓扑顺序提交**全轴**绝对原始位置计数。不是角度，不接受部分轴集合 |
| `switch <motion_profile_id>` | 切换运动方案。它不等于切换 CSP/CSV/CST 控制模式 |
| `stop` | 当前实现与 `shutdown` 一样请求会话有序退出。它不只是暂停轨迹，也不代表断电 |
| `shutdown` | 请求完整停机并退出 |
| `quick_stop`、`halt <参数>`、`fault_reset` | 解析器已有入口。入口存在不代表相关安全功能已经验收 |

发命令请复用 `tools/interactive_control.py`、`tools/emaster_client.py` 或控制台 Engine。不要假定一次 socket read 就是一条完整回复。

外部目标超时由 `external_target_timeout_ms` 指定，默认 200 ms。客户端 100 Hz 的目标更新率与主站 1 kHz 的 PDO 交换率不同。断连、目标暂停、重连必须分别测试。**不能假定自动恢复运动。**

**两层状态不能混。**

- **不能用 AL 状态推断驱动器使不使能。** 两个都要读。
- **不能用过程数据去送"停止周期通道"的命令。** 从站掉出 OP 后控制字到不了它。**禁用必须走邮箱。**
- AL 在 INIT 时读不到 `0x6041`，因为邮箱关闭了。这时只能说"读不到"。
- `0x0134` 只在 `0x0130` 的 bit4（Error Indicator）置起时才有意义。

机制与完整论证见[实现说明](docs/architecture.md)第 6 节。

## 6 观测与界面

观测口是独立的只读 unix 套接字 `/tmp/emaster-<部署ID>-obs.sock`。**它要 `EMASTER_OBSERVATION=1` 才存在。**

**主站认的环境变量只有这 8 个。**

| 变量 | 作用 |
| --- | --- |
| `EMASTER_OBSERVATION` | 建不建观测口。默认关 |
| `EMASTER_OBSERVER_VERBOSE` | 观测线程每约 1 秒打一条全部 12 项读数 |
| `EMASTER_FRAME_TIMEOUT_US` | 覆盖帧超时。默认周期/4 = 250 µs |
| `EMASTER_AUDIT_MAX_ACCESSES` | 审计访问记录容量上限 |
| `EMASTER_SHUTDOWN_FAST` | 停机快模式 |
| `EMASTER_SHUTDOWN_LEGACY_PROLOGUE` | 选择阻塞式停机序言 |
| `EMASTER_CMD_SERVER_VERBOSE` | 命令服务器详细日志 |
| `EMASTER_NO_FIRST_MISMATCH_AL_READ` | 首次不符时不读 AL |

设了 `EMASTER_OBSERVER_VERBOSE=1` 时，观测线程每约 1 秒在 stderr 打一条：

```text
[P4.3] 轴0: actual_current=9 0.001 A, cia402_error_code=0, bus_voltage=48104 mV,
       mosfet_temperature=391 0.1 degC, motor_temperature=344 0.1 degC,
       actual_motor_speed=0 rpm, speed_command=0 rpm,
       target_position_readback=3895 count, max_torque=1000, max_motor_speed=841045  (读取次数=221)
```

读哪些对象由设备档的 `slow_telemetry` 决定。加了 `EMASTER_OBSERVER_VERBOSE` 还会打出每条读数的对象地址与工作计数器。

**跨机图形界面**

主站和桥跑在目标主机，界面跑在另一台机器上。

```bash
# 目标主机
sudo python3 tools/socket_bridge.py --deployment <部署ID> --bind 0.0.0.0 --port 5001
```

```powershell
# 另一台机器
cd D:\taili\ethercat_master
$env:PYTHONPATH='tools'
python -m emaster_gui --tcp <host>:5001
```

- 端口约定：命令 5001、观测 5002。界面自动用 PORT+1，不用另给。
- `--tcp` 同时开了命令通道，也就是界面能驱动电机。只读改用 `--obs-endpoint tcp:<host>:5002`。
- 桥会绑到主站实时核之外的核。
- 桥监听 `0.0.0.0` 时会打一句警告：**这两个口没有任何认证，能连上命令口的人就能驱动电机。**

**终端面板（同机）**

```bash
sudo bash -c 'ulimit -r unlimited; exec python3 tools/emaster_console.py \
  --repo "$PWD" --deployment <部署ID> --start'
```

`--range` 是自助软范围（默认 45°），`--jog-speed` 是点动速度（默认 10 °/s），`--start` 表示套接字不在时替你拉起主站。终端面板与图形界面共用同一份控制律。

## 7 配置模型

配置分三层：**设备描述 → 出厂设计 → 现场**。交付时 `esi/` 和 `config/` 下的六族都是空的。

| 目录 | 谁写 | 内容 |
| --- | --- | --- |
| `esi/<你的型号>/` | 你放进去 | 厂商的 ESI/EDS 原文件。设备档的权威来源 |
| `device_policies/` | `add-device` 出草稿，人改 | 人写的那一半：停机掩码、PDO 组选择、支持的模式集合 |
| `devices/` | `add-device` 合成 | 设备档：身份、PDO 映射、`slow_telemetry` 清单、状态字掩码 |
| `topologies/` | `init-site` | 拓扑：环上有哪些从站、顺序、每台的身份期望与位置 |
| `operation_profiles/` | `init-site` | 运行方案：周期、DC 策略（`dc`/`sm`）、SM2/SM3 同步类型、相位与偏移 |
| `motion_profiles/` | 你写 | 运动方案：单步上限、跟随误差限幅、轨迹、每轴的换算 |
| `deployments/` | `init-site` | 部署：主机名、网口、轴数、引用哪套拓扑／运行方案／运动方案、报告路径 |
| `error_recovery_policies/` | 随产品交付 | 容错阈值。与电机无关 |
| `messages/zh_CN.json` | 随产品交付 | 中文文案 |

**换一款电机要动的只有前七行。** 那是跑命令，不是改代码。命令见第 3 节。

每族字段的完整语义见 `config/README.md`。设备上手工具的边界见 `gen/device/README.md`。

**换电机、换拓扑、换部署都不需要重编主站。** 六族的条目全部在运行期从 `config-runtime/` 的扁平文件读，生成的 C 与配置内容无关。改 `error_recovery_policies/` 才需要重编，那一族是编译期静态表。

只改 JSON 而运行旧二进制，不能保证生效。任何配置改动都要重跑 `build` 并重启主站。

`--deployment` 接收 JSON 里的 `deployment_id`，不是文件名。

## 8 工具清单

都在 `build/apps/<目录>/` 下。

| 工具 | 说明 |
| --- | --- |
| `emaster-master` | 主站 |
| `emaster-scan` | 只读扫描：逐台身份、AL 状态、`0x6041`、故障码、换算 |
| `emaster-disable` | **走邮箱把驱动器失能**。主站起不来时也能用 |
| `emaster-watch [间隔ms] --deployment <ID>` | 实时监控位置／速度／力矩／状态字 |
| `emaster-move-deg --deployment <ID>` | 交互式角度控制，自动换算 |
| `emaster-move --deployment <ID>` | 交互式脉冲控制，底层接口 |
| `emaster-fingerprint` | 硬件指纹。最高只到 PRE-OP |
| `emaster-dc-prepare` | PRE-OP DC 准备。不改状态 |
| `emaster-target-probe` | 目标源探针 |

`tools/` 下是 Python 工具：`emaster_client.py`（命令口客户端）、`interactive_control.py`、`emaster_console.py`（终端面板引擎）、`emaster_gui/`（图形界面）、`observation_client.py`（观测口）、`socket_bridge.py`（跨机桥）。

**`emaster-scan` 会把驱动器拉出 OP 再恢复**，因为它要发 INIT→PRE-OP 请求。**运行中不要用它。**

**`emaster-disable` 会让电机立刻失去力矩保持。** 带重力负载、或抱闸未抱时不要跑。

## 9 单位与换算

换算由每轴主站启动时读回的编码器分辨率和齿轮比确定。**不能把某个拓扑的比例用于全部设备。** `topology` 命令返回运行时换算。

```text
电机每转计数 = encoder_increments / encoder_motor_revolutions
输出轴每转计数 = 电机每转计数 × gear_motor_revolutions / gear_shaft_revolutions
相对位移计数 = 输出轴角度 / 360 × 输出轴每转计数
绝对位置目标 = 确认的起点位置计数 + 相对位移计数
```

例：编码器 16384 counts/电机转、齿轮比 28:1，对应 458752 counts/输出轴转，约 1274.31 counts/度。25:1 对应约 1137.78 counts/度。

还须核对正方向、机械零点和整数舍入。**原始位置零点不是自动校准出的机械零点。**

单步上限由运动方案的 `max_step_millidegrees` 决定，取值看 `topology` 回报的 `max_step`。

**单步超限不返回错误，而是判定 `MOTION_INVALID` 后中止整个会话、轴失能。** 客户端要把大角度拆成小步流式发送。

超过 `external_target_timeout_ms` 没收到新目标时，CSP 入口回调返回 `HOLD`，保留最后目标。它不等于驱动器失能。

算法见 `src/motion/position_target.c`，SDO 读回与配置见 `src/bus/session/session_setup.c`。

## 10 故障排查

| 现象 | 先查什么 |
| --- | --- |
| 主站起不来 | `ulimit -r` 是不是 `unlimited`。部署声明了 `realtime.required=true` 时，拿不到 `SCHED_FIFO` 主站会拒绝进入 OP |
| 进不了 OP | 用 `status` 看主站状态，再看逐轴 AL 状态。拓扑或身份不匹配会拒绝建立周期控制 |
| 轴不动 | 见下面两张表 |
| 命令口连不上 | 主站刚启动的几十秒内，命令套接字只有一个 accept 位。客户端不要快节奏重连 |
| 报告里温度／电压／电流恒为 `—` | 主站启动时有没有 `EMASTER_OBSERVATION=1` |
| 报告里字段是 `-` | 取错了层。见[测试方案](docs/test-plan.md)的"报告字段层级"一节 |
| 收尾显示 `OP=通过`，但有轴不在 OP | 那句走地址 0 广播，只照见环上最后一站。判"哪几根不在 OP"要看逐轴 `shutdown_al_entry` |
| 逐轴 AL 读到 `state=0` | `0` 不是"在 INIT"。SOEM 先清零缓冲再读，所以 0 只可能是**这一站没应答** |

**状态字 `0x6041` 的读法**

| 值 | 标准状态 |
| --- | --- |
| `0x1237` | Operation enabled。bit3=0、bit10=0、bit11=0，不应解读为"目标到达" |
| `0x1233` | Switched on。bit2=0、bit10=0，不能据此推断 AL 是否 SAFE-OP |
| `0x1250` | Switch on disabled，已失能 |
| `0x1210` | Not ready to switch on。出现原因仍需结合启动阶段和设备诊断 |

译名依据是 CiA 402 状态掩码，厂商扩展位按对应固件手册解释。

**只看停机后的状态字不能推断运行期间是否使能。** 审计段按阶段记录读到的对象，要按阶段看。

**"轴不动"的检查项**

| 检查项 | 证据与边界 |
| --- | --- |
| 目标传递 | 比较客户端发送值、主站 PDO 输出审计与有效的 `0x607A` 读回。读回一致不证明固件已经在预期周期执行 |
| 使能与模式 | 按状态掩码解码 `0x6041`，并核对 `0x6061`。只检查 bit2 或控制字 `0x000F` 不充分 |
| 限幅 | 核对有效的 `0x6072`、`0x6080` 与状态字 bit11。扩展保护仍需厂商定义 |
| 驱动器故障 | 比较状态字、`0x603F` 与厂商故障对象。慢速读取可能漏掉短暂故障 |
| 温度／母线 | 检查慢速遥测的有效位、年龄和厂商限值。退出快照不证明故障瞬间正常 |
| 链路 | 联合工作计数器、网卡计数、ESC 错误计数及读取有效性。零计数不能排除调度延迟或未采集事件 |
| 同步 | 比较 DC 与 freerun 行为，并按[实现说明](docs/architecture.md)第 10 节的测量边界解释 Sync0。软件估计不能替代锁存测量 |

## 11 安全边界

**本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。

- **指纹工具和 PRE-OP DC 准备工具的最高 AL 状态仍为 PRE-OP。** 它们分别只执行受限的只读采集或参数准备。
- **`emaster-master` 不在 PRE-OP 指纹流程范围内。** 它会请求 SAFE-OP/OP，并把驱动器推进到运行使能。
- 进入 CSP 前，以 `6064` 实际位置初始化 `607A`。
- 停止和失败路径按状态反馈**逐级撤销使能**，并以设备配置中的 `6041` 掩码和值确认停用，随后关闭 Sync0 并恢复 INIT。
- **获得 PRE-OP 指纹运行许可，不代表获得 SAFE-OP、OP、CiA 402 使能或运动授权。** 每个更高的边界都需要独立流程和批准。

实现与使用约束：

- 任何过程数据交换开始前，启动输出必须完全初始化。
- **硬件使能是生命周期状态，不能是编译开关。**
- 网络进入更高状态前，拓扑和身份必须匹配。
- 当前外部命令没有发送序号和失效时间。
- 通信丢失、命令过期、反馈无效和进程退出都必须有明确停机反应。
- 恢复过程必须有界、可记录，且**不能静默复位驱动器故障**。
- 单轴和整机停机策略必须明确且可测试。
- 报告文件在周期外写入。周期内仍有有预算的寄存器诊断和命令处理，须计入实时验证。

**每份硬件测试记录必须说明**：操作者、观察者、物理装置、功率限制、机械约束、STO/急停验证、网络接口、软件版本、配置哈希、ESI 哈希和全部从站序列号。

## 12 已知限制

下表是当前代码可直接确认的限制。它们属于使用边界。

| 限制 | 证据与影响 |
| --- | --- |
| 命令口权限过宽 | `src/bus/io/command_server.c` 将套接字 `chmod 0666`，无认证。本机任意用户都能控制运动、故障复位或停机 |
| 命令接收依赖一次 `read()` | 同一文件对 `SOCK_STREAM` 单次 read 缓冲后立即按命令比较。拆包会拒绝合法命令，粘包可能把多条合为一条，超长输入可能被截断后按局部有效值处理 |
| 外部目标超时不构成安全停机 | `main.c` 在超时后让 CSP 保持上个位置。若产品要求断连后撤使能或受控减速，现行为不满足需求 |
| 每轴保护统一使用首轴比例 | `main.c` 用 `plan.axes[0]` 的运动配置和换算计算跟随误差与单步计数上限，再作为统一回调限制 |
| 同型号从站不能唯一确定物理轴 | `topology_mapper.c` 按 vendor/product/revision 选择首个未分配的匹配项，没有序列号或物理标签参与映射。**环序不等于物理关节标签** |
| DC/Sync0 可靠性是模型估计 | `sync0_margin_ns` 使用参考 DC 时间、启动期 `pdelay` 和配置的 Sync0 相位，没有直接采集 Sync0 边沿或 SM2 完成时间。非负裕量不能证明目标已在正确的 Sync0 前由每个驱动器锁存 |
| 默认软限位与首次单步检查存在例外 | `session_target.c` 在读回 min=max 时放行位置范围，首目标未提交时跳过单步检查 |
| 模式之间的超时语义不统一 | `main.c` 仅按第一轴是否 CST 选择归零策略，其余模式走保持目标分支。CSV 若继续保持非零速度目标，不能据 CSP 的"保持位置"说明判定已停止 |

**不作为当前已支持能力承诺**：热插拔、动态轴数、多主站协同、功能安全。

代码内部的风险不在本表，见[实现说明](docs/architecture.md)第 14 节。

## 13 架构概述

主站按绝对截止点周期运行。配置层表达期望，从站扫描结果表达现场事实。

```text
config/*.json
  └─ gen/ 生成 config-runtime/*.flat
       └─ emaster-master 读扁平文件 → 查 deployment_id → 构造会话计划
            └─ SOEM 扫描从站 → 校验身份、拓扑、PDO 与能力
                 └─ PRE-OP：读身份、换算、初始 CiA 402；选 PDO、写启动 SDO、配 DC
                      └─ SAFE-OP：建立过程映像，读回实际位置并初始化目标
                           └─ OP：确认全轴运行模式和使能 → 开始 1 ms 周期控制
                                └─ 停机：撤使能 → 关 Sync0 → 诊断 → 回 INIT → 发布报告
```

目录按职责组织，CMake target 约束依赖方向。公共接口不暴露 SOEM 类型。设备、部署与方案由配置表达。

周期线程按绝对截止点唤醒后立即发送，接收完成后再记录审计、计算相位修正、处理反馈并生成下一帧。期限已错过时退出正常控制，不补发历史帧。

完整说明见[实现说明](docs/architecture.md)。

---

## 附：文档版本

本文按仓库中的当前实现和实际函数调用说明行为。部署时应使用与源码匹配的构建产物。

厂商资料的版本、限值和来源须与实际设备固件对应。仓库中的配置值本身不构成厂商保证。
