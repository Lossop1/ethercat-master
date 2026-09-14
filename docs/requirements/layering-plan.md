# 分层计划与原子问题排序（2026-09-11，更新于 2026-09-14）

依据 `capability-baseline.md`。分层原则：**自底向上，下层不知道上层存在。**
每一层只向下依赖，向上只提供"数据点 + 能力位图 + 时序契约"三件，不提供语义解释。

---

## 1 层级划分

| 层 | 名称 | 职责 | 明确不做 | 时序等级 |
| --- | --- | --- | --- | --- |
| L0 | 主机实时基座 | RT 线程、内存锁定、CPU 隔离、周期时钟、死区判定 | 不知道 EtherCAT | 周期内 |
| L1 | EtherCAT 传输 | 帧收发、FMMU/SM、ESM、DC、WKC、错误计数器、邮箱调度 | 不知道 CiA402，不知道"轴" | 周期内 + 跨周期 |
| L2 | 过程映像与对象访问 | 字节偏移 ↔ 命名数据点；SDO 读写（非 RT 线程） | 不解释数据点含义 | 周期内 + 非实时 |
| L3 | 设备语义（CiA402 轴） | 状态机、控制字/状态字、模式、原始 counts 进出 | **不做单位换算**，不知道减速比 | 周期内 |
| L4 | 运动与安全 | 单位换算、限幅、插值、跟随误差、包线、周期内控制律（PD/阻抗） | 不知道命令从哪来 | 周期内 |
| L5 | 命令与观测接口 | 目标下行、观测上行、失效超时、双缓冲 | **不构造观测帧**，不知道上层是 RL 还是示教 | 跨周期 |
| L6 | 上层应用 | 策略推理、帧构造、任务编排 | 不在本仓库 | 上层自定 |

### 1.1 边界判据

一条功能属于哪层，看它**需要知道什么**：

- 需要知道减速比/编码器分辨率 → L4 及以上（L3 只搬 counts）
- 需要知道"这是一条腿的髋关节" → L6
- 需要知道帧格式/传输方式 → L5 以上
- 需要知道 SM/FMMU → L1

### 1.2 L4 必须承载周期内控制律

这不是设计偏好，是硬件逼出来的：`MIT_Control`（kp/kd + 前馈力矩）在 EtherCAT ESI 中
无对应对象（见基线 2.4）。若上层需要关节阻抗，PD 环只能放在 L4 按周期跑，
输出折算为 CST 力矩或 CSP 位置。L4 因此不是纯粹的"限幅层"。

### 1.3 L5 的形态由时序反推，不预设

已知量：EtherCAT 周期 1ms；典型策略频率 50-100Hz（rl_sar `config.yaml` 量级）。
⇒ 周期比在 10:1 到 20:1，**延迟不是瓶颈**。

真正的约束只有两条：

1. RT 周期**绝不能阻塞等待** L5 ⇒ 排除同步请求-响应
2. 上层命令缺失或迟到时必须安全降级 ⇒ 必须有失效超时（当前已实现）

满足这两条的实现：Unix 域套接字 + 互斥保护双缓冲（已实现）。

观测量按可得性分两档（硬件决定，非设计选择）：

- **周期档**：位置、速度、力矩（在 TxPDO 内，见基线 2.2）
- **慢速档**：电流、电压、温度（只能 SDO，约 3 周期 + 抖动，见基线 2.3）
  ⇒ 10-100ms 级轮询，错开轴号

IMU 不在 EtherCAT 链路上，本轮不纳入。

---

## 2 原子问题排序

排序依据：**下层不成立，上层的验证就无意义。**

### P0 修验证链（阻塞其它一切）

