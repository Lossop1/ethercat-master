# 主站实现说明

本文说明主站的内部实现。使用说明见 [README](../README.md)。测试条件、判据与结果见 [测试方案](test-plan.md)。

写给改代码的人和写控制程序的集成者。新用户不需要读本文。

---

## 1 架构与所有权

目录按职责组织。CMake target 约束依赖方向。

两条边界：

- 公共接口不暴露 SOEM 类型。
- 设备、部署与方案由配置表达。入口中仍有缺省值和兜底参数。

**协议标准索引和数据类型属于实现知识。设备特性、拓扑数量、时序和运动限值属于配置。**

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
| `src/bus/soem/` | SOEM 集成边界。周期交换、配置/停机及观测 SDO 分属对应路径，不能描述为全部串行 |
| `src/audit/` | 保存访问事实、连续同值合并、退出后序列化 |
| `apps/master/`、`src/messages/` | 入口、信号处理、报告发布和中文展示 |

`src/cyclic/` 提供总线无关的时序、直方图和滑窗逻辑。`src/safety/` 提供条件汇总与决策，当前由 `session_supervisor.c` 接入控制循环。

库中存在的能力是否适用于具体部署，仍须检查调用路径和配置。

## 2 主线：从配置到周期

**核心理念：扫描结果是权威，配置文件表达意图。**

```text
配置文件（期望） → 扫描总线（真相） → 身份匹配映射 → 运行时API → 工具查询适配
```

会话启动分六个阶段（`include/emaster/audit/run_audit.h:9-17`）：

| 阶段 | 做什么 |
| --- | --- |
| `PREOP_CONFIGURATION` | 等全体到 PRE-OP → 读换算 SDO（读不到 `608F`/`6091` 时用设备档 `conversion` 的声明值兜底，来源记进报告）→ 发现设备实际映射并判定（逐条相同 / 采用设备实际 / 要求改写）→ 只在要求改写时才分配 PDO → 配 DC |
| `SAFEOP_INITIALIZATION` | 进 SAFE-OP，读回实际位置作为起点（**此时输出不生效**） |
| `OPERATION_REQUEST` | 请求 OP |
| `CYCLIC_OPERATION` | 1 ms 周期交换 |
| `SAFE_STOP` | 安全停机 |
| `FINAL_DIAGNOSTIC` | 退出 OP 后走邮箱读诊断 |

各阶段的验收步骤与证据要求见[测试方案](test-plan.md)的正式测试矩阵。

## 3 总线内部结构

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

## 4 周期与并发

主线程按**绝对截止点**唤醒后立即发送。接收完成后，主线程记录审计、计算相位修正、处理反馈并生成下一帧。

参数化的过程帧相位与 Sync0 偏移具有不同意义。**周期线程不能以"固定休眠 1 ms"代替相位跟随。**

期限已错过时退出正常控制。**不连续补发历史帧，不自动放宽 WKC 判据。**

PDO 审计游标按实际字段数量分配，每个字段直接查找上次记录。周期前预留并触及记录存储，**周期中不扩容、不写文件**。容量耗尽后明确记录截断。

**周期线程不发起慢速遥测 SDO 请求，但仍处理 SOEM 邮箱。**

每个物理从站还分别聚合主机单调时间、参考 DC 时间、发送/往返耗时、发送迟到量和距 Sync0 的理论裕量。这些数值用于定位时序，**不能替代从站提供的 SM2 完成时间戳**。

动态 PDO 直接提供模式反馈。固定 PDO 缺少模式反馈时，在 SAFE-OP 初始化阶段通过 SOEM SDO API 读回并在报告中保留。**周期控制线程不执行这些阻塞 SDO。** 观测线程仍在 OP 期间用同一会话 context 访问邮箱，`session_exchange.c` 也调用 SOEM 邮箱处理器。不能把"控制线程不执行 SDO"写成"OP 期间没有邮箱流量"。

