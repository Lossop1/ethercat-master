# 动态误差检测实机测试计划

测试提交：9983446

测试目标：验证每轴独立误差阈值功能

---

## 1. 测试前准备

### 1.1 代码同步

```bash
# 在 OrangePi 上
cd ethercat-master
git fetch origin
git reset --hard origin/main
git log -1 --oneline  # 应显示 9983446

cd build
make clean
make -j4
```

### 1.2 创建测试配置

创建 `config/motions/test-different-thresholds.json`：

```json
{
  "motion_profile_id": "test-different-thresholds",
  "status": "approved",
  "required_mode_id": "csp",
  "trajectory": "relative_linear_position",
  "coordinate": "output_shaft",
  "duration_ms": 5000,
  "settle_ms": 500,
  "axes": [
    {
      "axis_id": "bench_axis_01",
      "relative_angle_millidegrees": 36000,
      "acceleration_millidegrees_per_second2": 50000,
      "deceleration_millidegrees_per_second2": 50000,
      "max_following_error_millidegrees": 3000,
      "max_velocity_error_millidegrees_per_second": 10000,
      "expected_position_scale": {
        "encoder_increments": 16384,
        "encoder_motor_revolutions": 1,
        "gear_motor_revolutions": 11,
        "gear_shaft_revolutions": 158
      }
    },
    {
      "axis_id": "bench_axis_02",
      "relative_angle_millidegrees": 36000,
      "acceleration_millidegrees_per_second2": 50000,
      "deceleration_millidegrees_per_second2": 50000,
      "max_following_error_millidegrees": 10000,
      "max_velocity_error_millidegrees_per_second": 10000,
      "expected_position_scale": {
        "encoder_increments": 16384,
        "encoder_motor_revolutions": 1,
        "gear_motor_revolutions": 11,
        "gear_shaft_revolutions": 158
      }
    }
  ]
}
```

**注意**：轴1阈值 3° (3000 millidegrees)，轴2阈值 10° (10000 millidegrees)

### 1.3 更新部署配置

修改 `config/deployments/orangepi_bench.json`，添加：
```json
{
  "motion_profile_id": "test-different-thresholds",
  "fault_policy": "axis_isolation"
}
```

---

## 2. 测试场景

### 场景 1：正常运行（基线）

**目的**：验证两轴使用不同阈值能正常运行

**步骤**：
```bash
sudo ./build/tools/master/emaster-master \
  --deployment config/deployments/orangepi_bench.json \
  --motion test-different-thresholds
```

**观察**：
1. 两轴启动成功
2. 运动完成
3. 查看报告中的 `max_observed_following_error_counts`

**预期结果**：
- 轴1：`max_observed_following_error_counts` < 3° 对应的 counts
- 轴2：`max_observed_following_error_counts` < 10° 对应的 counts
- 无故障触发

**计算阈值对应的 counts**：
```
gear_ratio = 11 / 158 = 0.0696
encoder_resolution = 16384 counts/rev

3° 对应 counts = (3000 × 16384 × 11) / (360000 × 1 × 158) ≈ 9482 counts
10° 对应 counts = (10000 × 16384 × 11) / (360000 × 1 × 158) ≈ 31607 counts
```

---

### 场景 2：人工触发轴1超限

**目的**：验证轴1独立阈值生效

**方法**：物理阻挡轴1

**步骤**：
1. 启动运动
2. 在轴1运动过程中用手轻轻按住（增加负载）
3. 观察终端输出

**预期结果**：
- 终端输出：`[P2.5] 轴0 故障隔离: 原因=FOLLOWING_ERROR, 周期=XXX`
- 轴1进入 Quick-Stop
- 轴2继续运行（因为配置了 `axis_isolation`）
- 会话不退出

**失败模式**：
- 如果两轴都停止 → 全局阈值逻辑残留或策略未生效
- 如果轴1没有触发故障 → 阈值计算错误或检测逻辑失效

---

### 场景 3：人工触发轴2超限

**目的**：验证轴2的宽松阈值生效

**步骤**：
1. 重启会话
2. 在轴2运动过程中用手阻挡

**预期结果**：
- 轴2需要更大的阻力才触发故障（因为阈值是 10° vs 3°）
- 触发后：`[P2.5] 轴1 故障隔离: 原因=FOLLOWING_ERROR`
- 轴1继续运行

**对比验证**：
- 同样的阻力，轴1更容易触发故障
- 轴2需要更严重的阻挡才触发

---

### 场景 4：全局停止策略对比

**目的**：验证策略切换后行为改变

**步骤**：
1. 修改部署配置：`"fault_policy": "global_stop"`
2. 重新生成配置：`python tools/generate_runtime_config.py ...`
3. 重新编译：`cd build && make`
4. 重复场景2

