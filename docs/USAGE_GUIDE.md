# EtherCAT 主站使用指南

## 系统架构

本系统采用动态拓扑发现架构：
- 主站启动时扫描总线，根据设备身份（VID/PID）动态映射
- 支持任意轴数、任意总线位置
- 工具通过运行时API查询实际拓扑和参数
- 配置文件是声明性意图，不是硬性约束

## 快速启动

### 1. 启动主站

```bash
sudo ~/ethercat-master/build/bench/tools/master/emaster-master --deployment orangepi-bench-dual &
```

**启动日志示例**：
```
部署配置名称=orangepi6plus，EtherCAT接口=enp49s0，配置=bench-dual-slave，从站数=2，按 Ctrl+C 安全停止。
命令服务器监听：/tmp/emaster-orangepi-bench-dual.sock
[INIT] Initial positions: [0]=26862 [1]=477179
[MODE] Running DEMO sine wave (no external input)
```

**检查主站是否运行**：
```bash
pgrep -x emaster-master
ls -l /tmp/emaster-orangepi-bench-dual.sock
```

### 2. 实时监控

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_watch/emaster-watch 500
```

**显示内容**：
- 启动时显示系统拓扑（轴数、编码器参数、齿轮比、换算系数）
- 然后进入实时监控（500ms刷新）
- 显示位置、速度、力矩、状态字、跟随误差

**停止监控**：按 `Ctrl+C`

### 3. 交互式控制

```bash
sudo ~/ethercat-master/build/bench/tools/emaster_move/emaster-move
```

**使用方法**：
```
位置> 60000 500000    # 输入各轴目标位置（脉冲）
位置> 45000 490000    # 继续输入新位置
位置> quit            # 退出
```

**注意**：
- 单位是**脉冲**（不是角度）
- 输入的轴数必须与实际系统一致
- 工具在后台持续运行，保持控制权

## 电机参数（当前系统）

通过 `topology` 命令查询：
```bash
echo 'topology' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

**当前配置**（2024-09-10）：
```
轴1: 总线位置1, 编码器16384脉冲/转, 齿轮比28:1
轴2: 总线位置2, 编码器16384脉冲/转, 齿轮比28:1
```

**单位换算**：
- 电机侧：16384 脉冲/转
- 齿轮比：28:1（电机转28圈，负载转1圈）
- **负载侧**：458752 脉冲/转
- **角度换算**：**1° = 1274 脉冲**

**示例计算**：
- 45° → 57330 脉冲
- 90° → 114660 脉冲
- 180° → 229320 脉冲
- 360° → 458752 脉冲（1圈）

## 高级操作

### 直接命令接口

通过 Unix socket 发送命令：

**查询状态**：
```bash
echo 'status' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

**查询拓扑**：
```bash
echo 'topology' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

**设置外部位置目标**：
```bash
echo 'set_external_target 60000 500000' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

**停止运动**：
```bash
echo 'stop' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

**关闭主站**：
```bash
echo 'shutdown' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

### 查看日志

**主站日志**：
```bash
tail -f /tmp/master.log
```

**审计报告**（主站停止后生成）：
```bash
ls -lh /tmp/emaster-audit-*.json
cat /tmp/emaster-audit-*.json | jq .
```

## 故障排查

### 主站无法启动

**检查接口状态**：
```bash
ip link show enp49s0
```

**检查从站连接**：
```bash
sudo ethercat slaves  # 如果安装了 ethercat 命令行工具
```

**查看错误日志**：
```bash
cat /tmp/master.log
```

**常见问题**：
1. 网卡无载波（网线未连接）
2. 从站未上电
3. 配置文件中的设备身份与实际不符

### 工具无法连接

**检查 socket 文件**：
```bash
ls -l /tmp/emaster-orangepi-bench-dual.sock
```

**检查主站进程**：
```bash
pgrep -x emaster-master
ps aux | grep emaster-master
```

**手动测试连接**：
```bash
echo 'status' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock
```

### 位置不正确

**检查当前位置**：
- 主站启动时显示初始位置：`[INIT] Initial positions: [0]=X [1]=Y`
- 这是上电时编码器的实际位置
- 零点定义为上电位置，非绝对零点

**位置累积**：
- 位置可以超过 ±2^31，支持多圈累积
- 正常情况下不会溢出

**跟随误差**：
- 实时监控中显示 `err=0xXXXX`
- 如果持续增大，说明驱动器跟不上目标
- 检查速度是否过快

## 系统限制（待实现）

当前版本**尚未实现**以下保护：
- 软件限位（位置范围）
- 速度限制
- 加速度限制
- 急停功能（只能通过 Ctrl+C 停止主站）

**注意安全**：
- 测试时确保机械行程范围内无障碍物
- 从小幅度运动开始测试
- 随时准备按 `Ctrl+C` 停止

## 开发与调试

### 编译构建

```bash
cd ~/ethercat-master
mkdir -p build/bench
cd build/bench
cmake ../..
make -j8
```

### 单元测试

```bash
cd build/bench
ctest --output-on-failure
```

### 添加新轴

1. 修改配置文件（如有需要）
2. 主站会自动扫描并映射
3. 工具会自动适配轴数

**无需修改工具代码** — 这是动态拓扑架构的优势。

## 进阶功能（规划中）

### 角度控制接口
```bash
sudo emaster-move-deg
角度> 45.0 120.5    # 输入角度（度）
```

### 轨迹规划
```bash
sudo emaster-move --profile trapezoidal --max-velocity 1000 --max-accel 500
```

### 配置文件化限制
```json
{
  "axes": [
    {
      "soft_limit_min_deg": -180.0,
      "soft_limit_max_deg": 180.0,
      "max_velocity_rpm": 60.0,
      "max_acceleration_deg_s2": 360.0
    }
  ]
}
```

## 参考文档

- 电机参数说明：`docs/MOTOR_PARAMETERS.md`
- 拓扑重构设计：`docs/TOPOLOGY_REFACTOR.md`
- 配置格式：`deployments/*.toml`
- API 参考：`include/emaster/`

## 获取帮助

**工具帮助**：
```bash
emaster-watch --help
emaster-move --help
```

**诊断信息**：
```bash
# 系统状态
echo 'status' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock

# 拓扑信息
echo 'topology' | sudo nc -U /tmp/emaster-orangepi-bench-dual.sock

# 审计日志
ls -lh /tmp/emaster-audit-*.json
```
