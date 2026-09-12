# DC 时序统计解读指南

本文档说明 EtherCAT 分布式时钟（DC）的时序统计数据，用于评估周期性能和诊断实时性问题。

## 概述

DC 时序统计记录每个周期的关键时间点，包括：
- 周期启动时间
- 帧发送/接收延迟
- 从站处理时间
- 应用处理时间

这些数据以直方图形式汇总，便于识别抖动、延迟和异常模式。

---

## 时序模型

### 周期结构

```
Cycle N                           Cycle N+1
|                                 |
├─ T0: Cycle start                ├─ T0: Next cycle start
├─ T1: Frame send                 :
├─ T2: Frame return               :
├─ T3: Application finish         :
└─ Wait for next SYNC0            :
   (sleep until T0 + cycle_time)  :
```

### 关键时间点

1. **T0: SYNC0 触发**
   - DC 参考时钟触发周期开始
   - 理论上每个周期精确对齐

2. **T1: 帧发送**
   - 主站发送 EtherCAT 帧
   - 延迟 = T1 - T0（应用准备时间 + 系统调度延迟）

3. **T2: 帧返回**
   - 从站处理完成，帧返回主站
   - 延迟 = T2 - T1（传播延迟 + 从站处理时间）

4. **T3: 应用完成**
   - 主站解码 PDO、更新控制逻辑、编码下周期输出
   - 延迟 = T3 - T2（应用处理时间）

---

## 直方图字段说明

### 1. system_time_offset_ns

**含义**：系统时钟与 DC 参考时钟的偏差

**计算**：`system_time - dc_time`

**单位**：纳秒（ns）

**典型值**：
- 稳定运行：偏差变化 < 1000 ns
- 初次启动：可能有较大初始偏差（数毫秒）

**直方图示例**：
```json
"system_time_offset_ns": {
  "bucket_width": 1000,
  "buckets": [
    {"lower": 5000, "upper": 6000, "count": 8520},
    {"lower": 6000, "upper": 7000, "count": 1480}
  ]
}
```

**解读**：
- 集中在单个桶：时钟同步良好
- 分散在多个桶：系统时钟抖动严重
- 持续漂移：NTP 或系统负载问题

---

### 2. frame_send_latency_ns

**含义**：从周期启动到帧发送的延迟

**计算**：`T1 - T0`

**单位**：纳秒（ns）

**典型值**：
- 理想：< 10 µs
- 可接受：< 50 µs
- 警告：> 100 µs

**直方图示例**：
```json
"frame_send_latency_ns": {
  "bucket_width": 5000,
  "buckets": [
    {"lower": 10000, "upper": 15000, "count": 9800},
    {"lower": 15000, "upper": 20000, "count": 200}
  ]
}
```

**解读**：
- 大部分 < 15 µs：正常
- 偶尔 > 50 µs：系统调度抖动
- 频繁 > 100 µs：CPU 负载过高或实时性配置不当

**影响因素**：
- CPU 调度优先级（使用 `chrt` 设置实时优先级）
- 系统负载（其他进程占用 CPU）
- 中断处理（网络中断、定时器）

---

### 3. frame_return_latency_ns

**含义**：从帧发送到帧返回的延迟

**计算**：`T2 - T1`

**单位**：纳秒（ns）

**组成**：
- 网络传播延迟（取决于总线长度和从站数量）
- 从站处理时间（PDO 复制、状态机）

**典型值**：
- 单从站：5 ~ 20 µs
- 双从站：10 ~ 40 µs
- 多从站（5+）：30 ~ 100 µs

**直方图示例**：
```json
"frame_return_latency_ns": {
  "bucket_width": 5000,
  "buckets": [
    {"lower": 25000, "upper": 30000, "count": 9950},
    {"lower": 30000, "upper": 35000, "count": 50}
  ]
}
```

**解读**：
- 集中分布：从站处理稳定
- 偶尔跳跃：从站内部处理抖动
- 持续增加：总线拓扑变化或从站故障

**影响因素**：
- 从站数量和拓扑
- 电缆长度
- 从站内部负载（复杂控制算法）

---

### 4. app_latency_ns

**含义**：主站应用处理时间

**计算**：`T3 - T2`

**单位**：纳秒（ns）

**组成**：
- PDO 解码
- 控制逻辑（CiA 402 状态机、运动规划）
- PDO 编码
- 监控数据更新

