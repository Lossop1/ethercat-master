# 目标通道：多量下发（设计基线，未实现）

状态：**设计已定，代码未动**。本文是修正基础，不是现状说明。

## 1 修正要点

把"多量同时下发"从 MIT 专属提升为**通用能力**：

- 通道永远是多量的，**写哪些量由设备档声明的集合决定**，不由模式号、不由型号决定。
- LZ12028 不需要 MIT 也能用多量：它的 RPDO 里 `0x607A`/`0x60FF`/`0x6071` 本来就都在，声明几个就写几个。
- MIT 只是"声明了五个量"的一个例子（`0x607A`+`0x60FF`+`0x6071`+`0x2013:1`+`0x2013:2`）。

因此不再有"先单量、后多量"两轮：**回调接口一次就定成多量**，避免两次 ABI 改动。

## 2 数据模型

`operation_mode` 现有字段：`quantity`（单值：位置/速度/力矩/无）、`required_rx_fields`（名字）。
新增声明：

| 字段 | 含义 |
| --- | --- |
| `target_fields[]` | 本模式接受的量集合，逐项：`name`（`position`/`velocity`/`torque`/`mit_kp`/`mit_kd`）、`role`（`primary` 主量 / `feedforward` 前馈量）、`refresh`（`every_cycle` 每拍刷新 / `on_change` 变化时写） |
| `quantity` | 降级为"主量提示"，只用于反馈选择与日志，不再决定写哪个字段；保留以兼容既有配置 |

约束：

- 一个模式**最多一个 `primary`**；没有 `primary` 的模式不得声明周期目标（等价于今天的 `quantity == none`，回零走这条）。
- 字段名必须能在该轴 RxPDO 布局里找到，找不到就是"方案与设备档不符"，plan 层拒绝启动——复用现有 `declared_entry_named()` / `required_rx_fields` 机制。
- **索引一律从 PDO 映射解析**（`field_ordinal_for`），不写死：良质 `0x2013:1/2` 与 MT `0x2000/0x2001` 的差异由名字别名吸收。

## 3 通道与输出

| 环节 | 现状 | 改成 |
| --- | --- | --- |
| 回调 | `position_target_source(... int32_t *target_positions ...)`，每轴一个标量 | 新回调 `target_set_source(... emaster_axis_target_set_t *targets ...)`，每轴携带 q/dq/tau/kp/kd + `present_mask` |
| 写入 | `emaster_soem_axis_set_target_value(plan, axis, value)` 按 `quantity` 写一个 | `emaster_soem_axis_set_targets(plan, axis, const emaster_axis_target_set_t *)` 按 `target_fields` 逐项写 |
| 生效判据 | 位置偏移判据对一切模式执行 | 只在声明含 `position` 且为主量时执行；其余量不做幅度判据（力矩上限已按 `runtime_config.h:283-284` 有意删除，不复活） |
| 未声明的量 | — | **不写、不静默**：计入报告的"未声明量"列，客户端能看出自己发的东西没生效 |

## 4 客户端送来的量与声明的关系

- 记录里的 `mode` 只用于**核对**（现在就是这样，`MODE_MISMATCH` 拒收），不决定写什么。
- 记录里的五个量按**名字**匹配 `target_fields`：匹配上就写，匹配不上就记"未声明"。
- CSP 部署发 `dq`：忽略并计数，不再出现"`q` 被当成速度写进 `0x60FF`"这种串味（这是当前实现的缺陷，见第 6 节）。

## 5 改动清单

1. `include/emaster/config/runtime_config.h`：`emaster_operation_mode_t` 增 `target_fields[]`/`role`/`refresh`。
2. `tools/gen/`：从设备档 PDO 映射 + ESI 字段名生成 `target_fields`（`0x607A`→position、`0x60FF`→velocity、`0x6071`→torque、`MitKp/PVT_KP`→mit_kp）；不含任何型号分支。
3. `include/emaster/bus/control_session.h`：新增回调 typedef 与 `emaster_axis_target_set_t`；旧回调保留一轮并标废弃。
4. `src/bus/session/session_target.c`：按声明逐项写；位置判据按声明门控；"整条一起写、失败不提交"的语义不变。
5. `src/cia402/process_image.c`：`configured_quantity` 单量分支改为按声明逐项填 `rx_values`；保留单量路径作为退化情形。
6. `apps/master/main.c`：回调按声明从缓冲区取量；超时/失联的置零策略按 `refresh` 决定（速度/力矩模式仍必须写 0，见现有 `external_target_zero_on_loss` 的理由）。
7. `src/audit/report_sections.c`：每轴"生效量集合 + 值 + 未声明量计数 + 拒收原因"。
8. 契约与注释：`docs/protocol/target-channel-contract.md`、`include/emaster/setpoint/wire.h:69`、`README.md` 的"主站只留一条判据"限定为"位置主量"。

## 6 当前实现的缺陷（本次一并修）

`apps/master/main.c` 每拍无条件取 `buf->positions[i]`，而 `control_session.c` 按 `quantity` 把它写进位置/速度/力矩。于是：

- CSV 部署（客户端须发 mode=9）送来的 **`q` 被当成目标速度**写进 `0x60FF`，真正的 `dq` 没人读；
- CST 同理，`q` 被当成力矩。

这是"单量通道 + 多量记录"的必然产物，改多量后自然消失，但要有一条检查盯住它不再回来。

## 7 验收

| 场景 | 判据 |
| --- | --- |
| LZ12028 CSP，声明 `{position}` | 行为与今天一致；`dq`/`tau` 忽略并计入"未声明" |
| LZ12028 CSP，声明 `{position, velocity}`（设备支持前馈时） | `0x607A` 与 `0x60FF` 同帧写入；报告记两量生效 |
| LZ12028 CST，声明 `{torque}` | 只有 `0x6071` 被写；越 INT16 拒收 |
| MT X436 MIT，声明五个 | 五量同帧写入；`PVT_KP/PVT_KD` 走名字别名 |
| LZ12028（无 MIT） | 绝不写 `0x2013`；报告不出现 kp/kd |
| 任一模式发未声明量 | 不写、计数、报告可读；**不得静默当成别的量** |

检查脚本：`tests/checks/setpoint_channel_check.py` 每种声明加一例；`tests/checks/mode_gate_check.py` 加"未声明量不得生效"。

## 8 不做

- 不恢复力矩/速度幅度上限（属客户端与驱动器职责）。
- 不在主站内做规划或插补。
- 不按型号或模式号分支；换型号只换 ESI + 设备档，主站不重编。
