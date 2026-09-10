# EtherCAT 拓扑映射重构设计

## 当前问题

### 硬编码假设
```c
// session_setup.c:80
if (slave_count != session->plan->axis_count) {
    return EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;  // 硬失败
}

// session_setup.c:92
slave = &session->context.slavelist[axis_index + 1U];  // 假设配置顺序 = 总线顺序
```

**问题**：
1. 配置2轴，但总线只扫到1个 → 失败（今天早上的问题）
2. 无法处理跳跃位置（如总线位置2,5,7）
3. 无法容忍额外的未配置从站
4. 监控工具硬编码轴数

## 解决方案

### 阶段1：放宽拓扑验证（兼容性修复）

**目标**：允许 `slave_count >= axis_count`，按配置顺序映射

```c
// 允许总线上有更多从站
if (slave_count < session->plan->axis_count) {
    return EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;  // 只在太少时失败
}

// 映射：配置中第i轴 → 总线第(i+1)个从站
for (axis_index = 0; axis_index < session->plan->axis_count; ++axis_index) {
    if ((int)(axis_index + 1) > slave_count) {
        return EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;
    }
    slave = &session->context.slavelist[axis_index + 1U];
    session->axes[axis_index].position = (uint16_t)(axis_index + 1U);
    // ... 现有逻辑
}
```

**优点**：
- 最小改动
- 向后兼容
- 立即修复今天早上的问题

**缺点**：
- 仍然假设顺序映射
- 不支持按身份匹配

### 阶段2：身份匹配映射（架构改进）

**目标**：根据 VendorID/ProductCode 动态匹配

```c
// 1. 扫描所有从站，建立身份表
for (int i = 1; i <= slave_count; i++) {
    slave = &context.slavelist[i];
    discovered[i-1] = {
        .bus_position = i,
        .vendor_id = slave->eep_man,
        .product_code = slave->eep_id,
        .revision = slave->eep_rev,
        .assigned = false
    };
}

// 2. 为每个配置轴找到匹配的从站
for (axis_index = 0; axis_index < axis_count; ++axis_index) {
    axis_config = &plan->axes[axis_index];
    
    // 查找未分配的匹配从站
    matched_slave = find_unassigned_slave_by_identity(
        discovered, 
        slave_count,
        axis_config->device_profile
    );
    
    if (!matched_slave) {
        ERROR("配置轴%zu 未找到匹配从站", axis_index);
        return TOPOLOGY_MISMATCH;
    }
    
    // 建立映射
    session->axes[axis_index].position = matched_slave->bus_position;
    matched_slave->assigned = true;
    
    // 后续配置使用 session->axes[axis_index].position
    sdo_context_init(&sdo, &context, session->axes[axis_index].position, ...);
}

// 3. 警告未分配的从站
for (int i = 0; i < slave_count; i++) {
    if (!discovered[i].assigned) {
        WARN("总线位置%d 从站未配置: VID=0x%08X PID=0x%08X",
             discovered[i].bus_position,
             discovered[i].vendor_id,
             discovered[i].product_code);
    }
}
```

**优点**：
- 支持任意总线位置
- 支持跳跃位置
- 容忍未配置从站
- 真正的声明式配置

**缺点**：
- 较大改动
- 需要仔细测试

### 阶段3：运行时查询API

**目标**：暴露实际拓扑和参数给工具层

```c
// 新增API: include/emaster/bus/control_session.h

typedef struct {
    size_t axis_count;
    uint16_t *bus_positions;           // 每轴的总线位置
    uint32_t *encoder_resolutions;     // 每轴编码器分辨率
    uint32_t *gear_ratios_num;         // 齿轮比分子
    uint32_t *gear_ratios_den;         // 齿轮比分母
    uint32_t *rated_torques_mnm;       // 额定力矩(mNm)
} emaster_runtime_topology_t;

// 查询实际拓扑
bool emaster_control_session_query_topology(
    const char *socket_path,
    emaster_runtime_topology_t *topology
);

// 释放查询结果
void emaster_runtime_topology_free(emaster_runtime_topology_t *topology);
```

**工具层使用**：
```c
// emaster-watch: 动态查询轴数和参数
emaster_runtime_topology_t topo;
if (!emaster_control_session_query_topology(socket_path, &topo)) {
    ERROR("无法查询主站拓扑");
    return 1;
}

printf("发现 %zu 轴:\n", topo.axis_count);
for (size_t i = 0; i < topo.axis_count; i++) {
    printf("  轴%zu: 总线位置=%u 编码器=%u脉冲/转 齿轮比=%u:%u\n",
           i, topo.bus_positions[i], topo.encoder_resolutions[i],
           topo.gear_ratios_num[i], topo.gear_ratios_den[i]);
}

emaster_runtime_topology_free(&topo);
```

## 实施计划

### 立即执行（修复今天的问题）
- [x] 阶段1：放宽 `slave_count != axis_count` 为 `slave_count < axis_count`

### 短期（本周）
- [ ] 阶段2：实现身份匹配映射
- [ ] 添加详细的拓扑诊断日志

### 中期（下周）
- [ ] 阶段3：运行时查询API
- [ ] 工具层动态适配
- [ ] 添加角度单位接口

## 风险

1. **向后兼容性**：现有配置文件假设顺序映射
   - 缓解：阶段1保持向后兼容
   - 阶段2添加显式配置选项

2. **测试覆盖**：需要各种拓扑组合测试
   - 1轴、2轴、跳跃位置、多余从站

3. **性能**：身份匹配增加初始化时间
   - 影响可忽略（只在启动时执行一次）
