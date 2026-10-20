# 错误分类、编号与下一步

每一轮会话结束，报告和终端都会给出一条错误分类。分类回答"这一轮到底出了什么事"，
不是"哪一步失败了"；后者看 `status_code` 与 `first_cycle_failure`。

报告字段：

| 字段 | 内容 |
| --- | --- |
| `error_class` | 稳定 ASCII 键，分析脚本读它 |
| `error_class_text` | 中文名，给人读 |
| `error_class_code` | 稳定数字编号，工单用（"E130"） |
| `error_class_next_step` | 一句可执行的排查方向 |
| `error_class_evidence_*` | 判定证据：整帧缺失、同步证据、第一根不应答轴、第一根同步轴 |

终端在汇总行后印同样的分类与编号，只有真出故障时才多印一行"下一步"。

**编号一经发布不再改变，也不复用。** 新增类别取同段的下一号。1xx 配置与总线，
2xx 驱动器，3xx 运动，4xx 停机，5xx 审计，9xx 内部。

## 类别表

| 编号 | 键 | 含义 | 下一步 |
| --- | --- | --- | --- |
| E0 | `UNCLASSIFIED` | 未分类（规则没接住，属缺陷） | 保留报告与运行配置并上报：这是主站的分类缺口，不是设备问题。 |
| E1 | `NONE` | 无故障 | 无需处理。 |
| E100 | `CONFIG` | 调用参数非法 | 核对部署文件与命令行参数，按报错点名的字段改；改完重新 build。 |
| E110 | `BUS_INTERFACE` | 网卡/套接字不可用 | 核对网卡名、接口是否启用、进程权限（需 root 或 CAP_NET_RAW）。 |
| E111 | `BUS_NO_SLAVES` | 总线上没有从站 | 查从站供电与网线，先跑 `emaster-scan` 确认能看到从站。 |
| E112 | `BUS_TOPOLOGY` | 拓扑或身份不符 | 核对拓扑配置与实机站序、vendor/product/revision、序列号；换过设备后重新扫描并生成。 |
| E113 | `BUS_CONFIG` | 从站配置失败（PDO/映像/SDO） | 按报告里的逐轴 PDO 差异与 SDO 失败项改配置；确认设备固件与设备档一致。 |
| E114 | `BUS_STARTUP_STATE` | 从站没进到目标状态 | 看逐轴 AL 状态与错误码，确认从站在时限内进 SAFE-OP/OP；必要时 reset。 |
| E115 | `BUS_DC_CONFIG` | DC/SYNC0 配置失败 | 核对运行方案的 DC 策略、参考从站与 AssignActivate；确认参考从站支持 DC。 |
| E120 | `BUS_DC_SYNC` | 运行期 DC 同步失败 | 核对周期、DC 相位与参考从站；对比报告里的逐轴 timing。 |
| E121 | `BUS_CYCLE_TIMING` | 周期等待失败或截止错失 | 查主机实时性与 CPU 占用、周期线程上的阻塞操作，以及周期是否小于链路耗时。 |
| E130 | `BUS_RING_TAIL_LOST` | 环尾丢失/物理层：整帧缺失且轴不应答 | 查报告里不应答的轴号对应的线缆、接头与供电；重点看环尾最后一站。 |
| E131 | `BUS_LINK_DROPPED` | 整帧缺失（各站仍应答） | 查全环线缆与电磁干扰；看丢失链计数与网卡错误计数是否同时增长。 |
| E132 | `BUS_SYNCHRONIZATION` | 同步错误：轴报同步类 AL 码或 SYNC0 连续迟到 | 核对 SYNC0 周期/相位与从站 DC 设置；看报告里的同步轴号与连续迟到峰值。 |
| E133 | `BUS_SHORT_FRAME` | 短帧：计数变短且无同步/应答证据 | 查对应站点的 PDO 配置与固件版本；看计数窗口最大值出现在哪一拍。 |
| E200 | `DRIVE_FAULT` | 驱动器自报故障 | 读逐轴 603F 与厂商故障码，按驱动器手册处理；确认原因消除后再 fault_reset。 |
| E201 | `DRIVE_FEEDBACK` | 反馈或控制器失败 | 查编码器/反馈接线与驱动器诊断，按驱动器手册处理。 |
| E202 | `DRIVE_LIMIT` | 驱动器内部限位或限幅生效 | 核对目标与限幅（额定力矩、速度、软限位）设置，确认是否触到限位。 |
| E300 | `MOTION` | 运动层：目标非法或跟随误差 | 核对目标是否超偏移上限、换算与跟随误差窗；回零看方法、速度与完成时限。 |
| E400 | `SHUTDOWN` | 停机阶段失败 | 查停机阶段的总线与驱动器状态，确认安全停与恢复 INIT 卡在哪一步。 |
| E401 | `SHUTDOWN_UNCONFIRMED` | 停机未确认：轴已离开运行使能，未满足停机判据 | 核对停机判据的掩码与值是否与驱动器实际状态字一致；先读一次末拍状态字。 |
| E500 | `AUDIT` | 审计层失败 | 保留报告并检查磁盘空间与报告目录权限；反复出现时上报。 |
| E900 | `INTERNAL` | 内部失败（内存等） | 保留报告与运行配置并上报；内部失败不在现场修。 |

## 判读顺序

分类只给一个结论，证据字段给出它凭什么。三条通信类最容易混：

1. **整帧缺失**（`wkc_no_frame_count > 0`）优先于同步错误。帧没回来时，这一拍的同步错
   多半是后果。
2. 有轴完全不应答（AL state=0，且 `al_read` 为真）时，整帧缺失判为
   `BUS_RING_TAIL_LOST`；所有站都还应答时判为 `BUS_LINK_DROPPED`。
3. 没有整帧缺失，但有轴报同步类 AL 码，或 SYNC0 连续迟到 ≥ 2 拍，判为
   `BUS_SYNCHRONIZATION`。

`error_class_evidence_sync` 与命中的类别独立填写：一次"整帧缺失 + 同步码"的混合事故
仍能看到同步证据为真。

## 与其他字段的关系

- `status_code`：控制流结论，枚举值已冻结在归档报告里，只能追加。
- `first_cycle_failure` / `first_runtime_failure`：第一次失败的那一拍，含轴号与状态字。
- 逐轴 `drive_error_code_decoded` / `drive_error_code_origin` / `drive_error_code_text`：
  该轴选用的驱动器故障码与来源，与整轮分类分开。
- 回零失败是 `MOTION`；具体看逐轴 `homing_error`、`homing_attained` 与 `homing_timeout_ms`。