**典型值**：
- 简单应用（位置保持）：< 10 µs
- 复杂应用（多轴插补）：20 ~ 50 µs
- 警告：> 100 µs

**直方图示例**：
```json
"app_latency_ns": {
  "bucket_width": 5000,
  "buckets": [
    {"lower": 15000, "upper": 20000, "count": 9000},
    {"lower": 20000, "upper": 25000, "count": 900},
    {"lower": 25000, "upper": 30000, "count": 100}
  ]
}
```

**解读**：
- 窄分布：应用逻辑稳定
- 宽分布：算法复杂度波动（条件分支）
- 长尾：偶尔触发重计算（轨迹规划）

**优化建议**：
- 预计算：将复杂运算移到非周期线程
- 查表：用查表替代三角函数等
- 缓存：避免重复计算

---

### 5. total_cycle_latency_ns

**含义**：周期总延迟

**计算**：`T3 - T0 = frame_send + frame_return + app`

**单位**：纳秒（ns）

**典型值**：
- 1ms 周期：总延迟应 < 500 µs（留有 50% 裕量）
- 警告：> 700 µs
- 危险：> 900 µs（接近周期时间）

**直方图示例**：
```json
"total_cycle_latency_ns": {
  "bucket_width": 10000,
  "buckets": [
    {"lower": 50000, "upper": 60000, "count": 9500},
    {"lower": 60000, "upper": 70000, "count": 450},
    {"lower": 70000, "upper": 80000, "count": 50}
  ]
}
```

**解读**：
- < 50% 周期时间：健康
- 50% ~ 70%：可接受
- > 70%：裕量不足，容易丢周期
- > 90%：危险，需要优化

---

## 直方图分析方法

### 1. 集中度分析

**单峰集中**：
```json
"buckets": [
  {"lower": 10000, "upper": 15000, "count": 9900},
  {"lower": 15000, "upper": 20000, "count": 100}
]
```
- 99% 数据集中在 10~15 µs
- 系统稳定，抖动小

**双峰分布**：
```json
"buckets": [
  {"lower": 10000, "upper": 15000, "count": 7000},
  {"lower": 15000, "upper": 20000, "count": 100},
  {"lower": 20000, "upper": 25000, "count": 2900}
]
```
- 两种工作模式（轻载/重载）
- 可能有条件分支导致不同路径

**长尾**：
```json
"buckets": [
  {"lower": 10000, "upper": 15000, "count": 9000},
  {"lower": 15000, "upper": 20000, "count": 500},
  {"lower": 20000, "upper": 25000, "count": 300},
  {"lower": 25000, "upper": 30000, "count": 150},
  {"lower": 30000, "upper": 35000, "count": 50}
]
```
- 偶尔出现异常延迟
- 系统调度抢占或中断

### 2. 百分位计算

从直方图计算百分位：

```python
def calculate_percentile(buckets, total_count, percentile):
    target = total_count * percentile / 100
    cumulative = 0
    for bucket in buckets:
        cumulative += bucket['count']
        if cumulative >= target:
            # 线性插值
            ratio = (cumulative - target) / bucket['count']
            return bucket['upper'] - ratio * (bucket['upper'] - bucket['lower'])
    return buckets[-1]['upper']

# 示例
p50 = calculate_percentile(buckets, total, 50)  # 中位数
p95 = calculate_percentile(buckets, total, 95)  # 95 百分位
p99 = calculate_percentile(buckets, total, 99)  # 99 百分位
```

**评估标准**：
- P50 < 30% 周期时间：优秀
- P95 < 50% 周期时间：良好
- P99 < 70% 周期时间：可接受
- P99 > 80% 周期时间：需要优化

---

## 故障诊断

### 1. 周期丢失（Cycle Overrun）

**现象**：
- `total_cycle_latency_ns` 接近或超过周期时间（1ms = 1,000,000 ns）
- 报告中出现 `wkc_error` 或周期计数跳变

**排查**：
1. 检查 `frame_send_latency_ns`：是否有大延迟峰值
2. 检查 `app_latency_ns`：应用处理是否过慢
3. 系统负载：`top` 查看 CPU 占用
4. 实时优先级：`chrt -p <pid>` 确认周期线程优先级

**解决方案**：
- 降低周期频率（1ms → 2ms）
- 优化应用逻辑
- 减少从站数量或简化拓扑
- 调整实时优先级（`chrt -f 90 <command>`）

