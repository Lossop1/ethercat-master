# EtherCAT 主站

本仓库是面向多轴机器人的 EtherCAT 主站工程。项目使用 SOEM 作为 EtherCAT
协议实现，并将设备知识、实时执行、CiA 402 控制、安全策略和应用接口划分为独立模块。

## 当前状态

第一阶段正在进行。主站已接通从配置、PDO 映像、DC、SAFE-OP/OP 到 CiA 402 运行使能的周期链路。
当前控制会话用 `6064` 实际位置初始化 CSP 的 `607A` 保持目标，并已接入由部署显式选择的相对
位置运动方案；首个台架方案尚待真机验证。
指纹和 DC 准备工具仍止于 PRE-OP；主程序退出时发送安全输出、关闭 Sync0 并恢复 INIT。

仓库当前提供：

- 固定到 v2.0.0 的 SOEM 子模块；
- 由 JSON 生成的强类型设备目录；
- 由拓扑、运行方案和部署配置生成的只读运行时目录；
- 从中文 JSON 资源生成、带格式参数校验的只读提示文本表；
- 由配置定义的产品拓扑和当前台架拓扑；
- 相互分离的设备、拓扑、运行方案和部署配置及其离线校验；
- Linux 专用的 PRE-OP 指纹工具，只执行 SDO 读取；
- Linux 专用的 PRE-OP DC 准备工具，只使用已批准运行方案写入并读回同步对象和操作模式，随后关闭 Sync0 并恢复 INIT；
- Linux 主程序，按运行方案建立周期控制会话，核对 PDO 和工作计数器并执行 CiA 402 状态转换；
- Linux 专用的 PRE-OP 指纹工具按从站返回值只读发现完整 PDO 分配和映射；
- 独立于 SOEM 的原始 PDO 位域编解码库，供后续过程数据会话复用；
- 独立于总线的 CiA 402 状态字解析和生命周期控制字规划模块，默认安全停止且不自动复位故障；
- 独立于总线的多轴完整帧协调模块，校验轴集合、序号和截止时间并拒绝发布部分帧；
- 独立于总线的原始过程数据周期交换门，通过适配回调核对 WKC、序号和截止时间；
- 独立于总线的默认拒绝安全许可门，汇总拓扑、PDO、通信、命令、反馈和显式使能授权条件；
- 只读的 ESI 事实检查器，用于输出设备身份、PDO、同步和协议能力候选，不修改项目配置；
- 首版架构、安全、实时指标和验收约束。

供应商原始资料位于本地 `docs/lz-joint/`，由于尚无再分发授权，不提交到公开仓库，也不参与
构建或项目配置校验。需要调查 ESI 时，由开发者显式运行只读检查器。

## 构建

目标平台为 Linux：

```sh
git submodule update --init --recursive
cmake --preset linux-debug
cmake --build --preset linux-debug
```

在非 Linux 主机上进行不包含 SOEM 硬件工具的离线构建：

```sh
cmake -S . -B build/host-debug -DEMASTER_BUILD_HARDWARE_TOOLS=OFF
cmake --build build/host-debug
```

离线检查只证明构建、链接、严格告警、静态分析和项目配置一致性，不能证明 EtherCAT 协议或运动逻辑。
查看一份 ESI 的只读事实报告（输出到标准输出，不生成或批准设备配置）：

```sh
python3 tools/inspect_esi.py docs/lz-joint/ECAT_CIA402.xml
```

## 硬件指纹

`emaster-fingerprint` 不是被动网络监听器。EtherCAT 发现会发送帧，`ecx_config_init()` 会请求
PRE-OP。工具要求操作者明确确认；它不会映射 PDO、配置分布式时钟、请求 SAFE-OP/OP 或写入
SDO，并会在退出前尝试恢复 INIT。

```sh
build/linux-debug/tools/fingerprint/emaster-fingerprint deployments
sudo build/linux-debug/tools/fingerprint/emaster-fingerprint \
  capture fingerprint.json
```

