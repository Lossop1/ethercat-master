# EtherCAT Master 命令协议规格 v1.0

## 1 概述

本文档定义主站运行期的外部控制协议。协议基于 Unix 域套接字，文本格式，单客户端连接，支持命令-响应模式。

**协议版本**: 1.0  
**适用主站**: EtherCAT Master (CSP 模式)  
**传输层**: Unix 域套接字（`SOCK_STREAM`）  
**套接字路径**: `/tmp/emaster-<deployment-id>.sock`  
**字符编码**: UTF-8  

---

## 2 连接模型

### 2.1 单客户端约束

主站同一时刻**只接受一个客户端连接**。第二个客户端的 `connect()` 调用会被阻塞或拒绝，直到当前客户端断开或超时。

**原因**: 命令队列深度为 16，多客户端会导致命令交错和目标冲突，无法保证单一客户端的控制权。

### 2.2 空闲超时与重连

- **超时时间**: 5 秒（`SO_RCVTIMEO`）
- **触发条件**: 客户端连接后 5 秒内无任何数据发送
- **超时行为**: 主站关闭连接，释放套接字，允许新客户端连接
- **外部目标失效**: 超过 200ms 未更新的外部目标自动切换为 HOLD 模式

**这意味着交互式客户端无法在两分钟内思考。** 只要两条命令间隔超过 5 秒，连接就被
服务端单方面关闭，下一次 `send` 拿到 `EPIPE`。客户端必须处理：要么保持发送
（空闲时也周期性发 `status`），要么捕获 `EPIPE` 后重连重试。轮询式客户端还需要在
每次发送前检查连接闲置时长，主动重连以避免撞上超时。

**重连方法**: 客户端检测到连接关闭（`recv()` 返回 0、`EPIPE` 或 `ECONNRESET`）后，
关闭套接字，等待至少 100ms 再重新 `connect()`，然后重新执行就绪检查（第 2.3 节）。

### 2.3 主站就绪信号（P6.2）

**关键安全约束**: 主站在进入 `RUNNING` 状态（`state=4`）之前不接受 `set_external_target` 命令。

**客户端流程**:
1. 连接到套接字
2. 发送 `status` 命令
3. 解析响应中的 `state` 字段
4. 若 `state != 4`，等待 50-100ms 后重复步骤 2
5. 确认 `state=4` 后，开始发送 `set_external_target`

**违反约束的后果**: 若在 `state=4` 前注入目标，命令会返回 `ERROR|Not ready: state=X (need RUNNING=4)`；早期版本可能导致首周期大幅跳变。

---

## 3 命令格式

### 3.1 通用格式

```
<command_name> [<arg1> <arg2> ...]\n
```

- 命令以换行符 `\n` 结尾
- 参数以空格分隔
- 命令名大小写敏感
- 最大命令长度: 512 字节

### 3.2 响应格式

```
OK|<payload>\n
```
或
```
ERROR|<message>\n
```

- 响应以换行符结尾
- `OK` 表示命令执行成功，`payload` 为返回数据
- `ERROR` 表示命令失败，`message` 为错误原因
- 最大响应长度: 1024 字节

---

## 4 命令列表

### 4.1 `status`

查询主站当前状态和所有轴的实时反馈。

**请求**: `status\n`

**响应**: `OK|state=<s> cycle=<c> axes=<n> enabled=<e> completed=<m>|a<i>:pos=<p>,vel=<v>,torque=<t>,status=<sw>,target_pos=<tp>,planned=<pp>,err=<ec>,state=<as>|...\n`

**字段说明**:
- `state`: 主站状态码
  - `0` = INITIALIZING
  - `1` = PRE_OPERATIONAL
  - `2` = SAFE_OPERATIONAL
  - `3` = ENABLING（状态机转换中）
  - `4` = RUNNING（正常运行，可接受外部目标）
  - `5` = STOPPING
  - `6` = FAULTED
- `cycle`: 当前周期计数（从 0 开始）
- `axes`: 轴数量
- `enabled`: 是否所有轴已到达 Operation Enabled（`1` = 是，`0` = 否）
- `completed`: 运动方案是否完成（`1` = 是，`0` = 否或无运动方案）

