# 监控扩展指南

本文档说明 EtherCAT 主站的监控功能，包括实时数据采集、增益参数读取和故障诊断。

## 概述

监控系统分为两个通道：

1. **PDO 周期通道**（1ms 周期）
   - 位置、速度、转矩、状态字
   - 实时性高，用于运动控制

2. **SDO 观测通道**（~50ms 周期）
   - 电流、电压、温度、速度
   - 非实时，用于状态监控和故障诊断

所有监控数据最终记录到运行报告的 JSON 文件中。

---

## 监控字段说明

### 1. 电流监控（6078h）

**对象**：Actual Current（实际电流）  
**数据类型**：int16  
**单位**：毫安（mA）  
**读取方式**：SDO 观测线程

**含义**：
- 电机当前实际电流值
- 正值：电机输出转矩
- 负值：再生制动或反向驱动

**典型值**：
- 静止保持：-50 ~ +50 mA
- 正常运动：100 ~ 3000 mA
- 峰值电流：取决于电机额定值（参考 6075h）

**异常判断**：
- 电流持续超过额定值 150%：过载
- 电流波动剧烈：机械共振或控制环不稳定
- 静止时电流异常高：位置环增益过大或机械卡死

---

### 2. 母线电压（6079h）

**对象**：DC Link Voltage  
**数据类型**：uint32  
**单位**：毫伏（mV）  
**读取方式**：SDO 观测线程

**含义**：
- 驱动器直流母线电压
- 反映供电状态和能量回馈

**典型值**：
- 48V 系统：46000 ~ 52000 mV（46V ~ 52V）
- 24V 系统：22000 ~ 28000 mV（22V ~ 28V）

**异常判断**：
- 低于额定电压 85%：欠压，电机性能下降
- 高于额定电压 115%：过压，可能损坏驱动器
- 快速下降：供电中断或电源容量不足
- 快速上升：再生制动时能量回馈过快

---

### 3. 温度监控（200Bh:01h, 200Bh:02h）

#### 3.1 MOSFET 温度（200Bh:01h）

**数据类型**：int16  
**单位**：0.1°C（值 355 = 35.5°C）  
**读取方式**：SDO 观测线程

**含义**：驱动器功率 MOSFET 结温

**典型值**：
- 环境温度 + 5 ~ 15°C（轻载）
- 环境温度 + 20 ~ 40°C（重载）
- 最高允许：通常 85 ~ 100°C

**异常判断**：
- 超过 80°C：接近热保护阈值
- 快速上升：散热不良或过载
- 温差过大（多轴间）：负载不均或散热问题

#### 3.2 电机温度（200Bh:02h）

**数据类型**：int16  
**单位**：0.1°C  
**读取方式**：SDO 观测线程

**含义**：电机绕组或外壳温度（取决于传感器位置）

**典型值**：
- 环境温度 + 5 ~ 20°C（间歇运行）
- 环境温度 + 30 ~ 50°C（连续运行）
- 最高允许：通常 80 ~ 120°C（取决于电机等级）

**异常判断**：
- 超过 70°C：注意监控
- 超过 100°C：接近过热保护
- 持续高温：冷却不足、过载或环境温度过高

---

### 4. 速度监控（200Bh:08h, 200Bh:09h）

#### 4.1 实际电机速度（200Bh:08h）

**数据类型**：int32  
**单位**：RPM（转/分钟）  
**读取方式**：SDO 观测线程

**含义**：
- 电机实际转速（编码器反馈）
- 与 PDO 中的 606Ch（速度实际值）可能不同：
  - 606Ch：单位可能是用户单位或 increments/cycle
  - 200Bh:08h：直接以 RPM 表示

**典型值**：取决于应用
- 定位应用：0 ~ 500 RPM
- 高速应用：1000 ~ 6000 RPM

#### 4.2 速度指令（200Bh:09h）

**数据类型**：int32  
**单位**：RPM  
**读取方式**：SDO 观测线程

**含义**：
- 发送给速度环的目标速度
- 用于验证指令下发和速度跟随误差

