# 能力基线调查（2026-09-11）

## 目的与证据等级

本文回答三个问题：行业怎么分层、驱动器到底支持什么、项目实际到了哪一步。
分层计划见 `layering-plan.md`。

证据等级：

- **verified** — 有本仓库或子模块的 file:line、或真机 fingerprint/report 数据支撑
- **documented** — 有厂商手册/ESI 章节支撑
- **inferred** — 由多方一致性推导，无单一权威出处

调查期间外网全部不可达（WebSearch 无结果、WebFetch 域名校验失败），因此 IgH / acontis /
TwinCAT / ros2_control / ETG.1500 相关条目均为 documented 或 inferred，未经抓取验证。

## 1 行业标准做法

### 1.1 主站栈与运动层是两层，跨厂商一致

这是本次调查最硬的一条结论，五个独立来源指向同一边界：

| 来源 | 总线层 | 运动层 | 证据 |
| --- | --- | --- | --- |
| SOEM 2.0.0 | 全部 | 无 | 代码内 CiA402 语义零命中（statusword/controlword/402/scaling/interpolation 全部 0 hit）；`6040` 的 21 次命中是 SDO abort code，不是控制字 |
| acontis | EC-Master | EC-Motion（**独立产品**） | 商业产品线划分 |
| Beckhoff | TcIo（现场总线、过程映像、DC、ESM） | TcNc（轴、单位换算、软限位、插补、跟随误差窗口） | 两个独立运行时组件，ADS 为非实时参数通道 |
| IgH EtherLab | master/domain/slave_config | 无（Simulink 工具链另计） | ecrt.h 只有 IO 级抽象 |
| ros2_control | ethercat_driver_ros2 核心 | `ethercat_generic_cia402_drive` **插件** | CiA402 状态机在插件层，不在驱动核心 |

**没有"EtherCAT 主站顺便做运动控制"这种做法。** 本项目当前把 CiA402 状态机放在
`src/cia402/`、把单位换算放在工具层，方向与行业一致，但边界尚未强制（见 3.4）。

### 1.2 "能力"在工业上有三种并存表达，不是一张功能表

- **协议/特性位图**（栈级）：ETG.1500 Master Classification Class A/B 特性表；
  每从站的邮箱协议位掩码。SOEM 中即 `mbx_proto`（AOE..VOE）、`CoEdetails`、
  `slave->hasdc`（ec_main.h:94-99, 101-106, 188-189，verified）
- **数据点清单**（周期数据级）：PDO entry 列表 + 位宽 + 偏移；ros2_control 的
  `state_interface`/`command_interface` 命名清单（`HW_IF_POSITION` 等标准字符串）
- **时序契约**（接口级）：哪些函数可在实时线程调用。通常只是文档约定。
  本项目已有实例：`include/emaster/cyclic/exchange.h:22-23` 明文禁止回调执行
  SDO、状态扫描、动态分配、文件或日志操作（verified）

底层能力清单应当是"数据点 + 协议位图 + 时序契约"三件，而不是一张功能表。

### 1.3 实时与非实时靠"两种寻址方式"分开，不是靠加锁

- 周期数据走过程映像：指针 + 偏移，无分配、无阻塞
- 参数走邮箱：带 timeout、异步请求 + 轮询、或按 ESM 转换的启动列表

各家实现：IgH 用 `slave_config_sdo`（配置期）+ `sdo_request` 异步轮询（运行期）两条路；
SOEM 2.0.0 用 `ecx_mbxhandler` 在实时线程内做**有界**邮箱推进；TwinCAT 用 ADS 做运行期
非实时访问。

### 1.4 时序三级划分

| 级别 | 内容 | SOEM 官方示例对应 |
| --- | --- | --- |
| 周期内 | 过程数据、DC | `ecatthread` + `ecx_send/receive_processdata`（ec_sample.c:61-100, 85, 97） |
| 跨若干周期 | 邮箱轮询、状态监督、从站恢复 | `ecx_mbxhandler(&ctx, 0, 4)` 在 RT 线程内，limit=4（ec_sample.c:96）；`ecatcheck` 线程 10ms（103-182, 180） |
| 非实时 | 参数配置、总线扫描、EEPROM、FoE | `ecx_SDOread` 在**另一线程** 20ms，注释明写"Demonstrate SDO access from other threads"（ec_sample.c:281, 287, 291） |

