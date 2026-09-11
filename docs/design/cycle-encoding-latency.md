# 周期编码延迟契约

## 1 问题

编码在发送之后 ⇒ 固有 1 周期延迟

## 2 实际顺序

标准 EtherCAT 周期模式（`session_exchange.c:94,99` + `session_control.c:384`）：

```
周期 N:
  1. ecx_send_processdata()      发送周期 N-1 编码的 PDO
  2. ecx_receive_processdata()   接收周期 N 的反馈
  3. 解码反馈 → axis->actual_position
  4. 计算新目标（motion/external target）
  5. emaster_soem_axis_set_target_value() → axis->target_position
  6. emaster_cia_process_image_update_output() 编码进 PDO 缓冲区

周期 N+1:
  1. ecx_send_processdata()      发送周期 N 编码的 PDO
```

## 3 延迟来源

**不是实现缺陷**，是 EtherCAT 协议特性：

- 主站在周期开始发送帧，帧携带上一周期的输出
- 主站在同一周期接收帧，帧携带本周期的输入
- 主站根据本周期输入计算输出，但该输出要到下一周期才发送

这是所有 EtherCAT 主站的固有延迟（包括 TwinCAT、Acontis、IGH）。

## 4 影响

- **位置控制（CSP）**：目标位置晚 1ms 到达驱动器
  - 1kHz 周期下，1ms = 1 周期延迟
  - 对跟随误差预算的影响已在 P1.4 基线数据中体现
  
- **速度控制（CSV）**：目标速度晚 1ms 到达
  
- **力矩控制（CST）**：目标力矩晚 1ms 到达

## 5 可能的改进

**同周期编码**（编码在发送之前）：

```
周期 N:
  1. 计算新目标（基于周期 N-1 的反馈）
  2. 编码进 PDO
  3. ecx_send_processdata()      发送周期 N 编码的 PDO
  4. ecx_receive_processdata()   接收周期 N 的反馈
```

**代价**：
- 目标计算基于旧反馈（N-1 而非 N），对快速变化的外部目标可能增大跟随误差
- 反馈到编码的响应延迟从 1 周期变为 2 周期
- 与标准 EtherCAT 模式不一致，增加维护负担

## 6 决策

**当前实现维持标准 EtherCAT 模式**，理由：

1. 1ms 延迟在 P1.4 基线数据中已验证可接受（跟随误差 < 100 counts）
2. 标准模式与其他 EtherCAT 主站一致，便于参照和问题排查
3. 同周期编码的跟随误差改善需要实测验证，不应提前优化

**P3.2 完成判据**：明确记录为契约 ← 本文档

## 7 后续

如果 P1.5 实测显示周期抖动超过预期，需要重新评估同周期编码的价值。