**当前源码已接受 CSP/CSV/CST（模式 8/9/10）。** `src/session/session_plan.c` 允许这三种模式；`control_session.c` 按模式选择位置、速度或力矩目标/反馈；`session_target.c` 做目标检查。接口仍沿用 `position_target_source` 命名，但模式不同，目标量纲不同。不能把位置客户端直接用于 CSV/CST。当前正式测试方案仍只覆盖 CSP，源码路径存在不代表其它模式已完成真机验收。

截止时间检查和 WKC 检查属于执行条件。诊断性模式反馈策略**只允许保持位置**。配置了变化轨迹的轴必须匹配请求模式后才开始运动。**所有轴在同一次协调步骤中处理，不发布部分目标集合。**

## 5 位置命令入口

CSP 的最小产品入口必须把**位置目标输入**和**周期 PDO 发送**分开。

应用层通过 `emaster_control_session_callbacks_t.position_target_source` 提交完整轴集合的目标位置。主站周期线程只接受明确的当前目标，并检查已读回的软件位置范围。

**目标单位是已按当前从站换算完成的原始位置计数。** 单位换算、绝对零点和轨迹限制由独立的上层命令或轨迹模块负责。

位置目标来源回调只提供当前周期的完整目标快照。回调返回 `HOLD` 时保持上一目标。返回 `UPDATED` 时，主站检查所有轴的已读回软件位置范围，然后写入下一帧 CSP PDO。回调为空时使用部署引用的固定运动方案。`apps/master/main.c` 安装外部目标回调，未收到目标时保持初始位置，不自动运行固定轨迹。

这个回调契约不包含外部发送序号或客户端时间戳。命令服务器在主机接收目标时记录本机单调时间，入口回调用它检查目标年龄。这不能识别客户端发送前的排队延迟或重放目标。多轴协调器的 sequence 是主站内部周期号，也不能证明外部命令顺序。

## 6 两层状态：AL 与 CiA 402

**两层状态必须分别读。本节单独说明。**

| | EtherCAT AL（ESC 层） | CiA 402（驱动器层） |
| --- | --- | --- |
| 载体 | 寄存器 `0x0120` / `0x0130` / `0x0134` | 对象 `0x6040` / `0x6041` / `0x603F` |
| 状态 | INIT 1｜PRE-OP 2｜SAFE-OP 4｜OP 8 | 八个状态，见 `include/emaster/cia402/controller.h:8-19` |
| 通道 | INIT 无邮箱无 PDO｜PRE-OP 有邮箱｜SAFE-OP 有输入无输出｜OP 双向 | 邮箱（PRE-OP 及以上）或 PDO（仅 OP） |
| 谁管 | 主站的状态请求 | 驱动器的内部状态机 |

**两层状态必须分别验证。** AL OP 表示过程数据通信状态，不等于驱动器已使能。请求 INIT 也不构成驱动器撤使能的确认。停机路径先尝试撤销 CiA 402 使能并读回状态，再结束总线会话。

**使用边界：**

1. **不能用 AL 状态推断驱动器使不使能。** 两个都要读。
2. **不能用 PDO 去送"停止周期通道"的命令。** 从站掉出 OP 后控制字就到不了它。**禁用必须走邮箱。**
3. **AL 在 INIT 时读不到 `0x6041`**（邮箱关闭）。这时只能说"读不到"。
4. **`0x0134` 只在 `0x0130` 的 bit4（Error Indicator）置起时才有意义。** 没有 error 位时读它等于没读。SOEM 只在看到 error 位时才去读，这是照规范做。

**对象编号的权威链**：CiA 402（IEC 61800-7-201）规定编号 → 厂商在 ESI（XML）里声明（客户放在 `esi/` 下）→ 本项目在设备档的 PDO 映射表里引用 → 主站按表建过程映像。EDS（CANopen）本项目读不了，见 `esi/README.md`。

`src/cia402/controller.c` 按标准状态掩码解码状态字。设备配置另提供停用确认掩码。标准状态名不能代替厂商扩展位定义。`Voltage enabled` 等状态位也不等于独立的电气断能测量。