**用途**：
- 对比 200Bh:08h 和 200Bh:09h 判断速度环性能
- 跟随误差 = 200Bh:09h - 200Bh:08h
- 持续大的跟随误差表明速度环增益不足或负载超限

---

### 5. 增益参数（2008h）

**读取时机**：SAFE-OP 阶段启动时  
**读取方式**：SDO 同步读取  
**数据类型**：uint16  
**单位**：0.01（值 2000 = 20.00）

#### 子索引说明

| 子索引 | 名称 | 含义 |
|--------|------|------|
| 01h | velocity_loop_kp | 速度环比例增益 |
| 02h | velocity_loop_ki | 速度环积分增益 |
| 03h | velocity_loop_kd | 速度环微分增益 |
| 04h | position_loop_kp | 位置环比例增益 |
| 05h | position_loop_ki | 位置环积分增益 |
| 06h | position_loop_kd | 位置环微分增益 |
| 07h | current_loop_kp | 电流环比例增益 |
| 08h | current_loop_ki | 电流环积分增益 |
| 09h | current_loop_kd | 电流环微分增益 |

#### 典型值（示例）

```
速度环: Kp=10 (0.1)  Ki=100 (1.0)  Kd=0
位置环: Kp=2000 (20.0)  Ki=0  Kd=0
电流环: Kp=0  Ki=0  Kd=0
```

**说明**：
- 电流环增益为 0：可能未启用或使用驱动器内部默认值
- 位置环 Ki=0：纯比例控制，无积分项（常见于高刚度应用）
- 速度环 Ki 较大：快速消除稳态误差

#### 增益参数用途

1. **设备识别**
   - Fingerprint 记录增益参数
   - 验证驱动器配置一致性

2. **故障诊断**
   - 振动：Kp 过高，Kd 不足
   - 超调：Kp 过高，Ki 过大
   - 跟随误差大：Kp 过小
   - 稳态误差：Ki 为 0 或过小

3. **运行验证**
   - 启动时输出增益参数到终端
   - 记录到报告 JSON 中
   - 多轴间对比验证配置

---

## 数据访问方式

### 1. 实时终端输出

启动主站后，前 3 次 SDO 读取会输出详细信息：

```
[P4.3] 轴0 第1次读取:
  6078h(电流) wkc=1 val=-29
  603Fh(错误) wkc=1 val=0x0000
  6079h(电压) wkc=1 val=48261 mV
  200Bh:01h(MOSFET温度) wkc=1 val=355 (35.5°C)
  200Bh:02h(电机温度) wkc=1 val=317 (31.7°C)
  200Bh:08h(电机速度) wkc=1 val=0 rpm
  200Bh:09h(速度指令) wkc=1 val=0 rpm
```

每 20 次读取（约 1 秒）输出汇总：

```
[P4.3] 轴0: 电流=13, 错误=0x0000, 电压=48242mV, MOSFET=35.5°C, 电机=31.7°C (读取次数=20)
```

### 2. 运行报告 JSON

位置：`runtime/reports/<deployment>-latest.json`

结构示例：

```json
{
  "axes": [
    {
      "runtime": {
        "actual_current": -29,
        "dc_link_voltage": 48261,
        "mosfet_temperature": 355,
        "motor_temperature": 317,
        "actual_velocity": 0,
        "target_velocity": 0,
        "gain_parameters_read": true,
        "velocity_loop_kp": 10,
        "velocity_loop_ki": 100,
        "velocity_loop_kd": 0,
        "position_loop_kp": 2000,
        "position_loop_ki": 0,
        "position_loop_kd": 0,
        "current_loop_kp": 0,
        "current_loop_ki": 0,
        "current_loop_kd": 0
      }
    }
  ]
}
```

### 3. Python 分析示例

```python
import json

# 读取报告
with open('runtime/reports/orangepi-dual-bench-latest.json') as f:
    report = json.load(f)

for i, axis in enumerate(report['axes']):
    rt = axis['runtime']
    print(f"轴 {i}:")
    print(f"  电流: {rt['actual_current']} mA")
    print(f"  电压: {rt['dc_link_voltage']/1000:.1f} V")
    print(f"  MOSFET: {rt['mosfet_temperature']/10:.1f} °C")
    print(f"  电机: {rt['motor_temperature']/10:.1f} °C")
    print(f"  速度: {rt['actual_velocity']} rpm")
    
    if rt['gain_parameters_read']:
        print(f"  位置环 Kp: {rt['position_loop_kp']/100:.2f}")
        print(f"  速度环 Kp: {rt['velocity_loop_kp']/100:.2f}")
        print(f"  速度环 Ki: {rt['velocity_loop_ki']/100:.2f}")
```