### 2. 抖动（Jitter）

**现象**：
- 直方图分散在多个桶
- `frame_send_latency_ns` 变化范围 > 20 µs

**排查**：
1. 系统中断：`cat /proc/interrupts`，检查中断频率
2. CPU 频率调整：`cpufreq-info`，确认性能模式
3. 电源管理：禁用 CPU 睿频和休眠

**解决方案**：
```bash
# 设置性能模式
sudo cpupower frequency-set -g performance

# 禁用 CPU 休眠状态
sudo cpupower idle-set -D 0

# 隔离 CPU 核心（isolcpus）
# 编辑 /boot/cmdline.txt 或 grub 配置
# 添加: isolcpus=1,2,3
```

### 3. 从站响应慢

**现象**：
- `frame_return_latency_ns` 异常高
- 单个从站延迟远高于其他从站

**排查**：
1. 从站诊断：检查从站 AL 状态和错误计数器
2. 电缆质量：更换或检查屏蔽
3. 从站负载：检查从站内部应用是否过载

### 4. 应用处理慢

**现象**：
- `app_latency_ns` 持续 > 50 µs
- 直方图有长尾

**排查**：
1. 代码分析：使用 `perf` 或 `gprof` 分析热点
2. 算法复杂度：检查循环和递归
3. 内存访问：减少缓存未命中

**优化示例**：
```c
// 慢：每周期计算三角函数
for (i = 0; i < axis_count; i++) {
    target[i] = amplitude * sin(2 * PI * cycle / period);
}

// 快：预计算查表
static int32_t sine_table[1000];
// 初始化时生成查表
for (i = 0; i < axis_count; i++) {
    target[i] = sine_table[cycle % 1000];
}
```

---

## 性能目标

### 推荐值（1ms 周期）

| 指标 | 目标 | 警告 | 危险 |
|------|------|------|------|
| frame_send_latency | < 20 µs | < 50 µs | > 100 µs |
| frame_return_latency | < 40 µs | < 80 µs | > 150 µs |
| app_latency | < 30 µs | < 60 µs | > 100 µs |
| total_cycle_latency | < 300 µs | < 500 µs | > 700 µs |
| P99 total_latency | < 500 µs | < 700 µs | > 900 µs |

### 调整周期时间

如果无法满足 1ms 周期要求：

| 周期时间 | 推荐应用 | 最大从站数 |
|---------|---------|-----------|
| 500 µs | 高速响应（< 5 轴） | 3 |
| 1 ms | 标准运动控制 | 8 |
| 2 ms | 多轴协调 | 16 |
| 4 ms | 低速定位 | 32+ |

---

## 数据访问

### 报告位置

`runtime/reports/<deployment>-latest.json`

### JSON 结构

```json
{
  "axes": [
    {
      "timing": {
        "system_time_offset_ns": {
          "bucket_width": 1000,
          "buckets": [...]
        },
        "frame_send_latency_ns": {...},
        "frame_return_latency_ns": {...},
        "app_latency_ns": {...},
        "total_cycle_latency_ns": {...}
      }
    }
  ]
}
```

### Python 分析脚本

```python
import json
import numpy as np

def histogram_to_array(hist):
    """将直方图还原为数值数组（近似）"""
    values = []
    for bucket in hist['buckets']:
        mid = (bucket['lower'] + bucket['upper']) / 2
        values.extend([mid] * bucket['count'])
    return np.array(values)

def analyze_timing(report_path):
    with open(report_path) as f:
        report = json.load(f)
    
    for i, axis in enumerate(report['axes']):
        timing = axis['timing']
        
        print(f"轴 {i} 时序分析:")
        
        for metric, hist in timing.items():
            data = histogram_to_array(hist)
            
            print(f"  {metric}:")
            print(f"    中位数: {np.median(data)/1000:.1f} µs")
            print(f"    P95: {np.percentile(data, 95)/1000:.1f} µs")
            print(f"    P99: {np.percentile(data, 99)/1000:.1f} µs")
            print(f"    最大值: {np.max(data)/1000:.1f} µs")
            print()

analyze_timing('runtime/reports/orangepi-dual-bench-latest.json')
```

---

## 参考资料

- EtherCAT 技术规范：IEC 61158
- 分布式时钟：ETG.1020
- 实时 Linux 配置：PREEMPT_RT 补丁文档