"跨若干周期"这一级缺乏统一行业术语，三级划分本身是 inferred。

### 1.5 通用性靠四件事，不靠 if-else 分支

1. 离线机器可读配置产物（ESI/ENI）承载全部设备与拓扑差异，栈本身是通用引擎。
   SOEM 2.0.0 已支持 ENI（`ec_enit`/`ec_enislavet`/`ec_enicoecmdt`，
   `ecx_mbxENIinitcmds`，ec_main.h:360-397, 598，verified）
2. 过程映像用字节 + 映射表，不用按机型定义的 struct
3. 能力从设备描述读出并在启动时核对，不编译期硬编码
4. 上层通过回调注入，下层只持不透明指针。SOEM 的 `userdata` 明文声明库自身永不触碰
   （ec_main.h，verified）；`islost` 由应用负责设置而非库本身（ec_main.h:246-247，verified）

### 1.6 职责归属的行业惯例

- **单位换算不在总线层。** 换算因子是设备侧对象（608F 编码器分辨率、6076 额定转矩），
  必须运行时读回：本项目真机 `608F:01=16384`、`6091:01=28` 与 EDS 默认值不同（verified）。
  总线层只搬运原始 counts
- **安全分三处**：功能安全在软件之外（STO/急停/机械约束）；驱动内部限值在驱动对象；
  运动包线在运动层。主站只负责执行条件门禁
- **状态机分两个**：ESM 归主站（SOEM `ecx_readstate`/`writestate`/`statecheck`）；
  CiA402 归驱动/轴层

## 2 当前驱动器能力边界（良质 ISVD90RC，CiA402）

### 2.1 三个能力来源互相矛盾，以 ESI 为准

| 来源 | 内容 | 可信度 |
| --- | --- | --- |
| `docs/lz-joint/ECAT_CIA402.xml` | 90 个顶层对象 | **权威**（EtherCAT 固件真实描述） |
| EtherCAT PDF 手册 §5.8 | ESI 的一个小子集 | 不完整 |
| CANopen 手册 / EDS | 4×RPDO + 4×TPDO 自由重映射、60F6 | **不适用**：这些在 EtherCAT ESI 中不存在，是 CAN 版本的能力 |

不要用 CANopen 手册推断 EtherCAT 能力。

### 2.2 最硬的约束：PDO 内容不可改

`ECAT_CIA402.xml:6705`：

```xml
<CoE SdoInfo="true" PdoAssign="true" PdoConfig="false" CompleteAccess="true" SegmentedSdo="true"/>
```

`PdoConfig="false"` + 所有 Module 的 `RxPdo Fixed="true"`/`TxPdo Fixed="true"` + 手册明文
"用户不可更改 PDO 映射" ⇒ **可以选择用哪套 PDO 方案，不能改任何一套方案的内容。**

直接后果：**6078（实际电流）、6079（母线电压）、200B（MOSFET/电机温度、母线电流）、
60F4（跟随误差）无法放进任何 PDO**，尽管 ESI 把 6078/6079/60F4/6062/60FC/6069/606B/6074
标记为 TPDO-mappable —— 标记为可映射不等于本设备允许重映射。

三套可选方案（`config/devices/cyberbeast_isvd90rc_300b_100_70.json`）：

| module_ident | 名称 | 大小 | 含 6060/6061 | 用途 |
| --- | --- | --- | --- | --- |
| 0x00119800 | dynamic_csp_csv_cst | 14B rx / 14B tx | 是 | **唯一支持周期内模式切换** |
| 0x00219800 | fixed_csp | 8B | 否 | CSP 固定 |
| 0x00319800 | fixed_csv | 8B | 否 | CSV 固定 |

TxPDO 1A00h 真机实测（`records/fingerprints/orangepi-bench-20260901T034149Z.json`）：
status_word / actual_position / actual_velocity / actual_torque / mode_display + 8bit padding。