没有可信验证，下面每一项都无法确认做完了。

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P0.1 | `validate_project.py` 在 HEAD 失败（3 错误） | 退出码 0 | 已验证 |
| P0.2 | CI 不跑配置校验 | 校验进 CI | 已验证 |
| P0.3 | 时序统计只有 min/max | 输出 p50/p99/p99.9/p99.999 + 直方图 | 已验证 |
| P0.4 | 缺 `orangepi-stability-test` 部署配置；`orangepi_bench.json` 引用不存在的运动配置 | 配置入库且 validator 通过 | 已验证 |
| P0.5 | CSP36° 基线只存在于游离提交 | 从当前 main 复现，或明确标注该基线作废 | 已验证 |

**P0 完成记录**

**P0.1**（三个错误实为两个问题）：

- 悬空运动方案引用是回归。`8bfe32d` 把 `motion_profile_id` 改成
  `orangepi-bench.continuous-motion-test` 但从未创建该文件，已恢复为
  `orangepi-bench.output-positive-36deg`（approved、csp、`bench_axis_01`，与单从站拓扑匹配）。
  未新建"continuous-motion"方案：README 要求显式给出时长、稳定时间、每轴角度与误差边界，
  无依据编造
- 网口重复占用是**规则缺陷**，非配置错误。原规则"同一主机和网口只能有一个部署记录"
  与 README:58-59"`robot_12_axis` 与 `bench_single_slave` 并存"自相矛盾；台架物理只有一张
  EtherCAT 网卡，却需保留单从站/双从站/外部控制三份已验证场景。删配置合规会砸掉 29 处脚本
  引用且仍剩两个部署。已改为约束 `run_report_path` 唯一 —— 真实静态风险是不同场景的审计
  报告互相覆盖，网卡独占由运行时抢占自然保证。`config/README.md` 模型说明已同步
- 附带修复：验证器与四个生成器读配置用 `encoding="utf-8"`，遇 BOM 直接失败。项目在
  Windows 上开发，编辑器默认写 BOM。六处统一改为 `utf-8-sig`

**P0.2**：`校验配置` 步骤加入 `ci.yml`，位于构建之前（跨层引用错误比编译失败更廉价，
且悬空引用不一定会让生成器报错）。

原计划写"两者都进 CI"含 ctest，该判断有误：`1431b57`（8月31日）已有意删除整个 `tests/`
（含 179 行验证器负向契约测试）并把 `ctest` 从 CI、`include(CTest)` 从 CMake 移除。
仓库现为零测试，`tests/` 下只剩三个未跟踪 `.pyc`。该提交声称保留"资料一致性门槛"，
但那个门槛就是被删的测试 —— 这是 HEAD 验证器失败潜伏 11 天的直接原因。
按决定不重建夹具框架，`run_report_path` 规则目前无回归保护。

**P0.5**：CSP36° 基线（游离提交 `957e9cd` 等）明确标注作废，不从 main 复现。三条独立原因：
(1) 全部 sync0_margin 数据早于修复 `6c4ea9d`，旧公式存在回绕假象，修复后无任何新数据；
(2) 测量时无 RT 保障（mlockall / SCHED_FIFO / CPU 亲和均缺失），抖动量无法归因于总线；
(3) 产出 schema_version 6 报告的提交是游离提交，当前 main 只发 schema_version 3 且无
   `tracking_error` 字段，无法复现。
有效替代基线为 P1.4（2026-09-11，15分钟，deadline_missed=0），见 [[ethercat-csp36-timing-baseline]]。

**P0.4**：`orangepi_bench.json` 的悬空引用随 P0.1 一并修复。
`orangepi-stability-test` **不新增配置**：该报告生成于 9月9日 05:02，而覆盖同一场景的
`orangepi-current-bench-idle`（同拓扑 `bench-current-single-slave`、同 DC 1ms 运行方案、
无运动方案）是 9月10日 `8696498` 才加入的，当时确实无对应配置可用。新增近似重复配置正是
原网口规则要防的重复。**后续稳定性运行一律使用 `orangepi-current-bench-idle`**；
9月9日那份 `stability-test-latest.json` 来自未入库配置，不得作为可复现证据引用。

---