**每轴字段**（以 `|a<i>:` 分隔，`i` 为轴号 1-based）:
- `pos`: 实际位置（**原始 encoder counts**，非工程单位）
- `vel`: 实际速度（counts/cycle）
- `torque`: 实际力矩（设备原始单位，通常 0.1% 额定力矩）
- `status`: 状态字（`0x<4位16进制>`，CiA402 StatusWord）
- `target_pos`: 本周期主站写入的目标位置（counts）
- `planned`: 计划路径生成的目标位置（counts，用于诊断）
- `err`: 错误码（`0x<4位16进制>`，轴级错误掩码）
- `state`: CiA402 状态机状态（0-7，见 CiA402 规范）

**截断标记**: 若轴数过多导致响应超过 1024 字节，最后 7 字节会被 `|TRUNC` 覆盖，表示数据不完整。

**示例**:
```
请求: status
响应: OK|state=4 cycle=123456 axes=2 enabled=1 completed=0|a1:pos=573362,vel=0,torque=0,status=0x1637,target_pos=573362,planned=573362,err=0x0000,state=7|a2:pos=389669,vel=0,torque=0,status=0x1637,target_pos=389669,planned=389669,err=0x0000,state=7
```

---

### 4.2 `topology`

查询总线拓扑和硬件参数（不随时间变化）。

**请求**: `topology\n`

**响应**: `OK|axes=<n>,max_step=<s>|a<i>:bus=<b>,enc=<e>,gear=<gm>/<gs>,torque=<rt>|...\n`

**字段说明**:
- `axes`: 轴数量
- `max_step`: 相邻两条目标允许的最大增量（**原始 counts**，全轴统一值）。
  客户端必须按此值把大角度目标拆成小步流式发送，见第 7.2 节。`0` 表示未启用单步限幅。
- `bus`: EtherCAT 总线位置（1-based，与物理链路顺序一致）
- `enc`: 编码器分辨率（increments per motor revolution）
- `gear`: 减速比（`motor_revolutions / shaft_revolutions`）
- `torque`: 额定力矩（当前实现为 0，后续版本填充）

**示例**:
```
请求: topology
响应: OK|axes=2,max_step=6400|a1:bus=1,enc=16384,gear=28/1,torque=0|a2:bus=2,enc=16384,gear=28/1,torque=0
```

---

### 4.3 `set_external_target`

设置多轴位置目标（CSP 模式）。

**请求**: `set_external_target <pos1> <pos2> ... <posN>\n`

**参数**: 
- `<posI>`: 第 I 轴的目标位置（**原始 encoder counts**，32 位有符号整数）
- 必须提供与 `topology` 返回轴数完全一致的参数个数
- 轴序与 `topology` 和 `status` 一致（按总线位置排序）

**前置条件**:
- 主站必须处于 `RUNNING` 状态（`state=4`），否则返回 `ERROR|Not ready`
- 目标与上一条**已提交**目标的增量不得超过 `topology` 返回的 `max_step`

**响应**: 
- 成功: `OK|Updated <n> external targets\n`
- 失败: `ERROR|<reason>\n`
  - `Not ready: state=X (need RUNNING=4)` — 主站未进入 RUNNING 状态
  - `Wrong count: expected X, got Y` — 参数个数不匹配
  - `Invalid position value: <token>` — 参数格式错误或超出 int32 范围

**注意**：单步超限**不在此响应中体现**。限幅在周期线程里检查
（`session_target.c`），超限会经 `note_runtime_failure` 判定为
`MOTION_INVALID` 并**中止整个会话**，而不是返回一条错误响应。
命令返回 `OK` 只说明目标已入队，不代表它通过了限幅检查。
详见第 7.2 节。

**超时行为**: 若超过 200ms 未收到新的 `set_external_target`，主站自动切换为 HOLD 模式（保持最后一次目标），不会停机或报错。

**示例**:
```
请求: set_external_target 573362 389669
响应: OK|Updated 2 external targets
```

---

### 4.4 `stop`

请求主站停止运动并进入停机流程。

**请求**: `stop\n`

**响应**: `OK|stop requested\n`

**行为**: 主站在完成当前周期后逐步关闭，最终退出 `RUNNING` 状态。客户端应在发送 `stop` 后断开连接。

---

### 4.5 `quick_stop`

触发所有轴的 Quick Stop（CiA402 控制字 `0x0002`）。

**请求**: `quick_stop\n`

**响应**: `OK|quick stop requested for <n> axes\n`

**行为**: 每个轴立即执行设备定义的快速停止轨迹（通常为最大减速度停止）。主站不退出 `RUNNING` 状态，但不再接受新的位置目标，直到重新使能。

---

### 4.6 `halt`