**ESI 与手册在 0x1A02 上冲突**：ESI 映射 6041+6064，手册 §5.7.1.1 写 6041+606C。
`fixed_csv` 方案是否真能拿到速度反馈**未经真机验证**，用之前必须实测。

### 2.3 电流/电压/温度只能走 SDO，代价已量化

无 PDO 可用 ⇒ 只能邮箱 SDO ⇒ 非周期、有抖动。SOEM 2.0 cyclic 邮箱模式下单次 SDO 往返：

- `ecx_mbxsend` 入队，RT 线程内 `mbxouthandler` 做 FPWR：1 周期
- 从站响应后，下一个 PD 帧携带 mbx-full 标志，`mbxinhandler` 发 FPRD 取回：再 1 周期
- 请求线程以 `EC_LOCALDELAY=200us` 粒度轮询

⇒ **最少约 3 个周期 + 200us 轮询抖动**（1ms 周期下约 3ms）。

**真正的 RT 成本不在这里**：`mbxinhandler` 里每次 `ecx_FPRD` 都是一次**独立的阻塞帧往返**
（`EC_TIMEOUTRET=2000us`），跑在 RT 线程内部。`limit=4` 意味着每周期最多 4 次阻塞往返。
满尺寸 1486B 邮箱单向帧时约 121us，往返约 250us ⇒ limit=4 可能吃掉 500us 以上。
CiA402 驱动器邮箱通常 128-256B，往返约 25-45us，但**本驱动的 mbx_rl 尚未记录在
fingerprint 中**，精确估算缺这一个数。

结论：电流/电压/温度属于**慢速监控通道**（10-100ms 级、轮询式、错开轴号），不属于周期观测量。
这是硬件约束决定的，不是设计选择。

### 2.4 MIT_Control 无 CoE 等价物 —— 真实能力缺口

CAN 版本的 `MIT_Control`（阻抗控制：kp/kd + 前馈力矩）在 EtherCAT ESI 中**没有对应对象**。
这不是"换个索引就行"的问题，是能力缺失。

后果：如果上层需要关节阻抗/PD 控制，**PD 环必须放在主站侧按周期运行**，输出折算为
CST 力矩或 CSP 位置。这直接决定了分层：主站必须有一个能承载周期内控制律的位置。

### 2.5 模式与方案切换的代价不同

- **模式切换（CSP/CSV/CST）**：0x119800 方案把 6060/6061 放进 PDO ⇒ 可在 OP 态周期内切换。
  `6502=896`（bit 7,8,9）确认三种模式均支持（verified，真机读回）
- **PDO 方案切换**：必须回 PRE-OP（写 F030 → 清 1C12:00/1C13:00 → 写映射 → 写计数），
  过程映像大小 14↔8 字节变化 ⇒ FMMU/SM 必须重配，且**不写入 EEPROM**

### 2.6 时序数据的真实边界

- `1C32:05 = 31200ns` 是**协议下限，不是可用工作点**
- 厂商实验室数据：单从站 41.1us，五从站 46.5us。**无 12 轴厂商数据**
- `supported_sync_types = 16415 = 0x401F`（bit 0,1,2,3,4,14）vs ESI 默认 `0x8007`
  ⇒ **真机固件比 ESI 新**，以真机读回为准

## 3 SOEM 2.0.0 侧能力与陷阱

子模块确实是 v2.0.0。`git submodule status` 显示的 `v1.3.1-293-g304d1c0` 是过期的 describe
缓存，不是真实版本。

**项目只用了 92 个 `ecx_` 符号中的 16 个。** 未用的关键能力：

### 3.1 可用但未启用

| 能力 | API | 价值 |
| --- | --- | --- |
| 周期内有界邮箱 | `ecx_mbxhandler(ctx, group, limit)` | 让 SDO 与周期共存，见 2.3 |
| 从站恢复/重配 | `ecx_recover_slave` / `ecx_reconfig_slave` | 掉线恢复 |
| 网线冗余 | `ecx_init_redundant` | 双网口冗余 |
| ENI 配置 | `ecx_mbxENIinitcmds` | 离线配置驱动通用引擎 |
| 多组 | `slave->group`（`EC_MAXGROUP=2`） | 不同周期率分组 |
| 裸数据报 | `ecx_APRD/FPRD/BRD/ARMW/FRMW/LRW/LRWDC` 全部导出且**SOEM 内部无使用者** | 应用可自建数据报调度 |
| 错误计数器 | `ECT_REG_RXERR/FRXERR/EPUECNT/PECNT`（0x0300-0x030F） | **SOEM 只在初始化时清零，从不读回**，必须应用自己 FPRD |