工具按当前主机名唯一选择部署。部署配置决定专用 EtherCAT 网卡和预期拓扑；禁止通过命令行覆盖
网卡、拓扑、状态上限或协议参数。`capture` 会要求操作者在交互终端输入 `PRE-OP`。禁止在管理、
激光雷达或生产网络接口上运行此工具。运行前必须遵守
[硬件指纹流程](docs/testing/hardware-fingerprint.md)。

按批准方案执行一次 PRE-OP DC 准备（不会使能电机或进入 SAFE-OP/OP）：

```sh
sudo build/linux-debug/tools/dc_prepare/emaster-dc-prepare prepare-dc
```

命令按当前主机唯一部署读取网卡、拓扑和运行参数，要求交互确认令牌；它会通过 SOEM SDO API 写入并读回
`1C32:01`、`1C33:01`、`6060:00` 并尝试读取 `6061:00`，调用 SOEM DC/Sync0 接口并校验寄存器，退出前关闭 Sync0、恢复 INIT。
`6061` 是只读 PDO 反馈；若设备在 PRE-OP 不提供有效值，命令会记录该事实但不会伪造通过，必须在后续 SAFE-OP/OP 周期通信中验证。
任何身份、PDO、SDO 或 DC 校验失败都会停止后续轴并执行同样的回退流程。运行前必须确认电机处于可安全测试状态，
并遵守项目负责人批准的运行方案。

按当前主机部署启动控制会话：

```sh
sudo build/linux-debug/tools/master/emaster-master
```

命令不接受网卡、拓扑、周期、模式或轴数参数，只使用当前主机的部署和运行方案。它会发现并严格
核对当前 PDO，配置过程数据映像和 DC，在 SAFE-OP 读取 `6064` 初始化 CSP 保持目标，进入 OP 后
按 `6041` 周期计算 `6040`，直到收到 `Ctrl+C` 或 `SIGTERM`。当前会话会使能驱动器，但保持启动时
实际位置，不接收变化运动目标；退出和失败路径都会按状态反馈撤销使能，满足设备配置的停用条件后
关闭 Sync0 并恢复 INIT。

## 仓库结构

- `config/devices/`：设备型号和经过审查的设备事实；
- `config/topologies/`：任意规模的逻辑从站序列；
- `config/deployments/`：主机、专用 EtherCAT 网口、拓扑和已批准运行方案；
- `config/operation_profiles/`：引用设备 PDO 方案的同步、周期和模式候选；
- `config/messages/`：面向操作者的中文提示正文，禁止在业务代码中重复嵌入；
- `include/emaster/`：强类型模块契约，禁止出现 SOEM 类型；
- `src/catalog/`：平台无关的设备目录；
- `src/config/`：拓扑、运行方案和部署配置生成目标；
- `src/messages/`：本地化消息资源生成目标；
- `src/bus/soem/`：唯一允许调用 SOEM 的代码层；
- `tools/fingerprint/`：受限的 PRE-OP 操作工具和证据格式；
- `tools/master/`：主站进程入口，只负责装配配置、信号处理和结果输出；
- `docs/`：需求、决策、供应商资料说明和测试流程；
- `external/SOEM/`：固定版本的上游依赖。

模块按独立契约、依赖边界、失效模型和相应硬件证据逐个交付；尚未开发的运行时模块不会预先创建空目录。
架构边界和当前阶段门槛见 [`docs/architecture/overview.md`](docs/architecture/overview.md) 与
[`docs/requirements/phase-1.md`](docs/requirements/phase-1.md)。

配置字段、引用关系和安全边界见 [`config/README.md`](config/README.md)。

## 许可证

SOEM 2.0.0 采用 GPLv3 或商业许可证双重授权。本项目尚未确定最终许可证。在项目负责人选择
并记录兼容的授权方式之前，禁止闭源或商业化发布。
