# EtherCAT Master

工业级 EtherCAT 主站实现，基于 SOEM，支持 CiA 402 驱动器控制。

## 快速开始

### 启动主站

```bash
sudo ~/ethercat-master/build/bench/tools/master/emaster-master --deployment orangepi-bench-dual &
```

### 监控运行状态

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_watch/emaster-watch 500
```

### 角度控制（推荐）

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_move_deg/emaster-move-deg
```

输入目标位置（单位：角度）：
```
角度> 45.5 120.0
```

### 脉冲控制（底层接口）

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_move/emaster-move
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