### 3.2 线程安全性（关键前提）

port 层用三个互斥量保护 `getindex`/`tx`/`rx`；rx 路径把别的线程的帧按 index 存入对应缓冲。
⇒ **非 RT 线程调用阻塞 SDO 与 RT 周期线程可以安全并发**。这是 SOEM 2.0 的设计模式，
官方示例即如此（ec_sample.c:281-291）。

无非阻塞 SDO API（不同于 IgH 的 `sdo_request` 异步轮询），但"另一线程阻塞调用"等效可用。

### 3.3 SOEM 自身的两个 bug 与两个坑

- **bug**：`ec_main.c:1681` `EMp = (ec_emcyt *)mbx;` 与 1698 行 `(ec_EOEt *)mbx` 误用双重指针
  `mbx` 而非 `mbxin`，读到的是调用方栈上指针变量的地址。⇒ **非 cyclic 路径的
  emergency/EoE 检测读的是垃圾数据。** cyclic 路径（`ecx_mbxinhandler:1306`）正确
- **坑**：`ecx_slavembxcyclic` 只启用 `coembxin`。⇒ **cyclic 模式下 FoE/SoE/VoE/AoE 入站
  被丢弃**（`foembxoverrun++`），除非应用手动预置 `slave->foembxin = EC_MBXINENABLE`。
  存在鸡生蛋问题：1632 行的重新使能只在 `foembxinfull` 为真时才到达
- **坑**：错误环形缓冲（`EC_MAXELIST=64`）**无互斥量**。`pusherror` 在 RT 线程、
  `poperror` 在非 RT 线程 ⇒ 真实竞态
- **坑**：Sync1 脉冲延时在 `ecx_configdc` 里**硬编码 100ms**

### 3.4 DC 时间戳的真实语义

`ecx_send_processdata_group` 在**第一个 PD 帧**插入 FRMW 数据报读取 DC 参考从站的
`DCSYSTIME`，更新 `context->DCtime`。含义是"帧经过参考从站那一刻参考钟的系统时间"。

- 这是**每帧的参考钟时间戳 + 周期身份**，不是每从站的输入锁存时刻
- 真实采样时刻由 Sync0 决定（若从站在 Sync0 锁存输入），是设备相关行为
- 漂移补偿由 FRMW 每周期自动完成（各从站 DC 单元自行调速），不需要额外调用
- **无 API 读每从站漂移寄存器 0x092C，也无 API 读 DC 锁存寄存器** ⇒ 需要裸 FPRD

⇒ 如果上层需要"这批观测量是同一时刻的"，DC 提供的是统一时基与周期编号，
逐从站采样时刻需要自己用裸寄存器读取，或者接受"同一 Sync0 边界"这个假设。

### 3.5 其它已确认约束

- `EC_MAXFMMU=4`，且**邮箱状态每从站占 1 个 FMMU**（SOEM 2.0 自动把 mbx-status FMMU
  映射进 IOmap，这是 cyclic 邮箱能判断状态而不额外发 FPRD 的原理）
- WKC 计算：LRW 输出计两次；邮箱从站无输入时输入 WKC 额外 +1
- **无在线重扫描** —— 从站数量变化只能在 config_init 期发现
- 状态读取有 BRD 快路径，回落到批量 FPRD（每帧最多 64 从站）；状态写入与检查是
  1ms 轮询阻塞

## 4 项目实际进度（以代码与真机报告为准，不采信文档自述）

### 4.1 安全相关缺口（最高优先级）

