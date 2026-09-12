# 电机级故障恢复设计（P2.5）

## 目标

实现单轴故障后的独立恢复，允许其他健康轴继续运行。

## 当前行为

**全局停止模式**：
- 任何一轴进入 FAULT 状态
- `session_control.c` 检测到 `fault_present`
- 设置 `safety_denied = true`
- 所有轴停止运动，进入 Quick-Stop

**问题**：
- 一个轴故障导致整条生产线停机
- 无法继续使用健康轴完成部分任务

---

## 设计方案

### 方案 1：轴级隔离（推荐）

**架构**：
- 每个轴独立维护故障状态
- 故障轴进入 Quick-Stop 并标记为 "隔离"
- 健康轴继续执行运动指令
- 用户通过命令服务器单独恢复故障轴

**优点**：
- 故障影响范围最小
- 灵活性高，用户可选择何时恢复

**缺点**：
- 需要处理轴间依赖（机械耦合）
- 增加状态机复杂度

### 方案 2：降级运行模式

**架构**：
- 检测到故障后，系统进入 "降级模式"
- 所有轴先 Quick-Stop
- 用户确认后，健康轴恢复到 OP
- 故障轴保持 Quick-Stop，等待单独恢复

**优点**：
- 安全性高，有明确的降级点
- 适合有机械耦合的系统

**缺点**：
- 所有轴都会短暂停止
- 需要用户介入确认

---

## 实现方案（方案 1 - 轴级隔离）

### 1. 数据结构扩展

```c
/* include/emaster/bus/control_session.h */
typedef enum {
    EMASTER_AXIS_STATUS_NORMAL,        /* 正常运行 */
    EMASTER_AXIS_STATUS_FAULTED,       /* 故障，已隔离 */
    EMASTER_AXIS_STATUS_RECOVERING,    /* 恢复中 */
    EMASTER_AXIS_STATUS_ISOLATED       /* 手动隔离（预留） */
} emaster_axis_status_t;

typedef struct {
    /* 现有字段... */
    
    /* P2.5: 轴级故障恢复 */
    emaster_axis_status_t axis_status;
    bool fault_isolated;               /* 此轴已隔离，不影响其他轴 */
    uint64_t fault_cycle;              /* 故障发生的周期号 */
    emaster_control_session_status_t fault_reason; /* 故障原因 */
} emaster_control_session_axis_result_t;
```

### 2. 故障检测和隔离

```c
/* src/bus/soem/session_control.c */

/* 新增：单轴故障处理 */
static void handle_axis_fault(
    emaster_soem_session_t *session,
    size_t axis_index,
    emaster_control_session_status_t fault_reason)
{
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
    
    /* 标记故障 */
    axis->axis_status = EMASTER_AXIS_STATUS_FAULTED;
    axis->fault_isolated = true;
    axis->fault_cycle = session->exchange;
    axis->fault_reason = fault_reason;
    
    /* 将该轴控制器目标设为 Quick-Stop */
    emaster_cia402_controller_request_quick_stop(&session->controllers[axis_index]);
    
    /* 记录到审计 */
    fprintf(stderr, "[P2.5] 轴%zu 故障隔离: 原因=%d, 周期=%" PRIu64 "\n",
            axis_index, fault_reason, session->exchange);
}

/* 修改现有逻辑：从全局 safety_denied 改为单轴处理 */
static emaster_control_session_status_t check_axis_health(
    emaster_soem_session_t *session,
    size_t axis_index,
    bool *should_isolate)
{
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
    
    *should_isolate = false;
    
    /* 跳过已隔离的轴 */
    if (axis->fault_isolated) {
        return EMASTER_CONTROL_SESSION_OK;
    }
    
    /* 检查故障条件 */
    if (session->controller_outputs[axis_index].fault_present) {
        *should_isolate = true;
        return EMASTER_CONTROL_SESSION_DRIVE_FAULT;
    }
    
    if (!session->controller_outputs[axis_index].state_known) {
        *should_isolate = true;
        return EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
    }
    
    if (axis->internal_limit_active) {
        *should_isolate = true;
        return EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE;
    }
    
    return EMASTER_CONTROL_SESSION_OK;
}
```