### P1 实时基座（L0）

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P1.1 | 无 `mlockall` | 启动即锁定，失败则拒绝进入 OP | 已验证 |
| P1.2 | 无 SCHED_FIFO / 无 CPU 亲和 | 周期线程 FIFO + 绑核，优先级可配 | 已验证 |
| P1.3 | 死区超时永久锁存（`cycle_clock.c:147-151`），`deadline_recovery` 无消费者 | 可恢复，且恢复行为可配 | 已验证 |
| P1.4 | 无长时运行证据（最长 14.9 秒） | 15 分钟连续运行报告，含 P0.3 的百分位；报告的 `samples × observed_cycle_ns ≥ 900s` | 已验证 |
| P1.5 | 修复 `6c4ea9d` 后无 sync0_margin 数据 | 重测，作废旧的 180us 说法 | 已验证 |
| P1.6 | CSP 运动可靠性未在有负载情况下长时验证 | 300 秒 CSP 双轴保持位置，wkc_error=0，deadline_missed 仅来自 SIGINT | 已验证 |

**P1.4**（2026-09-12，双从站 idle 15 分钟）：

- 报告：`runtime/reports/p14-dual-15min-verified.json`（78MB）
- 配置：`orangepi-bench-dual`（拓扑 `bench-dual-slave`，2 个从站）
- 模式：idle（保持初始位置，禁用自动正弦波）
- samples = 919,912，observed_cycle_ns = 1,000,000（1ms）
- 运行时长：919.9 秒 = 15.33 分钟
- deadline_missed_count = 0（双轴）
- wkc_mismatch_count = 0（双轴）
- sync0_margin p99.9 = [202~204]μs（双轴，裕量充足）
- round_trip p99.9 = 276~277μs，p99.999 = 336~337μs
- send_lateness p99.999 = 36~37μs

**P1.6**（2026-09-14，双从站 CSP 300 秒）：

- 根本原因修复：`EC_TIMEOUTRET=2000µs` 超过 1ms 周期。修复：`frame_timeout_us = cycle_ns/4`
  （1ms 周期下 250µs），限制在 [50,500]µs。变更位于 `src/bus/soem/session_exchange.c`
- 结果：sent=217 ok=217 fail=0，cycle_count=306,970，wkc_error_count=0
- deadline_missed=True 仅来自 SIGINT 正常停机（status_code=26）

**P1 完成后必须重测 P0.3 的全部时序量**，之前的数据只能作为无 RT 保障时的参照。

---

### P2 安全门禁（L4 的安全子集，先于功能）

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P2.1 | **客户端断开后主站继续驱动**（`external_targets_available` 永久有效） | 命令带时间戳，超时进受控停止 | 已验证 |
| P2.2 | 生产主站跟随误差保护关闭（`tools/master/main.c` 未设该字段） | 设置且有触发测试 | 已验证 |
| P2.3 | 外部目标无限幅无插值 | 速度/加速度限幅 + 越限拒绝 | 已验证 |
| P2.4 | Quick Stop（bit2）/ Halt（bit8）缺失 | 两者实现且有测试 | 已验证 |
| P2.5 | Fault Reset 调用路径已存在但未真机验证 | `Fault → Fault Reset → Switch On Disabled` 全程可走 | 已实现·未验证（硬件阻塞） |
| P2.6 | 运行期 603F 恒为 0 | 运行期读取（走 P4 的邮箱通道） | 已验证 |

P2.1 实现细节：外部目标缓冲 `buf->last_update_ns` 记录最后更新时刻；
`position_target_source` 每周期检查，超过 `EMASTER_EXTERNAL_TARGET_TIMEOUT_NS`（200ms）
后清除 `buf->available`，返回 `EMASTER_POSITION_TARGET_SOURCE_HOLD`。

**注意**：P2.2 的 `following_error_counts` 当前在 `main.c:365` 硬编码为 256000（200°），
注释标为"P4.3 验证：临时放宽"。该临时值未回退，见 P6.3。