停机循环判"全部轴已安全停止"用两条判据：设备档掩码命中，或标准 CiA 402 的两个状态（Switch-on-disabled / Not-ready-to-switch-on）。**只要有轴没填掩码，那一轮就只能走标准判据**——报告用 `shutdown_stop_criterion` 记下用的是哪条。两条都不命中、但末拍每轴都已离开运行使能时，结论是"未确认"（`SHUTDOWN_UNCONFIRMED`），不是"失败"：两者处置相反。掩码是台架实测出来的参数，不能从 ESI 推。

**为什么 AL 状态码不能当"为什么失败"的入口**：它满足的是"这次请求的转移失败了"。error 位没置起就说明从站不认为它拒绝过。**要问"请求到底发到没有"，看的是写操作的工作计数器，不是状态码。**

## 7 失败与报告

- **首次周期失败**保留状态、阶段、交换号、WKC 是否取得及最后参考时间。它**不被后续停机交换覆盖**。
- **退出时的 AL 状态是退出快照，不冒充失败瞬间。** 过程数据访问成功也不等于电机完成指令。
- 停机尝试之后，先退出 OP，再关闭 Sync0 和读取诊断。报告以 `post_cycle_diagnostic` 标明阶段。**不由末尾计数器反推首次异常顺序。**
- 退出状态切换失败时跳过批量 SDO，继续关闭总线。**绝不把缺少反馈当作停用成功。**
- 报告文件在周期结束后**原子发布**。中文正文由消息目录管理，执行模块只返回结构化结果。

**报告里与状态有关的段**：

| 段 | 回答 |
| --- | --- |
| `startup_cia402` | **主站接手时，驱动器在不在使劲** |
| `shutdown_cia402` | **主站退出后，它还在不在使劲** |
| `shutdown_al_entry` / `_pre_stop` / `_exit` | ESC 层三个坐标的 AL 状态与状态码 |
| `timing_excursions` | 每一拍时序越界的分布 |
| `cycle_trace` | 首个异常前后各 200 拍的逐拍记录 |

**慢速遥测会写入报告。** `src/audit/report_sections.c` 保存逐轴慢速遥测快照、读取有效标志和年龄。开启观测才有采样。快照不是完整时间序列，也不是故障瞬间同步测量。读取失败、过期和有效零值必须区分。`0x0134` 需结合 AL Error Indicator 与读取有效性解释。

## 8 错误恢复策略

**阈值不写在代码里。** 阈值的来源是 `config/error_recovery_policies/<id>.json`（`default` / `strict` / `tolerant`），由部署档的 `error_recovery_policy_id` 引用。策略未找到时用保守默认（**首次错误即停机**）。

| 块 | 默认 | 含义 |
| --- | --- | --- |
| `wkc_recovery` | **连续 5**、60 秒窗口内 50 | 短帧 |
| `no_frame_recovery` | 同上 | 整帧未回 |
| `deadline_recovery` | 连续 10 | 周期截止点错失 |
| `al_state_recovery` / `cia402_fault_recovery` / `dc_late_recovery` | `enabled=false` | 未启用（键名不能简写，配置校验按全名） |

**连续阈值与滑动窗口阈值都参与判定。** 连续段是联合计数（整帧未回也计入）。窗口分别统计短帧和整帧缺失。因此 `wkc_short_window_max` 可能小于 `wkc_max_consecutive`，两者不是同一口径。

**累计量按类取**：整帧缺失用自己的累计，短帧用"联合累计 − 整帧缺失累计"。`wkc_*` 三个字段的语义不变，仍是联合计数。

滑动窗口在会话建立时就建好，**不等到第一次出错时才惰性初始化**。那次初始化恰好发生在已经出错、最不该多做事的时刻。

**`no_frame_recovery` 未另设 `enabled`，沿用 `wkc_recovery.enabled`。** 这样避免出现"wkc 恢复开着、整帧缺失却立刻停机"这种组合。

## 9 观测通道

**独立的只读 unix 套接字**（`/tmp/emaster-<部署>-obs.sock`）。**要 `EMASTER_OBSERVATION=1` 才存在。**

