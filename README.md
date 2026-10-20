# EtherCAT Master

面向机器人控制的 EtherCAT 主站：C11 + SOEM，1 ms DC 同步，多轴 CiA 402。

**本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。

交付的是一棵源码树。**树里不含任何电机的配置。** 你必须自己带进来两样东西：

- 电机厂商的 ESI 文件（XML）。**本项目只读 ESI，不收 CANopen 的 EDS。**
- 现场参数：网口名、轴数与总线顺序、正方向、零点和限幅。

```text
产品
  src/              实现，按功能分模块
  include/emaster/  公共头，与 src/<模块> 一一对应
  apps/             可执行程序
  config-runtime/   配置的运行时形态。构建期由 tools/gen/ 生成，主站启动时读它

配置（三层：设备描述 → 出厂设计 → 现场）
  esi/              设备描述。**出厂是空的**，放入你自己那台电机的 ESI（XML）
  config/           配置源。六族出厂为空：
                    devices/ device_policies/ topologies/ operation_profiles/
                    motion_profiles/ deployments/
                    另有随产品交付的两份：error_recovery_policies/、messages/

外部依赖
  external/SOEM/    SOEM 源码。随交付树给你，构建不需要联网

构建期
  CMakeLists.txt    顶层构建入口
  CMakePresets.json 构建预设
  cmake/            CMake 辅助模块

工具
  tools/            Python 工具：客户端、控制台、图形界面、观测、设点、跨机桥
  tools/gen/        生成器与校验器；tools/gen/device/ 是设备上手工具
  tools/analysis/   报告离线判读工具
  tools/bench/      台架脚本

资料
  docs/             实现说明、接口协议、测试方案、功能对标、运行期资料

开发与验证
  tests/            单元测试。安装一节的 ctest 要用它，构建时不能少
  tests/checks/     离线检查脚本；ci_replay.sh 一条命令按 CI 顺序跑完整套
  evidence/         已采集的测试证据
```

`开发与验证` 那几族是开发与台架用的，可以不看；**但 `tests/` 别删**，安装一节的 `ctest` 要用它。台架脚本与证据的用法见[测试方案](docs/test-plan.md)。

## 文档

| 文件 | 内容 |
| --- | --- |
| `README.md` | 本文件：交付内容、安装、用法、命令表 |
| [`docs/architecture.md`](docs/architecture.md) | 实现说明：架构、周期、两层状态、报告、DC 与同步 |
| [`docs/protocol/command-protocol-v1.md`](docs/protocol/command-protocol-v1.md) | 三个套接字的协议：命令文法、响应文法、设点记录格式 |
| [`docs/protocol/target-channel-contract.md`](docs/protocol/target-channel-contract.md) | 目标通道的行为契约：谁给、一次多少、多快、断供怎么办、允许哪些模式、零点在哪 |
| [`docs/test-plan.md`](docs/test-plan.md) | 测试条件、判据、已采集的证据、未决项 |
| [`docs/time-and-space.txt`](docs/time-and-space.txt) | 对象、空间、时间与读写的运行期资料 |
| [`docs/product/twincat-function-gap.md`](docs/product/twincat-function-gap.md) | 功能对标：本主站在哪一层、与 TwinCAT 逐层比什么、明确不做什么 |
| [`docs/product/verified-combinations.md`](docs/product/verified-combinations.md) | 有台架证据的驱动与模式组合；未列出的按未验证处理 |
| [`docs/product/error-codes.md`](docs/product/error-codes.md) | 错误分类的稳定编号、含义、证据字段与下一步 |
| `esi/README.md` | 设备描述放哪、从哪来 |
| `config/README.md` | 配置各族逐个字段的语义 |
| `tools/gen/device/README.md` | 设备上手工具的边界 |
| `CONTRIBUTING.md` | 构建、测试与提交要求 |

## 安装

主站只支持 Linux。Windows 可以运行 Python 工具、图形界面和离线单元测试。