P2.5 硬件阻塞原因：驱动器静置后位置反馈正常但不执行 607A，断电重启恢复（见 [[drive-idle-no-response]]）。

---

### P3 分层归位（L1/L2/L3 边界）

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P3.1 | `session_control.c:552-555` extern 引用 `tools/master/main.c` 的全局变量 | 外部目标状态下沉为总线层结构，上层通过接口注入；`target_probe` 不再复制定义 | 已验证 |
| P3.2 | 编码在发送之后 ⇒ 固有 1 周期延迟 | 明确记录为契约，或改为同周期编码 | 已验证 |
| P3.3 | 单位换算散落在工具层（`main.c:107` 用电机侧 counts，README 写负载侧） | 换算集中到 L4，L3 只进出 counts，两处口径统一 | 已验证 |
| P3.4 | demo 正弦违反 phase-1 禁令且幅值口径错 | demo 与生产路径分离 | 已验证 |

---

### P4 邮箱与慢速观测（L1 跨周期 + L2 非实时）

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P4.1 | `ecx_mbxhandler` 完全未用（92 个符号只用 16 个） | 周期内有界推进，limit 取值基于 P1.4 实测 round_trip 增量确定 | 已验证 |
| P4.2 | 缺 `mbx_rl` 记录 ⇒ 无法估算帧时 | 进 fingerprint | 已验证 |
| P4.3 | 电流/电压/温度报告字段恒为 0（死代码） | 走 SDO 慢速通道填充，或删除字段 | 已验证 |
| P4.4 | 错误计数器 SOEM 只清零不读回 | 应用侧 FPRD 读 0x0300-0x030F | 已验证 |
| P4.5 | SOEM 错误环无互斥（RT push / 非 RT pop） | 加同步或换无锁结构 | 已验证 |

P4.1 调优结果（2026-09-12，limit=4 无统计显著影响，见文档正文）。

P4.3 判定：由于 `PdoConfig="false"` 硬件约束，电流/电压/温度只能走 SDO 慢速通道
（约 3ms 往返，需要轮询线程）。若当前阶段不需要这三个量做保护决策，可保留字段但标注
"SDO 通道未实现，值为 0"，推迟到慢速监控专项。**不阻塞 CSP 交付。**

---

**P4.3**（2026-09-12，电流通过 SDO 慢速通道填充）：

问题：6078h (actual_current) 和 6079h (dc_link_voltage) 未映射进 TxPDO（ESI Fixed="true"），
报告中 `actual_current` / `dc_link_voltage` 恒为 0。

解决方案：
- observer_thread 以 ~50ms 周期通过 SDO 读取 6078h，存储到 `sdo_current_6078h`
- session_control 每周期从 observer 读取该值并填充到 `actual_current`（加锁保护）
- 验证结果：报告中 actual_current = -28/10（双轴，不再为 0）

dc_link_voltage 和温度字段未实现，保持为 0（低优先级监控数据）。

### P5 模式与能力扩展（L3）

仅 CSP 路径，P5.1 和 P5.2 可以暂缓。

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P5.1 | 运行期模式切换未实现（模式在计划构建期固定） | 0x119800 方案下 OP 态切换 CSP/CSV/CST | 未做（CSP 专项不要求） |
| P5.2 | ~~外部目标路径硬要求 mode==8（`session_target.c:41-45`）~~ | 三模式均可接收对应目标 | 已验证 |
| P5.3 | `fixed_csv` 的 1A02 内容 ESI 与手册冲突，未验证 | 真机读回确认 | 硬件阻塞（CSP 专项不要求） |
| P5.4 | 无 3 轴及以上时序数据 | 逐步扩到目标轴数，每档留报告 | 硬件阻塞 |

---

### P6 接口成形（L5）——外部控制器可对接的门槛

**前置条件已满足**：P1（实时基座）和 P2（安全门禁）均完成，P1.4/P1.6 提供了实测抖动。