| 缺口 | 证据 | 后果 |
| --- | --- | --- |
| **生产主站跟随误差保护未启用** | `position_target_max_following_error_counts` 出现在 5 个文件，**`tools/master/main.c` 不在其中**（只有 `tools/target_probe/main.c` 设置了它）⇒ 值为 0 ⇒ 保护关闭 | 失控不被拦截 |
| **无失效命令超时** | `external_targets_available` 一旦置位永久有效，无时间戳、无看门狗 | **客户端断开后主站继续驱动到最后一个目标** |
| **Fault Reset 未接线** | `emaster_cia402_controller_request_fault_reset` 只出现在 `src/cia402/controller.c` 与其头文件，**零调用者** | 故障后无法恢复，只能重启 |
| **Quick Stop / Halt 完全缺失** | 控制字只有 0x0000/0x0006/0x0007/0x000F/0x0080 | 无受控急停 |
| 运行期 603F 不读 | `session_observer.c:22` 只在 SAFE-OP 初始化与关机时读 | 运行期 error_code 恒为 0 |
| 死区超时永久锁存 | `cycle_clock.c:147-151` 首次发生即锁存；`deadline_recovery` 配置项无消费者 | 单次抖动导致永久降级 |
| 外部目标无插值无限幅 | 无双缓冲、无无锁队列 | 上层跳变直接下发 |

### 4.2 实时性：代码里没有任何实时原语

`mlockall` / `sched_setscheduler` / `SCHED_FIFO` / `pthread_setaffinity` / `CPU_SET`
在 `src`、`tools`、`include` 全树**零命中**（已复核）。

⇒ `docs/realtime/baseline.md` 的主机要求在代码中**完全未实现**。
⇒ **已有的全部时序数据都是在无 RT 保障的条件下测得的**，`send_lateness` 的
475.7us 峰值可能只是调度干扰，不能作为总线能力结论。

### 4.3 时序数据的可信度问题

- **最长运行记录约 14.9 秒**（14897 周期）。文件名 `stability-test-latest.json` 有误导性。
  双轴约 7.7 秒。`scripts/test_continuous_motion.sh` 只跑 60 秒 ⇒ 距 8 小时要求极远
- **只保留 min/max 与计数，无百分位、无直方图** ⇒ baseline.md 的直方图要求未满足，
  p99.9/p99.999 无法比较
- `send_lateness_ns`（最接近唤醒延迟的代理量）稳定性测试 78.5-116.5us，双轴达 **475.7us**
  ⇒ 超过 baseline 的 100us 上限
- **仓库内全部 sync0_margin 数据都早于修复 `6c4ea9d`**。负最小值（-161913、-749956、
  -749584、-209006）与 sync0_late 计数是 commit message 自己承认的回绕假象
  ⇒ **记忆中"180us Sync0 裕量"这个数字没有任何修复后报告支撑**（最接近的修复前值为
  136/143/154/159us）。该数字在拿到修复后数据前不可引用
- 单轴→双轴往返增长仅约 1.2-1.6us/轴，低于 3-5us 理论估计；**无 3 轴及以上数据**

### 4.4 分层违规与实现状态

- **`src/bus/soem/session_control.c:552-555` 用 `extern` 引用定义在
  `tools/master/main.c` 的全局变量**（`external_targets_available`、
  `external_target_positions`、`external_axis_count`、`external_targets_mutex`）
  ⇒ 下层依赖上层符号。`target_probe` 必须复制一份定义才能链接。已复核
- **编码发生在发送之后** ⇒ 本周期算出的控制字/目标下个周期才发出，固有 1 周期延迟
- 运行期模式切换未实现：CSV/CST 有代码分支（`control_session.c:14-19`、
  `cia_process_image.c:363-372`），但模式在计划构建期由 `selected_mode_id` 固定；
  外部目标路径硬要求 mode==8（`session_target.c:41-45`）