**主机要求**

- 使用 PREEMPT_RT 内核，并记录内核与配置哈希。
- 服务账号具备 `SCHED_FIFO` 权限，进程内存锁定（`mlockall`）。
- 周期线程使用专用 CPU。EtherCAT 网卡的中断亲和性经过明确配置。
- EtherCAT 使用**专用网卡**。网卡不配 IP，不跑路由、PTP、激光雷达或 NetworkManager。
- 网卡的中断合并和卸载参数已记录并经过有意配置。
- CPU 频率策略、温度限制和后台服务负载已记录。

`ulimit -r` 必须是 `unlimited`，否则主站拿不到实时优先级。

**构建依赖**：CMake ≥ 3.28、Python ≥ 3.10、C11 编译器。预设需要 Ninja。图形界面另需 PyQt5。SOEM 的源码在 `external/SOEM` 里，随交付树一起给你。**构建不需要联网。**

在仓库根目录执行：

```bash
python3 tools/gen/validate_project.py
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

## 用法

### 从 ESI 到主站跑起来

前提：安装一节构建完成。每条命令都在仓库根目录执行。

```bash
# 1 放入 ESI。一台型号一个目录，目录名随意
cp <厂商给的>/*.xml esi/<你的型号>/

# 2 从 ESI 生成设备档和策略档
python3 tools/gen/emaster_config.py add-device esi/<你的型号>/*.xml

# 3 补上推不出来的字段。<文件名> 由第 2 步打印
$EDITOR config/device_policies/<文件名>.json

# 4 只读扫描总线：环上有几台、每台各是什么型号。要 root：原始套接字
sudo build/apps/diagnose_slave/emaster-scan <网卡> > scan.txt

# 5 生成拓扑、运行方案与部署。轴数、逐台身份与序列号都取自扫描
python3 tools/gen/emaster_config.py init-site --device <设备档 ID> \
  --interface <网卡> --from-scan scan.txt

# 6 只有要跑内置的固定轨迹才做这两条：写运动方案，再把它挂到部署上
#   每轴的 axis_id 见 config/topologies/<拓扑>.json，required_mode_id 要等于模式
$EDITOR config/motion_profiles/<运动方案 ID>.json
python3 tools/gen/emaster_config.py init-site --device <设备档 ID> \
  --interface <网卡> --from-scan scan.txt --motion <运动方案 ID> --force

# 7 生成 config-runtime/*.flat
python3 tools/gen/emaster_config.py build

# 8 起主站
sudo bash -c 'ulimit -r unlimited; exec \
  ./build/apps/master/emaster-master --config-dir config-runtime'
```

每一步都会打印它下一步该跑的命令；设备档 ID 由第 2 步打印。

- **第 3、6 步要人给值。** 第 3 步按第 2 步打印的待确认清单逐条处理。第 6 步的换算与限幅推不出来，只能自己填。
- **不挂运动方案也能起主站。** 挂上才跑内置固定轨迹。要让应用自己给位置目标，就不挂，改用命令接口的 `set_external_target`（提交的是原始位置计数，不是角度）。
- **第 4 步只在主站没在跑的时候做。** `emaster-scan` 会驱动总线并把从站还原到 INIT；主站在跑时执行会打断它。
- **回零用 `--mode homing`。** 必须同时给 `--homing-timeout-ms`；`--homing-method`（0x6098）、`--homing-speed-switch`（0x6099:1）、`--homing-speed-zero`（0x6099:2）可省略，省略就保持驱动器里存的值。主站只把驱动器带进模式 6、置控制字 bit4、等状态字 bit12；全部轴到点后自动停机，bit13 或超时按故障停机。

各步的产出与失败点：

| 步 | 产出 | 失败看哪 |
| --- | --- | --- |
| 2 | `config/devices/`、`config/device_policies/` | 退出码非 0，并打印冲突的 PDO 集合。原因：ESI 里既没有默认分配（`#x1C12`/`#x1C13` 的 `DefaultData`），也没有 `Sm="2"`/`Sm="3"` 标记 |
| 4 | `scan.txt` | 报"一台从站都没扫到"。查网卡名写错、从站没上电、环没接好 |
| 5 | `config/topologies/`、`config/operation_profiles/`、`config/deployments/`、`config/motion_profiles/`（空目录） | 报"第 N 台不是 <设备档>"。总线上的型号和版本跟设备档对不上，点名到台。报"厂商没声明 DC"：ESI 里没有 `<Dc><OpMode>` 段，补 ESI，不要自己编同步参数 |
| 6 | 部署里的 `motion_profile_id` | 报"没有 config/motion_profiles/<ID>.json，部署挂不到它上面"。这份方案得先写出来 |
| 7 | `config-runtime/*.flat` | 校验失败会逐个指名字段。告警"运动方案 <ID> 没有被任何部署引用"表示它不会生效，照告警里那条命令挂上 |

`init-site` 的 `assign_activate` 取自 ESI，不猜。

第 2、5、6、7 步可以重复执行：

- 改了策略档，必须重跑第 2 步。只有它会合并策略档。
- 改了设备档或现场参数，必须重跑第 5 步。
- 改了运动方案，必须重跑第 6 步那两条。只写文件不挂到部署上，它不会生效。
- 任何配置改动，都必须重跑第 7 步并重启主站。

轴数来自部署档，硬件换算在主站启动时读回，恢复阈值来自错误恢复策略。

### 运行与停止

```bash
cd /path/to/ethercat-master
sudo bash -c 'ulimit -r unlimited; exec \
  ./build/apps/master/emaster-master --deployment <部署ID>'
```

`--deployment` 接收部署 JSON 里的 `deployment_id`，不是文件名。也可以直接给 `--config-dir config-runtime`，它是默认值。

观测通道随会话启动，不需要环境变量。

**确认主站起来了。** 不要只看进程在。

```bash
pgrep -a -f emaster-master
ls /tmp/emaster-<部署ID>*.sock          # 命令口和观测口，两个
sudo build/apps/status_client/emaster-status --deployment <部署ID>
```

`state=4` 是 RUNNING。

```bash
sudo timeout 3 nc -U /tmp/emaster-<部署ID>.sock <<< topology
# OK|axes=<轴数>,max_offset=<目标偏移上限计数>|a1:bus=<轴号>,enc=<增量数>/<电机转数>,gear=<M>/<S>,torque=0,ser=<序列号>|a2:...
```

`torque` 是保留字段，恒为 `0`。额定力矩在设备描述和配置里都没有数据源，不填猜测值。
`ser` 是主站实际读到的 `0x1018:04`；读不到印 `na`。

**停止：Ctrl+C。** 主站注册了 SIGINT 和 SIGTERM，两者走**和命令口 `shutdown` 完全相同**的安全停机路径：安全输出 → 停用各轴 → 回 INIT。

换部署前先经命令口发 `shutdown`，或向核实过的主站 PID 发 SIGTERM，等它退出并检查逐轴停用证据。**不要把 SIGKILL 当正常停机。**

网口、实时核、优先级和报告路径由部署决定。换主机时必须核对部署档。

### 配置模型

配置分三层：**设备描述 → 出厂设计 → 现场**。交付时 `esi/` 和 `config/` 下的六族都是空的。

| 目录 | 谁写 | 内容 |
| --- | --- | --- |
| `esi/<你的型号>/` | 你放进去 | 厂商的 ESI 原文件（XML）。设备档的权威来源 |
| `device_policies/` | `add-device` 出草稿，人改 | 人写的那一半：停机掩码、PDO 组选择、支持的模式集合 |
| `devices/` | `add-device` 合成 | 设备档：身份、PDO 映射、`slow_telemetry` 清单、状态字掩码 |
| `topologies/` | `init-site` | 拓扑：环上有哪些从站、顺序、每台的身份期望与位置 |
| `operation_profiles/` | `init-site` | 运行方案：周期、DC 策略（`dc`/`sm`）、SM2/SM3 同步类型、相位与偏移 |
| `motion_profiles/` | 你写 | 运动方案：跟随误差限幅、轨迹、每轴的换算 |
| `deployments/` | `init-site` | 部署：主机名、网口、轴数、引用哪套拓扑／运行方案／运动方案、报告路径 |
| `error_recovery_policies/` | 随产品交付 | 容错阈值。与电机无关 |
| `messages/zh_CN.json` | 随产品交付 | 中文文案 |

**换一款电机要动的只有前七行**，那是跑命令，不是改代码。

每族字段的完整语义见 `config/README.md`；设备上手工具的边界见 `tools/gen/device/README.md`。

**换电机、换拓扑、换部署都不需要重编主站。** 六族的条目全部在运行期从 `config-runtime/` 的扁平文件读。改 `error_recovery_policies/` 才需要重编，那一族是编译期静态表。

只改 JSON 而运行旧二进制，不能保证生效。任何配置改动都要重跑 `build` 并重启主站。

**配置指纹**：主站启动时按四个 `config-runtime/*.flat` 文件的内容算一个指纹，打在启动行下面；报告的同名字段是 `deployment.config_fingerprint`（十六进制字符串）。四份文件改一个字节，指纹就变。用它对报告和终端，能确认"这份报告是哪份配置跑出来的"。它只做身份标识，不是权限校验；改配置仍须重生成并重启。

### 单位与换算

主站启动时逐轴读回 `608F`/`6091`，同时取设备档 `conversion` 里声明的同一组值。声明三项齐全就用声明，缺项才用读回值；两者都没有、或换算出不来，该轴不参与运动。自报值与声明值都写进报告与终端，不一致时标出。**不能把某个拓扑的比例用于全部设备。** `topology` 命令返回运行时换算。

```text
电机每转计数 = encoder_increments / encoder_motor_revolutions
输出轴每转计数 = 电机每转计数 × gear_motor_revolutions / gear_shaft_revolutions
相对位移计数 = 输出轴角度 / 360 × 输出轴每转计数
绝对位置目标 = 确认的起点位置计数 + 相对位移计数
```

例：编码器 4096 counts/电机转、齿轮比 10:1，对应 40960 counts/输出轴转，约 113.78 counts/度。

还须核对正方向、机械零点和整数舍入。**原始位置零点不是自动校准出的机械零点。**

主站不限制目标之间的增量，也不限制速度。步长和速度由客户端决定。

**主站只留一条判据：`|目标 - 当前实际位置|` 超过上限就拒收这一条**，且只对位置轴生效。上限由部署的 `external_target_max_offset_millidegrees` 决定，默认 90°；取值看 `topology` 回报的 `max_offset`（各轴的最小值）。**拒收不中止会话**：该条目标不采纳，上一目标继续保持，其余轴照常跑。轴号与偏移量写进报告与 `status`。

超过 `external_target_timeout_ms` 没收到新目标时：位置轴返回 `HOLD`，保留最后目标；速度与力矩轴把目标写 0。写什么由运行方案为模式声明的量（`quantity`）决定，不按模式号。它不等于驱动器失能。谁给目标、多快给、断供怎么办、允许哪些模式、零点在哪，见[目标通道契约](docs/protocol/target-channel-contract.md)。

算法见 `src/motion/position_target.c`，SDO 读回与配置见 `src/bus/session/session_setup.c`。

### 观测与界面

观测口是独立的只读 unix 套接字 `/tmp/emaster-<部署ID>-obs.sock`，随会话启动。

**主站认的环境变量只有这 8 个。**

| 变量 | 作用 |
| --- | --- |
| `EMASTER_SETPOINT` | 建不建设点口。默认关 |
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

读哪些对象由设备档的 `slow_telemetry` 决定。

**跨机图形界面**

主站和桥跑在目标主机，界面跑在另一台机器上。

```bash
# 目标主机
sudo python3 tools/socket_bridge.py --deployment <部署ID> --bind 0.0.0.0 --port 5001
```

```powershell
# 另一台机器（克隆目录）
cd <克隆目录>
$env:PYTHONPATH='tools'
python -m emaster_gui --tcp <host>:5001
```

- 端口约定：命令 5001、观测 5002。界面自动用 PORT+1，不用另给。
- `--tcp` 同时开了命令通道，也就是界面能驱动电机。只读改用 `--obs-endpoint tcp:<host>:5002`。
- 桥会绑到主站实时核之外的核。
- 桥监听 `0.0.0.0` 时会打一句警告：**这两个口没有任何认证，能连上命令口的人就能驱动电机。**

**终端面板（同机）**

```bash
sudo bash -c 'ulimit -r unlimited; exec \
  python3 tools/emaster_console.py --repo "$PWD" --deployment <部署ID> --start'
```

`--range` 是自助软范围（默认 45°），`--jog-speed` 是点动速度（默认 10 °/s），`--start` 表示套接字不在时替你拉起主站。终端面板与图形界面共用同一份控制律。

### 故障排查

| 现象 | 先查什么 |
| --- | --- |
| 主站起不来 | `ulimit -r` 是不是 `unlimited`。部署声明了 `realtime.required=true` 时，拿不到 `SCHED_FIFO` 主站会拒绝进入 OP |
| 进不了 OP | 用 `status` 看主站状态，再看逐轴 AL 状态。拓扑或身份不匹配会拒绝建立周期控制 |
| 轴不动 | 见下面两张表 |
| 命令口连不上 | 主站刚启动的几十秒内套接字可能还没建好。客户端退避重试，不要快节奏重连 |
| 报告里温度／电压／电流恒为 `—` | 设备档的 `slow_telemetry` 有没有声明这些对象；MT 没有 0x6078，电流显示 `na` 是正常的 |
| 报告里字段是 `-` | 取错了层。见[测试方案](docs/test-plan.md)的"报告字段层级"一节 |
| 报告里的 `error_class` 编号不知道查什么 | 编号、含义与下一步见[错误分类](docs/product/error-codes.md)；报告同名字段已直接带下一步 |
| 普通账号打不开报告目录 | 旧版本把目录建成 `0750`。执行一次 `sudo chmod -R a+rX runtime`；新版本建 `0755` |
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

## 命令表

### 命令口

命令口是 AF_UNIX 流套接字，路径 `/tmp/emaster-<部署ID>.sock`。它按行分帧：命令以换行符结尾，主站把读进来的字节攒着按行切。一次发送可以带多条命令，一条命令也可以分几次发送——两种都按条处理，每条回一行。没有换行符的字节一直攒着，攒满 511 字节还没有换行就整块丢弃并打一条警告。

**权限警告：`src/bus/io/command_server.c` 对套接字执行 `chmod 0666`，而且没有应用层认证。任何本机用户都能下发运动和停机命令。** 跨机桥 `tools/socket_bridge.py` 同样没有认证。只能在可信、隔离网络、单用户测试环境使用。

| 命令 | 用途与约束 |
| --- | --- |
| `status` | 查询主站及逐轴状态。主站 `state=4` 和 AL SAFE-OP 的数值 4 属于不同枚举 |
| `topology` | 查询实际轴集合、位置换算和轴顺序。发送目标前先查询 |
| `dc` | 查询同步信息 |
| `set_external_target <p1> <p2> ...` | 按拓扑顺序提交**全轴**绝对原始位置计数。不是角度，不接受部分轴集合 |
| `switch <motion_profile_id>` | 切换运动方案。它不等于切换 CSP/CSV/CST 控制模式 |
| `stop` | 逐轴置 Halt（控制字 bit8）。驱动器减速并保持使能，会话不退出；`halt 0` 恢复跟随上一目标。不等于断电 |
| `halt <0\|1>` | 逐轴清 / 置 Halt。与 `stop` 同一机制 |
| `shutdown` | 请求完整停机并退出 |
| `quick_stop`、`fault_reset` | 解析器已有入口。入口存在不代表相关安全功能已经验收 |
| `recover_axis <轴号>` | 恢复一台处于故障隔离的轴。解析入口是后补上的，**未经台架验证** |
| `sdo_read <从站号> <索引> <子索引>` | 读任意对象字典条目。从站号从 1 起，索引与子索引接受 `0x` 前缀。由观测线程执行，最多等 5 秒；成功给 `raw=` 与 `u=`/`i=`，失败明说原因，不把 0 当读数 |
| `al` | 在线读每台配置从站的 AL 状态（0x0130）与 AL 状态码（0x0134）。由观测线程按映射后的总线位置执行，最多等 5 秒；读不到的项印 `na`。`status` 的逐轴字段全部来自过程数据，回答不了"是否掉出 OP"，用这条 |

发命令请复用 `tools/interactive_control.py`、`tools/emaster_client.py` 或控制台 Engine。**每条命令回一行**，但一行回复也可能被拆成多次到达：按换行符读到完整一行再判定，不要假定一次 socket read 就是一条完整回复。

**没登记序列号时轴序就是总线顺序；登记了序列号时按序列号绑定。** `init-site --from-scan` 把扫描读到的 `0x1018:04` 逐台写进拓扑。登记后，同型号从站换了环序也照样对得上；登记了但读不到、或读到的值不相等，主站拒绝启动并点名到轴。没登记序列号的拓扑保持原行为：按 vendor/product/revision 取首个未分配的从站。`topology` 的回复带 `ser=`，是主站实际读到的序列号，可直接核对。

**回零由执行层带模式、由驱动器走。** 用 `--mode homing` 生成的部署，各轴没有周期目标。主站把驱动器带进模式 6，在 Operation enabled 下置控制字 bit4，等状态字 bit12（homing attained）；bit13（homing error）或超过 `completion_timeout_ms` 按故障停机，全部轴到点后自动停机。回零方法与速度用 `--homing-*` 声明（0x6098 与 0x6099，写进 `safeop_to_op_sdo_writes`），不给就保持驱动器里的值。参考点准不准要在台架上核对。**回零尚未在台架验证。** 其他模式不做回零，位置基准由驱动器零点与机械决定。已验证的组合见[驱动与模式组合](docs/product/verified-combinations.md)。

外部目标超时由 `external_target_timeout_ms` 指定，默认 200 ms。客户端 100 Hz 的目标更新率与主站 1 kHz 的 PDO 交换率不同。断连、目标暂停、重连必须分别测试。**不能假定自动恢复运动。**

**设点通道（可选，默认关）**

外部控制器高频送目标可以走第三个套接字：`/tmp/emaster-<部署ID>-sp.sock`，AF_UNIX 加 `SOCK_SEQPACKET`，**只写不回**。要 `EMASTER_SETPOINT=1` 才建。

它和 `set_external_target` 写的是同一个目标汇点：覆盖式最新值。位置是原始计数，与观测通道同口径。

- 客户端连上就发，主站连上就收，**没有就绪握手**。挡住"连上之前发了一个离谱的值"的是目标判据，它从第一拍起就生效。先等 `status` 报 `state=4` 再起发送端。
- **只有 `mode` 与 `q` 进输出路径。** 其余四个量（`dq`、`tau`、`kp`、`kd`）会解码、校验、计数，但今天不用。
- 记录格式与四种拒收原因见[接口协议](docs/protocol/command-protocol-v1.md)。拒收是局部的：只丢这一条，会话继续，`status` 的 `rejected=` 加一。
- 发送端参考实现是 `tools/setpoint_send.py`，它的 `--dry-run` 不连主站，只打印会发出去的记录。**这条通道同样没有认证。** `tools/socket_bridge.py` 不转发设点口。

**两层状态不能混。**

- **不能用 AL 状态推断驱动器使不使能。** 两个都要读。
- **不能用过程数据去送"停止周期通道"的命令。** 从站掉出 OP 后控制字到不了它。**禁用必须走邮箱。**
- AL 在 INIT 时读不到 `0x6041`，因为邮箱关闭了。这时只能说"读不到"。
- `0x0134` 只在 `0x0130` 的 bit4（Error Indicator）置起时才有意义。

机制与完整论证见[实现说明](docs/architecture.md)第 6 节。

### 工具

都在 `build/apps/<目录>/` 下。

| 工具 | 说明 |
| --- | --- |
| `emaster-master` | 主站 |
| `emaster-scan` | 只读扫描：逐台身份、序列号、AL 状态、`0x6041`、故障码、换算。用法一节第 4 步用它 |
| `emaster-disable` | **走邮箱把驱动器失能**。主站起不来时也能用 |
| `emaster-status` | 只读查询命令口一次：`status`。`--watch <毫秒>` 周期刷新；`--al` 读一次逐台 AL 状态与状态码 |
| `emaster-watch [间隔ms] --deployment <ID>` | 实时监控位置／速度／力矩／状态字 |
| `emaster-move-deg --deployment <ID>` | 交互式角度控制，自动换算 |
| `emaster-move --deployment <ID>` | 交互式脉冲控制，底层接口 |
| `emaster-fingerprint` | 硬件指纹。最高只到 PRE-OP |
| `emaster-dc-prepare` | PRE-OP DC 准备。不改状态 |
| `emaster-target-probe` | 目标源探针 |

**每个工具都认 `--help` 与 `--version`。** 两者都打到 stdout，都以 0 退出。不带参数时出帮助并退出 0。

以下工具的默认流程不是帮助，而是一个真实动作，所以不带参数时不打印帮助：`emaster-master` 和 `emaster-target-probe` 按当前主机名匹配部署并起会话／跑探针；`emaster-status`、`emaster-watch`、`emaster-move`、`emaster-move-deg` 连默认部署的命令口。

`tools/` 下是 Python 工具：`emaster_client.py`（命令口客户端）、`interactive_control.py`、`emaster_console.py`（终端面板引擎）、`emaster_gui/`（图形界面）、`observation_client.py`（观测口）、`closed_loop_client.py`（闭环样板：观测→策略→命令）、`setpoint_send.py`（设点口发送端）、`socket_bridge.py`（跨机桥）。另四个是它们共用的支撑与自测：`emaster_endpoint.py`（主站端点）、`rt_affinity.py`（躲开实时核）、`gui_evidence.py`（判界面自检日志）、`test_client_reconnect.py`（重连逻辑的离线自测）。

**`emaster-scan` 会把驱动器拉出 OP 再恢复**，因为它要发 INIT→PRE-OP 请求。**运行中不要用它。**

**`emaster-disable` 会让电机立刻失去力矩保持。** 带重力负载、或抱闸未抱时不要跑。

## 安全边界

**本软件不是经过认证的功能安全系统。** 电气 STO、急停、机械约束、断电措施和安全测试区域仍是外部强制要求。

- **指纹工具和 PRE-OP DC 准备工具的最高 AL 状态仍为 PRE-OP。** 它们分别只执行受限的只读采集或参数准备。
- **`emaster-master` 不在 PRE-OP 指纹流程范围内。** 它会请求 SAFE-OP/OP，并把驱动器推进到运行使能。
- 进入 CSP 前，以 `6064` 实际位置初始化 `607A`。
- 停止和失败路径按状态反馈**逐级撤销使能**，并以设备配置中的 `6041` 掩码和值确认停用，随后关闭 Sync0 并恢复 INIT。设备档没填掩码时只有标准 CiA 402 判据可用；两条判据都没命中时，报告把**轴已离开运行使能**记成"未确认"，**仍有轴在运行使能**记成失败，两者不合并。
- **获得 PRE-OP 指纹运行许可，不代表获得 SAFE-OP、OP、CiA 402 使能或运动授权。** 每个更高的边界都需要独立流程和批准。

**每份硬件测试记录必须说明**：操作者、观察者、物理装置、功率限制、机械约束、STO/急停验证、网络接口、软件版本、配置哈希、ESI 哈希和全部从站序列号。

## 已知限制

下表是当前代码可直接确认的限制。它们属于使用边界。

| 限制 | 证据与影响 |
| --- | --- |
| 命令口权限过宽 | `src/bus/io/command_server.c` 将套接字 `chmod 0666`，无认证。本机任意用户都能控制运动、故障复位或停机 |
| 命令口超长行整块丢弃 | `command_server.c` 攒到 511 字节还没有换行时丢掉整块并打一条警告。丢弃的是那一行，连接不断——但是这一行里的命令不会被执行 |
| 断连后保持位置（已定的产品行为） | 目标超过 `external_target_timeout_ms`（默认 200 ms）没更新时，CSP 保持最后目标：不是撤使能，也不是受控减速。若将来要求"通信丢失即停机"，那是行为变更，不是缺陷修复。见[目标通道契约](docs/protocol/target-channel-contract.md)第 4 节 |
| 偏移上限折算不出来的轴不做目标判据 | 换算不可用的轴根本不让跑（会话判 `MOTION_INVALID`）。但换算可用、而上限折算结果为零（部署把 `external_target_max_offset_millidegrees` 配得过小）或溢出的轴会跑且不判；报告里 `offset_limit_checked=false` 如实标出 |
| 未登记序列号时，同型号从站不能唯一确定物理轴 | 拓扑里每台从站可登记 `serial_number`（扫描读 `0x1018:04`）。**没登记**的轴仍按 vendor/product/revision 选首个未分配项，**环序不等于物理关节标签**。登记序列号的映射路径尚未经台架验证 |
| 旧配置目录要与新版主站一起重建 | 扁平文件新增 `topology.<n>.slaves.<m>.serial_number`。缺这个键时主站报"缺键"并退出；对配置源跑一次 `emaster_config.py build` 即可 |
| DC/Sync0 可靠性是模型估计 | `sync0_margin_ns` 使用参考 DC 时间、启动期 `pdelay` 和配置的 Sync0 相位，没有直接采集 Sync0 边沿或 SM2 完成时间。非负裕量不能证明目标已在正确的 Sync0 前由每个驱动器锁存 |
| 目标判据不是速度限制 | `session_target.c` 只查 `|目标 - 实际位置|`，且只对位置轴查。客户端连发两条各自合法的目标，两条之间的速度可以任意大，主站不看这个 |
| 断供写 0 不等于受控停止 | 速度与力矩模式超时后主站写 0，但减速由驱动器自己的斜坡完成，主站不等它停；力矩模式下 0 力矩就是轴松开。谁写什么由运行方案声明的量（`quantity`）决定，见[目标通道契约](docs/protocol/target-channel-contract.md)第 4 节 |

**不作为当前已支持能力承诺**：热插拔、动态轴数、多主站协同、功能安全。

代码内部的风险不在本表，见[实现说明](docs/architecture.md)第 14 节。

## 维护者

问题与请求走 [GitHub Issues](https://github.com/Lossop1/ethercat-master/issues)。

## 贡献

见 `CONTRIBUTING.md`。

## 许可

GPL-3.0，全文见 [`LICENSE`](LICENSE)。

树内 `external/SOEM/` 是第三方源码，另按 GPLv3 与商业双重授权，见 `external/SOEM/LICENSE.md`。整包分发时 GPLv3 适用。要把本产品用在闭源产品里，须先向 RT-Labs 取得 SOEM 商业授权。

---

## 附：文档版本

本文按仓库中的当前实现说明行为。部署时应使用与源码匹配的构建产物；配置值本身不构成厂商保证。