**预期结果**：
- 轴1触发故障后，**两轴都停止**
- 会话退出
- 报告显示：`status = FOLLOWING_ERROR`

**对比**：
- `axis_isolation`：单轴故障，其他轴继续
- `global_stop`：单轴故障，全部停止

---

## 3. 数据收集

### 3.1 报告字段验证

每次测试后检查 `run_report_path` 中的 JSON：

```json
{
  "axes": [
    {
      "axis_id": "bench_axis_01",
      "max_following_error_counts": 9482,     // 应为 3° 对应值
      "max_observed_following_error_counts": 8500,  // 实际观测值
      "axis_status": 0,                       // NORMAL
      "fault_isolated": false
    },
    {
      "axis_id": "bench_axis_02",
      "max_following_error_counts": 31607,    // 应为 10° 对应值
      "max_observed_following_error_counts": 9200,
      "axis_status": 0,
      "fault_isolated": false
    }
  ]
}
```

### 3.2 关键指标

| 指标 | 轴1 (3°) | 轴2 (10°) | 说明 |
|------|----------|-----------|------|
| 配置阈值 (millidegrees) | 3000 | 10000 | 来自配置文件 |
| 转换后阈值 (counts) | ~9482 | ~31607 | 计算值 |
| 实际观测最大误差 (counts) | ? | ? | 从报告读取 |
| 触发故障阈值 (手动测试) | ? | ? | 物理阻挡实验 |

### 3.3 终端日志

关键日志标识：
```
[P2.5] 轴X 故障隔离: 原因=FOLLOWING_ERROR, 周期=YYYY
```

记录：
- 哪个轴触发
- 触发时的周期数
- 其他轴是否继续运行

---

## 4. 回归验证

### 4.1 向后兼容性

**测试**：运行不带运动方案的会话
```bash
sudo ./build/tools/master/emaster-master \
  --deployment config/deployments/orangepi_bench.json \
  --motion none
```

**预期**：
- 会话正常启动
- 无跟随误差检查（因为 `max_following_error_counts = 0`）
- 正常退出

### 4.2 旧配置兼容

**测试**：使用没有 `max_following_error_millidegrees` 字段的旧运动配置

**预期**：
- 配置验证失败，或
- 默认值为 0，跳过检查

---

## 5. 故障排查

### 5.1 如果阈值不生效

**检查步骤**：
1. 确认配置已重新生成：
   ```bash
   strings build/generated/runtime_config.c | grep max_following_error
   ```
2. 确认报告中的 `max_following_error_counts` 字段非零
3. 添加调试日志：
   ```c
   fprintf(stderr, "[DEBUG] 轴%zu: error=%lu, threshold=%lu\n",
           axis_index, following_error, max_error);
   ```

### 5.2 如果全局阈值仍在使用

**检查**：
```bash
grep -n "position_target_max_following_error_counts" \
  src/bus/soem/session_control.c
```

应该找不到在误差检测中使用该字段的代码（已被移除）。

### 5.3 如果单位转换错误

**验证计算**：
```c
// 在 session_start.c:91 后添加
fprintf(stderr, "[DEBUG] 轴%zu: millideg=%u, counts=%lu\n",
        axis_index,
        session->motion_axis_configs[axis_index]->max_following_error_millidegrees,
        session->axes[axis_index].max_following_error_counts);
```

对比手动计算值。

---

## 6. 成功判据

**核心验证**：
- [ ] 报告中 `max_following_error_counts` 字段与配置匹配
- [ ] 轴1用 3° 阈值，轴2用 10° 阈值（独立，不相同）
- [ ] 物理阻挡能触发单轴故障
- [ ] `axis_isolation` 策略下其他轴继续运行
- [ ] `global_stop` 策略下全部停止

**数据完整性**：
- [ ] 每轴的 `max_observed_following_error_counts` 正确记录
- [ ] 故障时的 `fault_reason` 正确
- [ ] 隔离轴的 `fault_isolated = true`

**向后兼容**：
- [ ] 无运动方案时不触发误差检查
- [ ] 阈值为 0 的轴跳过检查

---

## 7. 已知限制

1. **运动方案必需**：动态误差检测只在有运动方案时生效（因为阈值来自运动配置）
2. **外部目标路径**：当前外部目标使用旧的全局阈值逻辑（需要后续统一）
3. **单位依赖**：阈值计算依赖正确的 `position_scale` 读回

---

## 8. 后续工作

测试通过后：
1. 更新 `p2.5-verification-plan.md`，记录动态误差检测结果
2. 补充 JSON 报告字段输出（`axis_status`、`fault_isolated` 等）
3. 实现 `emaster-control recover-axis` 命令工具
4. 完整的硬件验证（12轴系统）