设置或清除所有轴的 Halt 位（CiA402 控制字 bit8）。

**请求**: `halt <flag>\n`

**参数**:
- `<flag>`: `1` = 置位 Halt（轴减速停止但保持使能），`0` = 清除 Halt（恢复运动）

**响应**: `OK|halt <set|cleared> for <n> axes\n`

**示例**:
```
请求: halt 1
响应: OK|halt set for 2 axes
```

---

### 4.7 `fault_reset`

对所有轴发送 Fault Reset 信号（CiA402 控制字 `0x0080`）。

**请求**: `fault_reset\n`

**响应**: `OK|fault reset requested for <n> axes\n`

**行为**: 尝试清除驱动器故障，使轴从 Fault 状态转换到 Switch On Disabled。若故障持续存在（如过流、过温），重置会失败且轴重新进入 Fault。

**注意**: 当前实现未在真机长时验证（P2.5 硬件阻塞）。

---

### 4.8 `switch`

切换运动方案（需要预先在配置中定义）。

**请求**: `switch <motion_profile_id>\n`

**参数**: `<motion_profile_id>` — 运动方案 ID（如 `orangepi-bench.output-positive-36deg`）

**响应**: 
- 成功: `OK|motion switched to <motion_profile_id>\n`
- 失败: `ERROR|motion profile not found: <id>\n` 或 `ERROR|motion switch failed: status=<code>\n`

**约束**: 仅在主站支持运行期方案切换时可用（当前 CSP 专项未实现，P5.1 待做）。

---

### 4.9 `shutdown`

请求优雅关闭主站。

**请求**: `shutdown\n`

**响应**: `OK|shutdown requested\n`

**行为**: 等价于 `stop`，但语义上明确表示关机意图。主站完成当前周期后停止总线通信并退出。

---

## 5 单位约定

### 5.1 位置单位

**协议层位置单位为原始 encoder counts**（编码器增量计数，int32），对应电机侧。

**换算公式**（输出轴角度 → counts）:
```
counts_per_degree = (encoder_increments / encoder_motor_revolutions)
                  × (gear_motor_revolutions / gear_shaft_revolutions) / 360
counts = angle_degrees × counts_per_degree
```

**示例**（16384 inc/rev 编码器，28:1 减速比）:
- `counts_per_degree` = 16384 × 28 / 360 = **1274.31 counts / 度**
- 输出轴 1° = 1,274 counts
- 输出轴 36° = 45,875 counts
- 输出轴 360°（整圈）= 458,752 counts

**原因**: L3 层只处理原始 counts，单位换算由 L4 层负责。协议暴露的是 L3 边界。

**注意**：`topology` 返回的 `enc` 和 `gear` 就是上式的输入，客户端应现场换算而不是
硬编码。不同部署的减速比可能不同。

### 5.2 速度与力矩单位

- **速度**: counts/cycle（1ms 周期下即 counts/ms）
- **力矩**: 设备原始单位（通常为 0.1% 额定力矩，int16）

---

## 6 错误处理

### 6.1 命令解析错误

未识别的命令返回空响应或关闭连接（实现依赖）。建议客户端设置 5 秒接收超时，超时后重连。

### 6.2 参数错误

返回 `ERROR|<specific message>`，连接保持打开，客户端可继续发送命令。

### 6.3 主站状态异常

若主站进入 `FAULTED` 状态（`state=6`），所有命令仍可发送，但 `set_external_target` 会被拒绝。客户端应：
1. 停止发送目标
2. 发送 `fault_reset` 尝试恢复
3. 持续查询 `status` 直到 `state` 恢复到 `4`

### 6.4 连接丢失

客户端检测到 `recv()` 返回 0、`EPIPE` 或 `ECONNRESET` 时：
1. 关闭当前套接字
2. 等待 100-500ms
3. 重新连接并执行就绪检查（`status` 直到 `state=4`）

---

## 7 限幅与安全

### 7.1 跟随误差限幅（P6.3）

每个轴配置了 `max_following_error`（从部署配置或运动方案读取，默认 2° 输出轴）。若实际位置与目标位置差值超过此限制，主站触发故障隔离该轴。

**客户端责任**: 确保发送的目标轨迹可达（速度、加速度在设备能力范围内）。

### 7.2 单步限幅（P6.3）

相邻两次 `set_external_target` 的位置增量不得超过 `topology` 返回的 `max_step`。

