# 错误恢复配置解耦设计与实现

## 1. 设计目标

### 1.1 问题陈述
之前的WKC容错实现存在以下问题：
- **硬编码阈值**：连续5次、累计50次的阈值直接写在代码中
- **配置耦合**：错误恢复策略与业务逻辑紧密耦合
- **可扩展性差**：添加新的错误恢复机制（AL状态、CiA 402故障）会进一步加剧耦合

### 1.2 解耦目标
- **配置与代码分离**：阈值和策略参数通过JSON配置，不写在代码中
- **策略可选择**：不同部署可以选用不同的容错策略（严格/默认/宽容）
- **易于扩展**：新的错误恢复机制只需扩展配置结构，不修改核心逻辑
- **可维护性**：配置文件清晰可读，修改不需要重新编译业务代码

## 2. 架构设计

### 2.1 四层架构

```
┌─────────────────────────────────────────────────┐
│  配置文件层 (JSON)                                │
│  - config/error_recovery_policies/*.json       │
│  - 定义策略参数和阈值                            │
└─────────────────────────────────────────────────┘
                    ↓ 编译时生成
┌─────────────────────────────────────────────────┐
│  配置数据层 (Generated C)                        │
│  - generated/error_recovery_config.c           │
│  - 编译期静态数据结构                            │
└─────────────────────────────────────────────────┘
                    ↓ 查询接口
┌─────────────────────────────────────────────────┐
│  配置接口层 (Header)                             │
│  - emaster/config/error_recovery_config.h      │
│  - 类型定义和查询函数                            │
└─────────────────────────────────────────────────┘
                    ↓ 依赖注入
┌─────────────────────────────────────────────────┐
│  业务逻辑层 (Runtime)                            │
│  - session_exchange.c 使用配置的阈值            │
│  - 策略通过部署配置注入                          │
└─────────────────────────────────────────────────┘
```

### 2.2 配置关联关系

```
deployment_config
    ├── error_recovery_policy_id: "default"  (引用)
    └── ...

error_recovery_policy (id: "default")
    ├── wkc_recovery
    │   ├── enabled: true
    │   ├── consecutive_error_threshold: 5
    │   └── total_error_threshold: 50
    ├── deadline_recovery
    ├── al_state_recovery (待实现)
    └── cia402_fault_recovery (待实现)
```

## 3. 实现细节

### 3.1 配置文件结构

#### 3.1.1 错误恢复策略 (`config/error_recovery_policies/*.json`)

```json
{
  "schema_version": 1,
  "policy_id": "default",
  "description": "默认错误恢复策略：保守的容错阈值",
  "wkc_recovery": {
    "enabled": true,
    "consecutive_error_threshold": 5,
    "total_error_threshold": 50,
    "description": "WKC不匹配容错：连续N次或累计M次错误才停机"
  },
  "deadline_recovery": {
    "enabled": false,
    "consecutive_error_threshold": 10
  },
  "al_state_recovery": {
    "enabled": false,
    "check_interval_cycles": 100,
    "max_recovery_attempts": 3
  },
  "cia402_fault_recovery": {
    "enabled": false,
    "max_reset_attempts": 3,
    "recoverable_error_codes": []
  }
}
```

#### 3.1.2 预定义策略

- **default**: 保守容错（连续5次、累计50次）
- **strict**: 严格策略（连续3次、累计20次）
- **tolerant**: 宽容策略（连续10次、累计100次）

### 3.2 配置生成器

#### 3.2.1 工具：`tools/generate_error_recovery_config.py`

功能：
- 读取 `config/error_recovery_policies/*.json`
- 生成 `generated/error_recovery_config.c`
- 提供查询接口实现

#### 3.2.2 生成的C代码结构

```c
// 策略数组（静态数据）
static const emaster_error_recovery_policy_t policies[] = {
    { .policy_id = "default", .wkc_recovery = {...}, ... },
    { .policy_id = "strict", .wkc_recovery = {...}, ... },
    { .policy_id = "tolerant", .wkc_recovery = {...}, ... }
};

// 查询接口
size_t emaster_error_recovery_policy_count(void);
const emaster_error_recovery_policy_t *emaster_error_recovery_policy_at(size_t index);
const emaster_error_recovery_policy_t *emaster_error_recovery_policy_by_id(const char *policy_id);
```

### 3.3 配置接口层

#### 3.3.1 类型定义 (`include/emaster/config/error_recovery_config.h`)

```c
typedef struct {
    bool enabled;
    uint32_t consecutive_error_threshold;
    uint32_t total_error_threshold;
} emaster_wkc_recovery_config_t;

typedef struct {
    const char *policy_id;
    emaster_wkc_recovery_config_t wkc_recovery;
    emaster_deadline_recovery_config_t deadline_recovery;
    emaster_al_state_recovery_config_t al_state_recovery;
    emaster_cia402_fault_recovery_config_t cia402_fault_recovery;
} emaster_error_recovery_policy_t;
```

### 3.4 业务逻辑集成

#### 3.4.1 Session结构 (`session_internal.h`)

```c
typedef struct {
    // ...
    const emaster_error_recovery_policy_t *error_recovery_policy;  // 策略引用
    uint64_t wkc_consecutive_errors;  // 运行时计数器
    uint64_t wkc_total_errors;
} emaster_soem_session_t;
```

#### 3.4.2 策略加载 (`control_session.c`)

```c
// Session初始化时加载策略
const char *policy_id = plan->deployment->error_recovery_policy_id;
if (policy_id == NULL) {
    policy_id = "default";
}
session->error_recovery_policy = emaster_error_recovery_policy_by_id(policy_id);
```

#### 3.4.3 策略应用 (`session_exchange.c`)

