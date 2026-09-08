# EtherCAT 主站

面向多轴控制的 Linux 主站，使用 SOEM 2.0.0。设备、拓扑、运行方案和运动参数由配置选择，
CiA 402 状态转换与轨迹计算独立于总线实现。

## 当前功能基线

当前单从站 CSP 功能基线代号为 `CSP36°成功`，对应 Git 标签 `CSP36°成功` 和功能提交
`7be74e95b1a5620543c220cb80eca29aa5df5386`。该版本从此前的 `9dce12f` 工作状态恢复得到，
恢复目标是 `7be74e9`，控制逻辑没有在基线标记后改写。

该基线使用 `cyberbeast.dynamic-dc-csp-1ms` 与 `orangepi-bench.output-positive-36deg`：
单从站、DC 周期 1 ms、过程帧相位 50 us、Sync0 偏移 250 us、SM2 类型 `0x0002`、
SM3 类型 `0x0022`，输出轴相对正向 36°、轨迹时间 5 s。已在真实电机上重复完成目标发送、
实际位置跟随、停机和恢复 INIT；该基线不代表多轴产品验收。

## 构建与启动

在仓库根目录执行：

```sh
git submodule update --init --recursive
cmake --preset linux-debug
cmake --build --preset linux-debug
sudo build/linux-debug/tools/master/emaster-master
```

主程序按主机名选择唯一部署，不通过命令行覆盖网卡、轴数、周期或模式。
修改配置后重新构建。部署引用运动方案时会执行该运动；没有运动方案时保持启动位置。

控制主线为：发现与身份核对、PDO 配置、DC 配置、SAFE-OP 初始化、持续周期交换、
OP、CiA 402 使能、全轴轨迹、停用与退出。指纹和 DC 准备工具是独立诊断工具，不是启动前必跑步骤。

非 Linux 主机可构建通用模块：

```sh
cmake -S . -B build/host-debug -DEMASTER_BUILD_HARDWARE_TOOLS=OFF
cmake --build build/host-debug
```

构建只检查源码、链接和配置一致性，协议、实时性与运动行为使用真机验证；不维护持久化测试程序。

## 结构与参数

- `config/`：设备、拓扑、运行、运动、部署和中文提示，各自具有明确所有者
- `include/emaster/`：模块公共契约，不含 SOEM 类型
- `src/session/`：会话计划、布局匹配与模式条件
- `src/protocol/`：PDO 布局与编解码
- `src/cia402/`、`src/multiaxis/`、`src/motion/`：单轴状态机、全轴协调和相对位置轨迹
- `include/emaster/motion/position_command.h`、`src/motion/position_command.c`：有界多轴位置命令流
- `src/bus/soem/`：总线配置、周期收发、邮箱及生命周期执行
- `src/audit/`：执行记录与报告，周期内不输出文件
- `tools/master/`：进程入口与中文结果展示
- `external/SOEM/`：固定版本的协议栈依赖

配置字段见 [配置模型](config/README.md)，执行边界见 [架构说明](docs/architecture/overview.md)。
设备支持的动态和固定 PDO 方案保留为可选配置，轴数来自拓扑列表。

## 产物与诊断

`build/` 保存构建与生成源码，`runtime/` 保存运行记录，`tmp/` 保存临时调查材料，均不提交 Git。
主站按部署中的 `run_report_path` 原子更新最新报告。需要长期保留的真机证据另行明确归档，
不把每次运行的日志、压缩包或临时脚本加入源码目录。

报告版本为 2：首次周期异常与最后一次交换、退出时 AL 状态、退出周期后的诊断分别记录；每个从站
还聚合主机单调时间、参考 DC 时间、发送相位和 Sync0 理论裕量，便于定位周期时序问题。
未执行的 OP 即时 SDO 读取不再出现在报告中。PDO 审计预分配存储并按字段直接定位，
容量耗尽时统计未保存样本，不动态扩容或将截断冒充完整记录。

```sh
sudo build/linux-debug/tools/fingerprint/emaster-fingerprint capture runtime/reports/fingerprint.json
sudo build/linux-debug/tools/dc_prepare/emaster-dc-prepare prepare-dc
python3 tools/inspect_esi.py docs/lz-joint/ECAT_CIA402.xml
```

供应商资料保留在本地 `docs/lz-joint/`，不参与构建门控。
历史调查记录不代表当前加载配置；当前行为以部署、构建及实际运行记录为准。

## 验证范围

`CSP36°成功` 已在单从站台架上重复出现模式反馈为 CSP、位置指令产生实际运动、正常完成和
安全停机的结果。该基线只覆盖当前设备、当前 PDO 方案和当前单从站部署；周期稳定性、多轴
同步、其他运动模式和产品拓扑仍需单独验证。

## 许可证

SOEM 2.0.0 采用 GPLv3 或商业许可证双重授权。本项目尚未确定最终许可证。