**超限的后果是会话中止，不是命令被拒。** 限幅在周期线程里检查：
`session_target.c` 返回 `MOTION_INVALID`，`session_control.c` 随即调用
`note_runtime_failure` 并置 `safety_denied`，整个控制会话终止、轴失能。
客户端不会从 `set_external_target` 的响应中看出任何异常——那条命令早已返回 `OK`。

**客户端必须做的事**：把大角度目标拆成小步流式发送，单步增量不超过 `max_step`。
发送频率需高于主站 200ms 的外部目标看门狗。

**原因**: 防止客户端错误或网络延迟导致的突变指令损坏设备。单步限幅的取值来源：
若部署引用了运动方案，取该轴 `max_following_error_millidegrees` 换算；否则回退到
6400 counts（16384 enc × 28:1 下约 5.02°，不是 2°）。

### 7.3 外部目标看门狗（P2.1）

`set_external_target` 必须以 ≤200ms 的频率持续发送。超时后主站自动切换为 HOLD（保持最后目标），不会报错或停机。

**推荐发送频率**: 50-100Hz（10-20ms 周期），远高于看门狗阈值。

---

## 8 客户端实现示例

### 8.1 参考实现

`tools/interactive_control.py` 是一个可用的参考客户端，覆盖了本协议三个最容易踩的坑：
连接复用与超时重连、从 `topology` 读取 `max_step`、以及把大角度目标拆成小步流式发送。
对接前建议先读它。

### 8.2 最小骨架

```python
import socket, time

class Client:
    def __init__(self, path):
        self.path = path
        self.sock = None
        self.buf = b""

    def connect(self):
        self.close()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5.0)
        self.sock.connect(self.path)

    def close(self):
        if self.sock:
            self.sock.close()
        self.sock = None
        self.buf = b""

    def command(self, cmd, retries=2):
        """发送命令；连接被 5 秒空闲超时关掉时自动重连重试。"""
        for attempt in range(retries + 1):
            try:
                if self.sock is None:
                    self.connect()
                self.sock.sendall((cmd + "\n").encode())
                while b"\n" not in self.buf:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        raise ConnectionResetError
                    self.buf += chunk
                line, _, self.buf = self.buf.partition(b"\n")
                return line.decode().strip()
            except (OSError, ConnectionResetError):
                self.close()
                if attempt == retries:
                    raise
        return None

# 1. 等待就绪：必须等到 state=4 才能发目标（第 2.3 节）
c = Client("/tmp/emaster-orangepi-bench-dual.sock")
while "state=4" not in (c.command("status") or ""):
    time.sleep(0.1)

# 2. 读单步限幅，不能硬编码（第 7.2 节）
topo = c.command("topology")
max_step = int(topo.split("max_step=")[1].split("|")[0])

# 3. 移动必须斜坡：单步不超过 max_step，频率高于 5Hz 以喂 200ms 看门狗
start = [573362, 389669]
target = [611589, 427902]
steps = 60
for i in range(1, steps + 1):
    pos = [s + (t - s) * i // steps for s, t in zip(start, target)]
    c.command("set_external_target " + " ".join(map(str, pos)))
    time.sleep(0.02)
```

### 8.3 注意事项

- **不要单步超过 `max_step`**。超限不会返回错误，而是中止整个会话（第 7.2 节）
- 保持连接复用。每条命令新建连接会不断撞上单客户端约束
- 若在两条命令之间思考超过 5 秒，下次发送前先重连（第 2.2 节）
- 持续控制应使用独立线程：一个线程 50-100Hz 发送目标，另一个线程 1-10Hz 查询状态
- 捕获 `SIGINT` 并发送 `stop` 或 `shutdown` 以优雅退出

---

## 9 版本历史

| 版本 | 日期 | 变更 |
| --- | --- | --- |
| 1.0 | 2026-09-14 | 初始版本：P6.1 协议规格交付 |

---

## 10 已知限制

- `status` 响应**不包含**电流、母线电压、温度。这三个量已由 SDO 观测线程采集并写入运行报告
  （JSON），但尚未加入 `status` 命令的响应格式。需要它们做在线判决的外部控制器当前只能读
  运行报告。
- P5.1 未实现：运行期模式切换（CSP/CSV/CST）和 `switch` 命令当前不可用
- P2.5 部分验证：Fault Reset 调用路径存在但未在真机长时验证（驱动器静置问题阻塞）
- 单客户端约束：多客户端并发控制需要外部协调（如消息队列或优先级仲裁），协议层不支持