- 单线程周期会话 + 一个辅助 socket 线程；实际只跑 CSP
- 周期定时用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`（`cycle_clock.c:96`）；
  DC 对齐用 PI 控制器调下一周期长度（`cycle_clock.c:181-197`）

### 4.5 文档与代码不一致清单

| 文档 | 声称 | 实际 |
| --- | --- | --- |
| `phase-1.md` | 进行中，禁止变化目标 | `emaster-master` 默认 demo 发 ±180° 正弦（`tools/master/main.c:106`），直接违反该禁令 |
| `README.md` | 1° = 1274 脉冲（负载侧） | `main.c:107` 用 `angle_degrees * 16384.0 / 360.0` 是**电机侧** counts ⇒ ±180° 实际只有 ±8192 counts ≈ 输出轴 ±6.43° |
| `error-recovery.md` | 设计阶段 | af458d8/42a53a0 已实现 |
| `TOPOLOGY_REFACTOR.md` | 复选框未勾 | cb7a76d/8e0cbf0 已完成 |
| `MOTOR_PARAMETERS.md` | 有软限位 | 实际只有 607D 读回，±2e9 等同无限制，无用户可配限位 |
| `功能要求.md` 电流电压监控未勾 | —— | **该未勾是对的**：edc525b/5ce284b 加的报告字段因对象不在 PDO 而恒为 0，是死代码 |

### 4.6 验证链本身是断的

- **项目自带的 `validate_project.py` 在 HEAD 上失败，3 个错误**
  ⇒ phase-1 的"配置一致性"完成条件未满足
- **CI 构建并跑 cppcheck，但不跑 `validate_project.py`，也不跑 ctest** ⇒ 上述失败无人发现
- `orangepi-stability-test` 部署配置**在仓库中不存在** ⇒ 稳定性报告来自未入库的代码
- `orangepi_bench.json` 引用了不存在的运动配置文件
- 双轴部署缺 `motion_profile_id` ⇒ 配置里 2000 毫度跟随误差上限从未生效
  （`max_following_error_counts: 0`，实测 `max_observed` 991/1029）
- **CSP36° 成功基线的报告来自游离提交** 957e9cd / 4468712 / a58e352 / a860181 /
  5deb751 / c9dd001 / 2c06636 / cee7b9d —— 从任何分支都不可达。它们产出的
  schema_version 6 报告带 `tracking_error` 遥测，而当前 main 只发 schema_version 3
  且无这些字段 ⇒ **该成功证据无法从当前 main 复现**

### 4.7 已实现但未被验证过的

- WKC 容差：已实现且启用，但所有报告 `wkc_mismatch=0` ⇒ 从未被触发过
- 死区/AL 状态/CiA402 故障恢复：已配置但处于关闭状态
- SM2/SM3 已由两台台架真机读回确认（55/55 最终诊断读取）；200E `sync_loss_count` 为零
- 0x200E 子索引在配置与审计输出之间存在不一致
- 无 Quick Stop、故障注入、断线重连的任何测试证据；**无 12 轴记录**

### 4.8 两个曾被混为一谈的问题（已分开）

1. **6061=0 且 6064=0**（9月2-3日）：电机从未动过。9月4日 08:42 前 md=0，
   09:41 起 `validation-68f9bc5` 报告 md=8 ⇒ 疑似 68f9bc5 修了过程映像字段绑定/偏移，
   但**无提交信息或文档记录根因**
2. **驱动器静置后不响应**（9月8日）：6064 读回 458634 合理，但不执行 607A。
   05:29 能动，08:00 前停止。属设备参数问题
   （`tmp/emaster-new-motor.pcap` 暗示期间换过硬件）

## 5 调查缺口

- 外网全程不可达 ⇒ 第 1 节的 IgH/acontis/TwinCAT/ros2_control/ETG.1500 条目均未抓取验证
- 本驱动 `mbx_rl`（邮箱尺寸）未记录 ⇒ 2.3 的 SDO 帧时估算缺精确输入
- `fixed_csv`（0x319800）的 1A02 内容未真机验证（ESI 与手册冲突）
- 修复 `6c4ea9d` 之后无任何 sync0_margin 数据
- 3 轴及以上无任何时序数据；12 轴既无厂商数据也无实测
- Orange Pi 台架本次不可达（SSH 超时），`emaster-move-deg` 未编译未上机

---

原始调查产物（五个维度，约 155k 字符）恢复于
`<transcriptDir>/wf_bc3ad848-f4e/recovered/`，属会话临时目录，不随仓库保留。