- 观测线程做**邮箱 SDO 往返**。它读设备配置里 `slow_telemetry` 列的对象：电流 `6078h`、母线电压 `6079h`、温度 `200Bh:01h`／`200Bh:02h`、转速、读回目标、限幅等。
- 慢速 SDO 读取由观测线程发起。周期线程只拷贝快照。同时交换路径仍会调用 SOEM 邮箱处理器。
- 停机序言的 AL 快照与 join 耗时写进报告（`shutdown_al_entry` / `shutdown_al_pre_stop` / `shutdown_observer_join_ns`）。

报告中的 join 时间是单次运行测量值，不能作为停机耗时上界。

**电流／电压／温度的当前通路**：设备配置将这些对象放在 `slow_telemetry`。所选 PDO 布局未包含这些字段，因此通过 SDO 读取。固定 PDO 布局限制主站重映射。对象是否支持其它映射须另查设备定义，不能只凭模块的 `Fixed` 属性推出。SDO 往返时间受设备、总线与线程调度影响，不是固定上界。主站不主动重映射设备：设备实际映射与设备档不同时，按设备实际建映像并逐条记差异；只有运行方案要用的字段对不上、且设备档声明可改映射时才改写设备。

## 10 DC 与同步

### 10.1 Sync0 由谁产生

Sync0 是从站 **ESC 的分布式时钟（DC）硬件事件**。主站配置起始时刻、周期和激活位，由 ESC 按自己的 DC 时钟产生事件。Linux 线程没有逐周期发送一个"Sync0 命令"。驱动器内部控制或目标锁存怎样使用这个事件，还取决于设备固件与 SM2/SM3 同步设置。

Sync0、主机截止点和报告时间不是同一个时间域：

| 量 | 来源和采样点 | 含义 |
| --- | --- | --- |
| 主机时间 H | Linux `clock_gettime(CLOCK_MONOTONIC)`；发送调用前后、接收与诊断完成后等软件点 | 主机软件事件时间，不是网卡硬件时间戳，不是从站事件时间 |
| 参考 DC 时间 R | 过程帧附加 FRMW 数据报在参考 ESC 读取 `DCSYSTIME`（0x0910），SOEM 解包保存到 `context.DCtime` | 参考站处理该数据报时的 DC 系统时间，不是每轴目标锁存时刻 |
| 周期 T、Sync0 相位 S、过程帧目标相位 P | 运行方案的 `cycle_ns`、`sync0_shift_ns`、`process_data_phase_ns` | 配置意图，不是实测边沿 |

H 与 R 不能直接相减。周期计时使用单调时钟，其绝对值不等于 UTC。DC 的初始纪元由 SOEM 配置，运行期相位比较使用 R 对 T 取模。

### 10.2 DC 参考与传播延迟从哪里来

`external/SOEM/src/ec_dc.c::ecx_configdc()` 广播触发 DC 端口接收时间锁存，读取各端口硬件时间，结合拓扑估算从站传播延迟 `pdelay`，写入 DC 系统延迟和偏移寄存器。传播计算包含正向/返回延迟近似相等的假设。pdelay 是配置期估计，不是每帧测量。

SOEM 将扫描到的组内第一个支持 DC 的从站作为参考，其实际总线编号保存在 `grouplist[0].DCnext`，主站报告为 `dc_reference_slave`。参考站未必等于逻辑轴 1。SOEM 配置还用 `osal_current_time()` 生成 EtherCAT 纪元的初始系统时间偏移。这不是主机周期等待用的 H。

`external/SOEM/src/ec_main.c` 给过程帧追加 FRMW：读取参考站的 DC 时间并向后续 DC 站传播，返回帧解包更新 R。主站未逐周期读出每轴本地时钟与参考的残差。一个参考值不能证明全部从站偏差小于某个保证值。

### 10.3 Sync0 的绝对启动时刻

`ecx_dcsync0()` 读取待配置从站本地 DC 系统时间 t，先停止周期事件，再计算（T>0）：

```text
start = floor((t + SyncDelay) / T) × T + T + S
```

SOEM 将 start 写 `DCSTART0`（0x0990），将 T 写 `DCCYCLE0`（0x09A0），通过 `DCCUC/DCSYNCACT`（0x0980/0x0981）选择 EtherCAT 配置权限并激活周期和 Sync0。此后按该 DC 网格由 ESC 硬件触发。