L5 当前实现：Unix 域套接字（`/tmp/emaster-<deployment-id>.sock`），文本命令协议，
单客户端，命令队列深度 16，5 秒客户端空闲超时。已实现命令：
`set_external_target`、`status`、`topology`、`stop`、`quick_stop`、`halt`、
`fault_reset`、`shutdown`、`switch`。

以下四项是外部控制器能安全、正确对接主站的最低要求。**全部完成前，L6 对接是不安全的。**

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P6.1 | 无协议规格文档：外部控制器不知道单位、轴序、状态码语义 | 一份协议文档覆盖：位置单位（原始 encoder counts）、轴序规则、`status` 各字段含义、响应格式 | 未做 |
| P6.2 | **主站就绪信号缺失**：socket 在 RT 初始化和 OP 状态之前出现，早连接的客户端可能在第一个控制周期就注入目标，导致从静止位置的跳变 | 主站进入 OP 态后再接受 `set_external_target`；或在状态响应中暴露"is_op"字段，客户端等待后再发目标 | 未做 |
| P6.3 | 跟随误差限幅硬编码为临时测试值（`main.c:365` 256000 counts = 200°，注释标"临时放宽"） | 该值从部署配置或运动方案读取，且值为合理生产值（建议 ≤5°） | 未做 |
| P6.4 | 单客户端限制和重连语义未文档化：第二个客户端 `connect` 会被挂起或拒绝，5 秒超时后连接才能复用 | 文档说明单客户端约束、超时时间、重连方法，并在协议文档中体现 | 未做 |

**P6.2 的风险细节**：`emaster_command_server_create` 在 `main.c:343` 调用，`install_rt_primitives`
在 `main.c:349`，而实际进入 OP 态发生在 `emaster_soem_control_session` 内部的从站状态机流程完成后。
这段窗口期（数百毫秒到数秒）内，socket 已存在但主站不在 OP 态。若外部控制器在此期间
发送 `set_external_target`，命令会入队。进入 OP 态的第一个周期调用 `position_target_source`，
`initialized=0` 先记录 `initial_positions`，然后立即读取缓冲区中已有的目标值并输出。
若目标值与初始位置相差较大，第一周期即命令一次大幅跳变。步进限幅
（`max_step_counts = 256000`，即 200°）目前等同于无限幅，无法阻止此路径。

---

### P7 收尾与交付门槛

P6 全部完成后，工程应用可对接。交付前还需确认：

| # | 问题 | 完成判据 | 状态 |
| --- | --- | --- | --- |
| P7.1 | `frame_timeout_us` 修复（P1.6）已在 orangepi，未 commit 到本地仓库 | commit 到 main，含说明性 commit message | 未做 |
| P7.2 | `功能要求.md` 状态与代码实况不同步 | P2 CSV/CST 描述修正；P4 状态查询打勾；P1 持续运行验证打勾；添加"已知代码缺陷"节（P4.3 SDO 未实现、P2.5 Fault Reset 未真机验证） | 未做 |
| P7.3 | 本地 main 落后 origin/main 18 个 commit | git sync | 未做 |

---

## 3 立即可动与被阻塞的

**台架可达**（2026-09-12 确认）。

**当前最高优先级（按顺序）**：
1. P6.2 主站就绪信号——安全缺口，不修则外部对接不安全
2. P6.3 限幅值回退——临时测试值流入生产路径
3. P6.1 协议文档——无文档则外部控制器无从对接
4. P6.4 单客户端约束文档化
5. P7.1 frame_timeout_us commit

**硬件阻塞项**：P5.3（`fixed_csv` ESI 验证）、P5.4（3 轴及以上时序）、
P2.5 的触发测试（需先解决驱动器静置不响应问题，见 [[drive-idle-no-response]]）。

---

## 4 与既有文档的关系

`phase-1.md` 的自述状态不可采信（见基线 4.5）。以本文的 P 编号为进度口径，
phase-1.md 降为历史记录。

