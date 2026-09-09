# 归档文档说明

**归档日期**: 2026-09-09  
**原因**: 项目文档整理，移除过期或已完成的临时文档

## 归档内容

### development/
- `industrial_roadmap.md` - 工业级路线图快照（2026-09-09）
- `task_1.2_network_binding_audit.md` - 网卡绑定审计报告（2026-09-09）

**状态**: 快照文档，任务已完成。网卡解耦已验证通过，路线图中的部分任务已在后续开发中完成。

### investigation/
- `6061.txt` - 驱动器模式显示问题简要记录
- `6061问题/` - 详细调查记录（抓包分析、过程概述）
- `6061.zip` - 相关抓包数据
- `ethercat-official-baseline.md` - EtherCAT官方资料调查基线（2026-09-02）

**状态**: 早期问题调查记录。关键发现已整合到memory系统和代码修复中。保留用于历史追溯。

### environment-findings.md
早期环境检查记录（2026-08-31 至 2026-09-01）

**状态**: Orange Pi台架环境初始配置记录。当前环境状态和访问方式已记录在memory中（`ethercat-orangepi-bench-access.md`）。

## 相关资源

当前有效文档位于：
- `docs/realtime/baseline.md` - 实时性设计基线（包含关键timing参数）
- `docs/testing/hardware-fingerprint.md` - 硬件指纹采集流程
- `.claude/projects/*/memory/` - 持久化的项目知识和配置
