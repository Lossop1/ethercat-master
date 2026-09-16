# EtherCAT Master

工业级 EtherCAT 主站实现，基于 SOEM，支持 CiA 402 驱动器控制。

## 快速开始

### 启动主站

```bash
sudo ~/ethercat-master/build/bench/tools/master/emaster-master --deployment orangepi-bench-dual &
```

### 监控运行状态

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_watch/emaster-watch 500 --deployment orangepi-bench-dual
```

### 角度控制（推荐）

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_move_deg/emaster-move-deg --deployment orangepi-bench-dual
```

输入目标位置（单位：角度）：
```
角度> 45.5 120.0
```

### 脉冲控制（底层接口）

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_move/emaster-move --deployment orangepi-bench-dual
```

输入目标位置（单位：脉冲）：
```
位置> 60000 500000
```

## 单位换算（当前系统）

- 编码器分辨率：16384 脉冲/转（电机侧）
- 齿轮比：28:1
- 负载侧分辨率：458752 脉冲/转
- **1° = 1274 脉冲**

**推荐使用角度控制工具** (`emaster-move-deg`)，自动完成换算。

脉冲控制示例（如需底层接口）：
- 移动 45° → 输入 `57330`
- 移动 90° → 输入 `114660`

## 文档

- **[使用指南](docs/USAGE_GUIDE.md)** - 完整的启动、监控、控制说明
- **[电机参数](docs/MOTOR_PARAMETERS.md)** - 编码器、齿轮比、单位换算
- **[拓扑重构](docs/TOPOLOGY_REFACTOR.md)** - 架构设计和实现细节

## 主要特性

### 动态拓扑发现
- 启动时扫描总线，根据设备身份自动映射
- 支持任意轴数、任意总线位置
- 配置文件是声明性意图，非硬性约束

### 运行时查询
工具自动查询实际参数，无需硬编码：
```bash
echo 'topology' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```
输出：
```
OK|axes=2|a1:bus=1,enc=16384,gear=28/1|a2:bus=2,enc=16384,gear=28/1
```

### 实时监控
- 位置、速度、力矩、状态字
- 200ms ~ 5000ms 可调刷新率
- 显示跟随误差和 CiA 402 状态

### 交互式控制
- 持续运行，接受多次位置输入
- 支持任意轴数（动态适配）
- Unix socket 命令接口

## 交互式位置控制工具

Python 交互式客户端，用于实时控制电机位置（需在 Orange Pi 上运行）：

```bash
# 启动主站后，运行交互式控制工具
python3 tools/interactive_control.py /tmp/emaster-orangepi-bench-dual.sock
```

### 可用命令

| 命令 | 说明 | 示例 |
|------|------|------|
| `s` 或 `status` | 查看当前状态（位置、速度、力矩） | `s` |
| `t` 或 `topology` | 查看拓扑与单步限幅 | `t` |
| `g <c1> <c2>` | 绝对移动（counts） | `g 573362 389669` |
| `d <d1> <d2>` | 绝对移动（度，输出轴） | `d 90 120` |
| `r <d1> <d2>` | 相对移动（度，相对最后提交目标） | `r 30 -30` |
| `speed <deg/s>` | 设置斜坡速度，默认 20 | `speed 5` |
| `rate <hz>` | 设置发送频率，默认 50 | `rate 100` |
| `q` 或 `quit` | 退出 | `q` |

### 使用示例

```bash
> t                          # 先看拓扑和限幅
轴数=2 单步限幅=6400 ≈ 5.02°
  a1: bus=1 enc=16384 gear=28/1
  a2: bus=2 enc=16384 gear=28/1
  换算: 1° = 1274.31 counts

> r 30 30                    # 两轴各相对移动 30 度
斜坡：2 轴，94 步，步长上限 508 counts，50Hz，约 1.88s
      起点 [573360, 389673]
      终点 [611589, 427902]
斜坡完成
```

### 大角度移动是斜升的，不是一步到位

主站对相邻两条目标的增量有硬限幅（`topology` 返回的 `max_step`）。**单步超限不会返回
错误，而是判定 `MOTION_INVALID` 后中止整个会话、轴失能**。所以工具会把大角度目标
拆成小步以 50Hz 流式发送。

工具还处理了主站的 5 秒空闲超时：两条命令间隔超过 5 秒，服务端会关掉连接。工具在
发送前检查闲置时长并自动重连，所以你可以慢慢想。

### 安全约束

- 工具会自动等待主站进入 RUNNING 状态（state=4）后才允许发送目标
- 单步增量受 `max_step` 约束，客户端自己负责斜坡
- 超过 200ms 未收到新目标，主站自动切换为 HOLD 模式（保持最后目标）
- Ctrl+C 可中断斜坡，主站保持最后一条成功提交的目标

### 协议文档

完整的命令协议规格见 `docs/protocol/command-protocol-v1.md`

## 系统架构

```
配置文件（期望）
    ↓
扫描总线（真相）
    ↓
身份匹配映射
    ↓
运行时API
    ↓
工具查询适配
```

**核心理念**：扫描结果是权威，配置文件表达意图。

## 故障排查

**主站未启动**：
```bash
pgrep -x emaster-master || echo "主站未运行"
```

**查看日志**：
```bash
tail -f /tmp/master.log
```

**测试连接**：
```bash
echo 'status' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

## 编译构建

```bash
cd ~/ethercat-master
mkdir -p build/bench
cd build/bench
cmake ../..
make -j8
```

## 技术栈

- **SOEM** - Simple Open EtherCAT Master
- **CiA 402** - CANopen 驱动器和运动控制设备规范
- **DC (Distributed Clocks)** - 亚微秒级时钟同步
- **PDO (Process Data Objects)** - 实时周期数据交换

## 许可证

根据项目实际许可证填写。

## 贡献

根据项目实际贡献指南填写。