`session_setup.c` 读回周期和组合激活值，与方案比较。这个检查支持"这些寄存器读回一致"。当前不完整读回 DCSTART0，SOEM `DCactive/DCshift` 是软件镜像，且 `ecx_dcsync0()` 内若干访问返回值未检查。因此不能只凭调用完成或软件镜像证明实际 Sync0 起始时刻正确。

### 10.4 主机怎样跟随参考时钟

`cycle_clock.c` 用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` 等待绝对截止点。每次过程交换后按 R 的周期相位更新 PI：

```text
e = -centered((R - P) mod T)
integral = saturating_add(integral, e)
correction = e / 100 + integral / 50000
correction 限制在 [-T/2, T/2]
下一等待间隔 = T + correction
```

centered 将相位差归一化到约 [-T/2,T/2]。积分累加用饱和避免溢出。PI 改变下一次**主机等待间隔**，不直接修改从站 Sync0。它可以跟随相位漂移，但不会消除 Linux 调度延迟、网卡排队和通信故障。增益常数本身不是可靠性保证。

### 10.5 报告的 Sync0 裕量不是边沿时间戳

`src/cyclic/timing.c` 按每轴传播估计计算：

```text
arrival_phase = positive_mod(R + pdelay, T)
sync0_phase = positive_mod(S, T)
sync0_margin = centered(sync0_phase - arrival_phase, T)
phase_error = centered(arrival_phase - P, T)
```

该值是**参考 DC 样本加传播模型与配置相位的差**。它没有直接测到各站 SM2/PDO 接收完成、驱动器目标锁存或 Sync0 实际边沿。报文中 PDO 与 FRMW 的偏移、参考站到目标站的实际延迟、残余时钟差和驱动器内部处理均未成为完整的逐帧测量误差项。

centered 只保留半周期内相位差，不能区分相差整数个周期的事件。因此 `margin>=0` 不是"目标在正确控制周期的 Sync0 前可靠锁存"的证明。负值也应先作为模型诊断线索解释。配置 P=50 µs、S=250 µs 只有名义 200 µs 相位差，不能自动推出最坏保护余量≥100 µs。

真实锁存余量的证明需要同一时钟域的 PDO/SM2 完成事件与 Sync0 事件，或可关联的外部硬件测量，并给出同步残差、报文偏移、传播、锁存和测量误差的上界。当前报告不提供这套完整证明。

### 10.6 DC 样本与启动判据

`session_exchange.c` 以 `dc_required && context.DCtime>0` 标记 DC 统计有效。SOEM 收不到相关响应时可能保留旧 R。大于零不能证明本周期是新样本，当前也未单独核验 DC 数据报的 WKC。正常过程 WKC 后的 PI 更新同样缺独立新鲜度标志。

`session_start.c` 在有限启动交换次数内要求连续 `dc_startup_cycles` 个各 DC 轴估计 margin≥0 的有效样本，失败清零。`dc_startup_stable` 是这项软件条件达标。没有独立边沿测量、最小正余量或全部从站同步残差检查，不能将它等同于同步可靠性已经证明。

freerun 运行方案选择 `strategy:"sm"`，不配置 DC/Sync0，相应 DC 推导无有效样本。主机仍按周期交换。不同同步策略须分别验证，不能从一种模式的结果推断另一种。

## 11 实时基线与证据边界

**主机要求**（与 README 第 2 节同源，此处给出完整项）：

- 使用 PREEMPT_RT 内核，并记录内核与配置哈希。
- 服务账号具备 `SCHED_FIFO` 权限，进程内存锁定。
- 周期线程使用**专用 CPU**。EtherCAT 网卡 IRQ 亲和性经过明确配置。
- EtherCAT 使用**专用网卡**。网卡无 IP、路由、PTP、激光雷达或 NetworkManager 业务。
- 网卡中断合并和卸载参数已记录并经过有意配置。
- CPU 频率策略、温度限制和后台服务负载已记录。

**1 ms 设计目标**

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

**可靠性的分层证据**

本项目的"可靠"必须针对明确行为、配置、负载和观察时长判断，不能由单个状态或统计量给出。以下检查构成逐层证据。后一层不能由前一层自动推出。

| 要验证的行为 | 当前检查及源码 | 能支持的结论 | 仍需补充的证据 |
| --- | --- | --- | --- |
| 接入了期望设备 | `topology_mapper.c`、`session_setup.c`：扫描身份、匹配轴、核对 PDO 布局与换算 | 本次扫描与所用配置的匹配条件成立 | 同型号物理轴绑定、布线和外部轴标签核对 |
| 本周期过程访问完整 | `session_setup.c` 从映像计算预期 WKC；`session_exchange.c` 比较 SOEM 返回 WKC | 回帧的访问计数与预期一致 | 逐轴目标锁存、内部执行、机械动作；WKC 不是目标值校验码或运动完成确认 |
| 主机没有越过软件期限 | `cycle_clock.c`、`session_exchange.c`：单调时间与下一截止点比较，统计发送迟到与各段耗时 | 已采样的软件执行点符合期限条件 | NIC 发出时刻、驱动器接收截止点、未覆盖代码段及真实最坏执行时间 |
| 目标集合满足输入约束 | `session_target.c`：轴数、模式与目标到实际位置的距离验证（`target_guard.c`）；`coordinator.c`：内部序号与期限检查 | 通过检查的候选集合可进入下一帧输出 | 外部发送顺序、端到端年龄、重放防护、连续轨迹速度/加速度约束 |
| 驱动器可接受运动目标 | `controller.c`、`session_control.c`：状态字解码、模式反馈、全轴使能条件 | 所读反馈满足控制路径的使能/模式条件 | 反馈新鲜度与设备固件定义；已使能不证明目标已执行 |
| 机械动作符合要求 | `session_control.c`：跟随误差、固定轨迹终点和方向检查；观测输出实际位置 | 主站采样点内的误差未超所配置护栏 | 产品精度阈值、完整目标/反馈时间序列、外部位置测量和负载条件 |
| 停止动作被确认 | `session_shutdown.c`：撤使能写入、逐轴状态字读取及设备停用掩码 | 有效读回满足软件停用条件 | 停止时间上界、机械停止与电气断能；总线完全中断时依赖外部保护 |
| Sync0 同步满足锁存要求 | 第 10 节的参考 DC、相位跟随和裕量估计 | 软件同步模型与配置条件达标 | 各站同步残差及 PDO/SM2 与 Sync0 的关联测量，当前尚不足以证明真实锁存余量 |

恢复策略决定发现异常后的软件动作，不是可靠性合格线。稳定性验收按[测试方案](test-plan.md)要求无异常。故障注入另验证检测、记录、停止和恢复语义。短时零故障或平均延迟较小，都不能证明最坏延迟、长期故障率或功能安全等级。

## 12 端到端数据流与动作时序

### 12.1 启动：配置到 OP

```text
config/*.json
  └─ CMake 配置期生成器（tools/gen/generate_*.py）→ build/generated/*.c
       └─ emaster-master 读取编译期配置 → 查 deployment_id → 构造 session plan
            └─ SOEM 扫描从站 → 校验身份、拓扑、PDO 与能力
                 └─ PRE-OP：读身份/换算/初始 CiA 402；选 PDO、写启动 SDO、配 DC
                      └─ SAFE-OP：建立 process image，读取实际位置并初始化目标
                           └─ OP：确认全轴运行模式和使能 → 按周期开始控制
```

入口和文件对应关系：

| 步骤 | 主要源码 |
| --- | --- |
| 参数解析、部署查找、实时调度、回调装配、报告发布 | `apps/master/main.c`、`apps/master/console.c` |
| JSON 到生成 C | `CMakeLists.txt`、`tools/gen/generate_config_apply.py`、`generate_catalog_apply.py`、`generate_runtime_apply.py`、`generate_deployment_apply.py`、`generate_slave_catalog.py`（被前者 import）、`generate_error_recovery_config.py`、`generate_messages.py` |
| 部署与轴计划校验 | `src/session/session_plan.c`、`src/config/`、`src/catalog/` |
| SOEM 初始化、身份与 PDO/DC 配置 | `src/bus/session/control_session.c`、`session_setup.c`、`soem_common.c` |
| SAFE-OP 到 OP、起始位置与控制字初始值 | `session_start.c`、`cia_process_image.c` |
| PDO 描述与字节编解码 | `src/protocol/pdo_layout.c`、`pdo_codec.c`、`cia_process_image.c` |

配置层表达期望，从站扫描结果表达现场事实。身份或 PDO 不匹配应拒绝建立周期控制。安全门在 `src/safety/gate.c` 汇总拓扑、PDO、输出初始化、通信、反馈、模式、同步、目标、驱动器限位、使能授权、停止和故障锁存条件。`session_supervisor.c` 将决策接入主循环。

### 12.2 每 1 ms 周期：反馈、目标与下一帧

```text
CLOCK_MONOTONIC 绝对截止点唤醒
  → SOEM send/receive（按计划相位交换 PDO）
  → 检查 WKC、截止期、参考 DC/Sync0 时序并保存首故障现场
  → 按轴解码 PDO：状态字、模式、位置/速度/力矩反馈、限位与故障
  → CiA 402 controller 计算下一控制字
  → 多轴 coordinator 统一检查轴集合与轨迹进度
  → 安全门批准后检查外部/固定运动目标；外部目标过远即拒收该条并保留上一目标
  → 编码完整 PDO 输出，进入下一周期
```

主要实现位于 `session_control.c`（周期控制总编排）、`session_exchange.c`（定时和 SOEM 交换）、`cia_process_image.c`（轴 PDO）、`src/cia402/controller.c`（状态机）、`src/multiaxis/coordinator.c`（全轴一致性）、`src/motion/`（相对位置/速度轨迹）和 `session_target.c`（运行时目标验证）。`cycle_clock.c` 负责绝对时基与 DC 参考相位修正。

截止点已错过时不补发旧帧。恢复行为须结合选定策略和实际检查路径判断。周期内 ESC 错误计数器 FPRD 诊断读有抽样和预算限制，它不是邮箱 SDO。

控制生命周期与数据源必须分清。配置运动方案时由 `src/motion/` 生成目标。外部控制时 `apps/master/main.c` 的 `position_target_source()` 从互斥锁保护的目标缓冲取完整快照。`session_target.c` 在所有轴通过检查后才提交目标，随后 PDO 编码才对总线生效。命令"成功"只代表已接收/进入处理，不等于驱动器已到位。应继续看反馈、following error 与逐轴结果。

### 12.3 外部命令与观测数据

```text
GUI/TUI/CLI
  → tools/emaster_client.py / tools/interactive_control.py
  → AF_UNIX SOCK_STREAM 命令 socket
  → command_server.c 监听线程解析并排队
  → 周期控制线程取命令，读状态/更新目标/请求停机
  → 非阻塞 send 回应 OK|... 或 ERROR|...
```

命令线程主动退出实时策略。响应使用 `MSG_DONTWAIT`。短写/阻塞视作慢客户端并关闭连接，避免实时线程被 socket 写阻塞。状态类命令会在周期线程执行。客户端请求频率不等于周期频率。

观测分为快、慢两路。快路由周期线程把 PDO 反馈组为帧写入 `src/observation/ring.c`，独立观测 socket 供 `tools/observation_client.py` 解码。发布耗时计入报告。慢路由 `session_observer_thread.c` 通过邮箱 SDO 读设备 `slow_telemetry` 配置项，写线程安全快照。周期线程只拷贝已采样值，不执行这些 SDO。停机时请求 observer 退出，在内联停机序言各周期继续发送 PDO 等待线程退出，然后 join 并把快照写入报告。

报告由 `src/audit/run_audit.c` 累积总线访问和有效性信息，`src/audit/report_sections.c`、`run_report.c` 序列化 JSON，入口在会话完全收尾后原子发布。实时周期内不写报告文件。

### 12.4 停止与异常动作

```text
SIGINT / stop / shutdown / 应用 stop callback
  → session_supervisor / 安全门锁存停止请求与来源
  → 请求 observer 停止，在可通信时逐周期 drain 并采集停机 AL 快照
  → SAFE_STOP：撤销各轴使能、检查停用状态
  → 停止 Sync0、邮箱最终诊断、请求从站回 INIT
  → 汇总审计并发布报告
```

停机实现主要在 `session_shutdown.c`。`main.c` 的 SIGINT handler 只设置 `sig_atomic_t` 标志，不在信号上下文中做复杂 EtherCAT 操作。命令 `shutdown` 请求同一有序退出路径。`quick_stop` 设置 CiA 402 quick-stop goal，但具体驱动器反应与最大时间仍需硬件验证。软件命令不能替代 STO/急停。掉出 OP 后 PDO 不再可靠，最终停用需要可用的邮箱和从站响应。总线完全中断时软件不能保证电气断能。

## 13 功能状态与验证边界

| 能力 | 当前实现与证据边界 |
| --- | --- |
| 发现、身份校验、PDO、CiA 402、DC、CSP | 已有实现；是否满足验收以[测试方案](test-plan.md)矩阵为准 |
| 多轴外部位置目标 | 已有 socket、客户端与 GUI；需逐轴验证运动、目标超时和重连 |
| 位置护栏、跟随误差、WKC/截止期监控 | 已有配置与处理路径；保护值不是精度验收线 |
| 停机与诊断 | 已有 PDO 停用及邮箱补偿路径；缺少逐轴有效读回不能证明禁用成功 |
| 慢速遥测、时序统计、异常窗口、报告 | 已有实现；观测负载需 A/B 验证，快照不是完整历史 |
| GUI、TUI、TCP 桥 | 已有工具；桥无认证，使用受控管理网络 |
| CST/CSV/混合模式 | 配置或枚举存在不代表完整控制链可用，不纳入本次 CSP 验收 |
| 长时稳定性、精度、故障恢复 | 尚无充分统一验收证据，见[测试方案](test-plan.md) |
| 热插拔、动态轴数、多主站协同、功能安全 | 不作为当前已支持能力承诺 |

## 14 代码内部风险

本节的条目是代码内部问题。它们不影响客户当前的使用方式，但限制后续改动的自由度。

| 风险 | 代码证据与影响 | 处理方向 |
| --- | --- | --- |
| 观测线程共享普通 `volatile bool` | `session_internal.h` 的 `observer_running` / `observer_exited` 在 observer 与周期线程间读写；`volatile` 不提供 C 线程同步或原子语义，构成潜在数据竞争 | 改 C11 `_Atomic` 或明确的 mutex/condition 同步，并以 ThreadSanitizer/并发测试验证 |
| 命令线程生命周期与连接并发 | `command_server.c` 的 `running` 是跨线程普通 bool；响应取 fd 快照后解锁再 send，监听线程同时可能 close/accept，数值相同的 fd 可能已代表新连接；队列项也未绑定连接代次 | 生命周期标志用原子或锁；明确连接所有权、代次与响应归属，覆盖快速断连/重连场景 |
| 部分 DC/停机访问使用轴序号 | PDO 路径使用映射后的总线位置，但 `session_setup.c` 的 Sync0 配置及 `session_shutdown.c` 的关闭 Sync0/部分邮箱路径仍有 `axis_index + 1` | 配置轴与总线位置不连续时可能读写错误从站；此类拓扑不能直接宣称支持 |
| DC 样本没有独立新鲜度标记 | `session_exchange.c` 以 `context.DCtime > 0` 判断有效；SOEM 未收到新响应时可能保留上一值 | `dc_startup_stable` 和 PI 反馈可能包含旧时间；需要按数据报有效性和周期序号复核 |
| 目标判据用的是主站采样的实际位置 | `session_target.c` 的 `target_guard` 比较对象是上一拍从 PDO 解出的实际位置，不是驱动器内部锁存值 | 判据本身只拦量纲错位与未初始化值，这个差别不影响它的用途；若将来要求它做精度判定则必须先解决这一条 |

审查时特别区分代码与注释。`main.c` 回调注释仍写"从未收到外部目标生成正弦波"，但实际分支保持初始位置，没有自动启动 demo。本文按实现说明。