### 3. 周期控制逻辑

```c
/* src/bus/soem/session_control.c - 修改 session_run */

emaster_control_session_status_t emaster_soem_session_run(
    emaster_soem_session_t *session)
{
    size_t axis_index;
    bool all_axes_enabled = true;
    bool all_modes_confirmed = true;
    emaster_control_session_status_t safety_status = EMASTER_CONTROL_SESSION_OK;
    
    for (axis_index = 0; axis_index < session->plan->axis_count; ++axis_index)
    {
        bool should_isolate = false;
        emaster_control_session_status_t axis_status;
        
        /* 检查轴健康状态 */
        axis_status = check_axis_health(session, axis_index, &should_isolate);
        
        if (should_isolate) {
            /* 隔离故障轴，但不停止全局运行 */
            handle_axis_fault(session, axis_index, axis_status);
            
            /* 第一次故障记录到全局状态（审计用） */
            if (safety_status == EMASTER_CONTROL_SESSION_OK) {
                safety_status = axis_status;
            }
            
            continue; /* 跳过此轴，继续处理其他轴 */
        }
        
        /* 已隔离的轴：保持 Quick-Stop，不参与运动 */
        if (session->axes[axis_index].fault_isolated) {
            continue;
        }
        
        /* 健康轴：正常处理 */
        // ... 现有的控制逻辑 ...
    }
    
    /* 修改：只要有任何健康轴在运行，就不返回错误 */
    size_t healthy_count = 0;
    for (axis_index = 0; axis_index < session->plan->axis_count; ++axis_index) {
        if (!session->axes[axis_index].fault_isolated) {
            healthy_count++;
        }
    }
    
    if (healthy_count == 0) {
        /* 所有轴都故障了，才停止整个会话 */
        return EMASTER_CONTROL_SESSION_ALL_AXES_FAULTED;
    }
    
    return EMASTER_CONTROL_SESSION_OK;
}
```

### 4. 恢复流程

```c
/* src/bus/soem/command_server.c - 新增命令 */

typedef enum {
    /* 现有命令... */
    EMASTER_COMMAND_RECOVER_AXIS,  /* 恢复单个轴 */
} emaster_command_type_t;

/* 处理恢复命令 */
case EMASTER_COMMAND_RECOVER_AXIS: {
    size_t axis_index = command.axis_index;
    
    if (axis_index >= session->plan->axis_count) {
        response.status = EMASTER_COMMAND_STATUS_INVALID_AXIS;
        break;
    }
    
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
    
    if (!axis->fault_isolated) {
        response.status = EMASTER_COMMAND_STATUS_NOT_FAULTED;
        break;
    }
    
    /* 标记为恢复中 */
    axis->axis_status = EMASTER_AXIS_STATUS_RECOVERING;
    
    /* 发送 Fault Reset */
    emaster_cia402_controller_request_fault_reset(&session->controllers[axis_index]);
    
    fprintf(stderr, "[P2.5] 轴%zu 开始恢复流程\n", axis_index);
    
    response.status = EMASTER_COMMAND_STATUS_OK;
    break;
}
```

### 5. 恢复状态机

```c
/* 在周期中检查恢复进度 */
static void check_recovery_progress(
    emaster_soem_session_t *session,
    size_t axis_index)
{
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
    emaster_cia402_output_t *output = &session->controller_outputs[axis_index];
    
    if (axis->axis_status != EMASTER_AXIS_STATUS_RECOVERING) {
        return;
    }
    
    /* 检查是否已到达 Switch On Disabled */
    if (output->state == EMASTER_CIA402_STATE_SWITCH_ON_DISABLED &&
        !output->fault_present)
    {
        /* 恢复成功，重新启动轴 */
        axis->axis_status = EMASTER_AXIS_STATUS_NORMAL;
        axis->fault_isolated = false;
        
        /* 请求上电 */
        emaster_cia402_controller_request_operation_enabled(
            &session->controllers[axis_index]);
        
        fprintf(stderr, "[P2.5] 轴%zu 恢复成功，重新启动\n", axis_index);
    }
    else if (session->exchange - axis->fault_cycle > 5000)
    {
        /* 超时（5秒），恢复失败 */
        fprintf(stderr, "[P2.5] 轴%zu 恢复超时\n", axis_index);
        axis->axis_status = EMASTER_AXIS_STATUS_FAULTED;
    }
}
```