```c
// 使用配置的阈值而不是硬编码
if (!matched) {
    ++session->wkc_consecutive_errors;
    ++session->wkc_total_errors;

    bool should_fail = false;
    if (session->error_recovery_policy != NULL &&
        session->error_recovery_policy->wkc_recovery.enabled) {
        const emaster_wkc_recovery_config_t *wkc_cfg =
            &session->error_recovery_policy->wkc_recovery;
        if (session->wkc_consecutive_errors >= wkc_cfg->consecutive_error_threshold ||
            session->wkc_total_errors >= wkc_cfg->total_error_threshold) {
            should_fail = true;
        }
    } else {
        // 未配置策略，采用保守默认行为（首次错误即停机）
        should_fail = true;
    }

    if (should_fail) {
        (void)fail_exchange(session, phase, EMASTER_CONTROL_SESSION_WKC_MISMATCH, true);
    }
}
```

### 3.5 部署配置集成

#### 3.5.1 部署配置引用策略 (`config/deployments/*.json`)

```json
{
  "deployment_id": "orangepi-bench-dual",
  "hostname": "orangepi6plus",
  "topology_id": "bench-dual-slave",
  "ethercat_interface": "enp49s0",
  "operation_profile_ids": ["cyberbeast.dynamic-dc-csp-1ms"],
  "run_report_path": "runtime/reports/orangepi-dual-bench-latest.json",
  "error_recovery_policy_id": "default"  // 引用策略ID
}
```

#### 3.5.2 部署配置结构扩展 (`runtime_config.h`)

```c
typedef struct {
    const char *deployment_id;
    const char *hostname;
    const char *ethercat_interface;
    // ...
    const char *error_recovery_policy_id;  // 新增字段
} emaster_deployment_config_t;
```

## 4. 构建系统集成

### 4.1 CMake配置 (`CMakeLists.txt`)

```cmake
# 发现错误恢复策略配置文件
file(GLOB EMASTER_ERROR_RECOVERY_POLICY_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/error_recovery_policies/*.json")

# 定义生成目标
set(EMASTER_GENERATED_ERROR_RECOVERY "${EMASTER_GENERATED_DIR}/error_recovery_config.c")
```

### 4.2 配置生成命令 (`src/config/CMakeLists.txt`)

```cmake
add_custom_command(
    OUTPUT "${EMASTER_GENERATED_ERROR_RECOVERY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EMASTER_GENERATED_DIR}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_error_recovery_config.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../config/error_recovery_policies"
        "${EMASTER_GENERATED_ERROR_RECOVERY}"
    DEPENDS
        ${EMASTER_ERROR_RECOVERY_POLICY_FILES}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_error_recovery_config.py"
    COMMENT "生成错误恢复策略配置"
    VERBATIM)

add_library(emaster_config STATIC
    "${EMASTER_GENERATED_CONFIG}"
    "${EMASTER_GENERATED_ERROR_RECOVERY}")
```

## 5. 设计优势

### 5.1 可维护性
- **配置与代码分离**：修改容错策略不需要修改C代码
- **清晰的职责划分**：配置层、接口层、业务层各司其职
- **易于理解**：配置文件直观可读，策略参数一目了然

### 5.2 可扩展性
- **新增策略简单**：只需添加新的JSON文件
- **新增恢复机制**：扩展配置结构体，业务逻辑解耦
- **向后兼容**：未配置策略时使用保守默认行为

### 5.3 灵活性
- **按部署定制**：不同部署可以选用不同的容错策略
- **按环境优化**：干扰环境用宽容策略，关键应用用严格策略
- **运行时查询**：支持动态查询所有可用策略

### 5.4 工程化
- **编译时验证**：配置错误在编译期发现，不等到运行时
- **类型安全**：C结构体提供类型检查
- **零运行时开销**：配置编译为静态数据，查询开销最小

## 6. 后续扩展计划

### 6.1 AL状态恢复 (待实现)
```json
"al_state_recovery": {
  "enabled": true,
  "check_interval_cycles": 100,
  "max_recovery_attempts": 3
}
```

### 6.2 CiA 402故障恢复 (待实现)
```json
"cia402_fault_recovery": {
  "enabled": true,
  "max_reset_attempts": 3,
  "recoverable_error_codes": [8600, 8611, 8310]
}
```

### 6.3 周期超时恢复 (待实现)
```json
"deadline_recovery": {
  "enabled": true,
  "consecutive_error_threshold": 10
}
```

## 7. 验证清单

### 7.1 编译验证
- [ ] 配置生成器能够正确解析所有策略JSON
- [ ] 生成的C代码能够编译通过
- [ ] 链接时找到所有符号

### 7.2 功能验证
- [ ] Session初始化时能够正确加载策略
- [ ] WKC错误容错使用配置的阈值
- [ ] 不同策略产生不同的容错行为
- [ ] 未配置策略时使用保守默认行为

### 7.3 真机验证
- [ ] 默认策略（5次/50次）在双从站系统上验证通过
- [ ] 严格策略能够更快触发停机
- [ ] 宽容策略能够容忍更多瞬态错误

## 8. 文档更新

### 8.1 配置指南
- 如何选择合适的错误恢复策略
- 如何创建自定义策略
- 各策略参数的含义和影响

### 8.2 开发指南
- 如何添加新的错误恢复机制
- 配置生成器的工作原理
- 策略加载和应用流程

## 9. 总结

本次配置解耦重构实现了：

1. **分离关注点**：配置、接口、实现三层解耦
2. **工程化设计**：编译期生成、类型安全、零运行时开销
3. **可扩展架构**：支持多种错误恢复机制，易于扩展
4. **灵活配置**：按部署定制策略，适应不同应用场景

这为后续实现更复杂的错误恢复机制（AL状态恢复、CiA 402故障恢复）奠定了坚实的架构基础。