---

## 故障诊断流程

### 1. 电机不动或抖动

检查项：
1. `actual_current`：是否异常高（卡死）或波动剧烈（共振）
2. `position_loop_kp`：是否过高（振荡）或过低（无力）
3. `mosfet_temperature` / `motor_temperature`：是否过热保护

### 2. 速度跟随不良

检查项：
1. 对比 `actual_velocity` 和 `target_velocity`
2. 跟随误差 = target - actual
3. `velocity_loop_kp` / `velocity_loop_ki`：增益是否合适

### 3. 过热保护

检查项：
1. `mosfet_temperature` > 80°C 或 `motor_temperature` > 100°C
2. `actual_current`：是否持续过载
3. 散热条件：环境温度、通风情况

### 4. 电压异常

检查项：
1. `dc_link_voltage` 低于额定值 85%：供电不足
2. 电压快速上升：再生制动能量无法消耗
3. 电压波动：电源容量不足或电缆压降

---

## 性能优化建议

### 1. 增益调整

**位置环（CSP 模式）**：
- 从 Kp=10 开始，逐步增加直到出现轻微振荡
- 回退到振荡前 60-80% 的值
- 如有稳态误差，增加 Ki（从 0.1 开始）

**速度环（CSV 模式）**：
- Kp 控制响应速度
- Ki 消除稳态误差（典型值 0.5 ~ 2.0）
- Kd 抑制超调（通常为 0 或很小）

### 2. 负载平衡

多轴系统：
- 对比各轴 `actual_current`，差异应小于 30%
- 对比各轴温度，差异应小于 10°C
- 负载不均：检查机械对齐和摩擦

### 3. 热管理

- MOSFET 温度 > 60°C：考虑增加散热片或降低占空比
- 电机温度 > 60°C：检查通风或减少连续运行时间
- 环境温度 > 35°C：考虑主动冷却

---

## 附录：字段对应关系

| 报告字段 | EtherCAT 对象 | 数据类型 | 单位 | 通道 |
|---------|--------------|---------|------|------|
| actual_current | 6078h | int16 | mA | SDO |
| dc_link_voltage | 6079h | uint32 | mV | SDO |
| mosfet_temperature | 200Bh:01h | int16 | 0.1°C | SDO |
| motor_temperature | 200Bh:02h | int16 | 0.1°C | SDO |
| actual_velocity (SDO) | 200Bh:08h | int32 | rpm | SDO |
| target_velocity (SDO) | 200Bh:09h | int32 | rpm | SDO |
| velocity_loop_kp | 2008h:01h | uint16 | 0.01 | SDO (SAFE-OP) |
| velocity_loop_ki | 2008h:02h | uint16 | 0.01 | SDO (SAFE-OP) |
| velocity_loop_kd | 2008h:03h | uint16 | 0.01 | SDO (SAFE-OP) |
| position_loop_kp | 2008h:04h | uint16 | 0.01 | SDO (SAFE-OP) |
| position_loop_ki | 2008h:05h | uint16 | 0.01 | SDO (SAFE-OP) |
| position_loop_kd | 2008h:06h | uint16 | 0.01 | SDO (SAFE-OP) |
| current_loop_kp | 2008h:07h | uint16 | 0.01 | SDO (SAFE-OP) |
| current_loop_ki | 2008h:08h | uint16 | 0.01 | SDO (SAFE-OP) |
| current_loop_kd | 2008h:09h | uint16 | 0.01 | SDO (SAFE-OP) |

**注意**：actual_velocity 和 target_velocity 在报告中可能同时包含 PDO 数据（606Ch, 60FFh）和 SDO 数据（200Bh）。SDO 数据在 session_control.c 中覆盖 PDO 数据。