---

## 硬件依赖验证

### 需要验证的驱动器行为

1. **Quick-Stop 后的状态保持**
   - 轴A进入 Quick-Stop
   - 轴B是否继续接受 PDO 并执行运动？

2. **Fault Reset 的作用域**
   - 发送单轴 Fault Reset（SDO 到特定从站）
   - 是否影响其他从站？

3. **总线状态**
   - 单个从站 Fault 是否影响总线 WKC？
   - 是否需要继续发送该从站的 PDO？

### 验证方法

```c
/* 测试工具：单轴故障注入 */
// 1. 启动双轴系统
// 2. 通过命令服务器强制轴0进入 Quick-Stop
// 3. 观察轴1是否继续运动
// 4. 发送轴0 Fault Reset
// 5. 观察轴0是否恢复

/* 测试命令序列 */
emaster-control set-axis-goal 0 quick-stop
sleep 2
emaster-control get-status  // 检查轴1状态
emaster-control recover-axis 0
sleep 2
emaster-control get-status  // 检查轴0状态
```

---

## 实现计划

### 阶段1：框架搭建（无硬件依赖）
- [ ] 添加 `emaster_axis_status_t` 枚举
- [ ] 扩展 `emaster_control_session_axis_result_t` 结构
- [ ] 实现 `handle_axis_fault()` 函数
- [ ] 实现 `check_axis_health()` 函数
- [ ] 修改 `session_run()` 逻辑，移除全局 `safety_denied`

### 阶段2：命令接口（无硬件依赖）
- [ ] 添加 `EMASTER_COMMAND_RECOVER_AXIS` 命令
- [ ] 实现恢复命令处理
- [ ] 添加 `check_recovery_progress()` 函数
- [ ] 扩展 `emaster-control` 工具支持 `recover-axis` 命令

### 阶段3：硬件验证（需要硬件）
- [ ] 测试单轴 Quick-Stop 时其他轴是否继续运行
- [ ] 测试 Fault Reset 的作用域
- [ ] 测试恢复后轴重新启动
- [ ] 验证 WKC 和 PDO 行为

### 阶段4：优化和文档（验证后）
- [ ] 添加恢复超时配置
- [ ] 支持用户自定义隔离策略
- [ ] 编写使用文档和故障排除指南
- [ ] 添加到报告 JSON

---

## 风险和限制

### 已知风险

1. **机械耦合**
   - 多轴机械耦合系统（龙门、并联机构）
   - 单轴停止可能导致机械损坏
   - 缓解：用户配置"耦合组"，组内任一轴故障则全组停止

2. **轨迹规划**
   - 插补运动中单轴故障
   - 其他轴继续运动可能导致路径偏差
   - 缓解：检测插补模式，故障时停止所有参与轴

3. **总线带宽**
   - 故障轴是否仍占用 PDO？
   - 可能影响周期时间
   - 缓解：硬件验证后确定

### 当前限制

- 不支持运动中恢复（恢复后从当前位置重新启动）
- 不支持轴间依赖配置（需要人工判断）
- 恢复超时固定为 5 秒（未来可配置）

---

## 替代方案对比

| 方案 | 故障影响 | 实现复杂度 | 安全性 | 灵活性 |
|------|---------|-----------|--------|--------|
| 全局停止（当前） | 所有轴 | 低 | 高 | 低 |
| 轴级隔离（推荐） | 单轴 | 中 | 中 | 高 |
| 降级运行 | 短暂全部 | 高 | 高 | 中 |

**选择理由**：
- 轴级隔离在安全性和灵活性间取得平衡
- 实现复杂度可控
- 用户可选择何时恢复，而非强制自动恢复

---

## 参考

- CiA 402 规范：Fault 和 Quick-Stop 状态转换
- P2.6：运行期 603Fh 错误码监控（已实现）
- 当前代码：`src/bus/soem/session_control.c:200-210`
