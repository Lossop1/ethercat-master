# EtherCAT 主站

本仓库是面向 12 轴机器人的 EtherCAT 主站全新工程基线。项目使用 SOEM 作为 EtherCAT
协议实现，并将设备知识、实时执行、CiA 402 控制、安全策略和应用接口划分为独立模块。

## 当前状态

第一阶段正在进行。项目尚未实现驱动器使能和运动控制。指纹和 DC 准备工具的能力上限由代码
固定在 PRE-OP；独立的 CIA 402 前置工具可在额外确认后临时请求 SAFE-OP/OP，但只发送安全首帧
并在退出前恢复 INIT，普通配置不能提高任何工具的能力。

仓库当前提供：

- 固定到 v2.0.0 的 SOEM 子模块；
- 由 JSON 生成、并与供应商 ESI 校验的强类型设备目录；
- 由拓扑、运行方案和部署配置生成的只读运行时目录；
- 从中文 JSON 资源生成、带格式参数校验的只读提示文本表；
- 作为产品目标的暂定 12 位置拓扑和作为当前台架事实的单从站拓扑；
- 相互分离的设备、拓扑、运行方案和部署配置及其离线校验；
- Linux 专用的 PRE-OP 指纹工具，只执行 SDO 读取；
- Linux 专用的 PRE-OP DC 准备工具，只使用已批准运行方案写入并读回同步对象和操作模式，随后关闭 Sync0 并恢复 INIT；
- Linux 专用的 CiA 402 前置工具，按已批准运行方案验证 SAFE-OP/OP 首帧、工作计数器和 `6061` 反馈，随后关闭 Sync0 并恢复 INIT，不执行状态转换；
- Linux 专用的 PRE-OP 指纹工具按从站返回值只读发现完整 PDO 分配和映射；
- 独立于 SOEM 的原始 PDO 位域编解码库，供后续过程数据会话复用；
- 独立于总线的 CiA 402 状态字解析和生命周期控制字规划模块，默认安全停止且不自动复位故障；
- 独立于总线的多轴完整帧协调模块，校验轴集合、序号和截止时间并拒绝发布部分帧；
- 首版架构、安全、实时指标和验收约束。

供应商原始资料位于本地 `docs/lz-joint/`，由于尚无再分发授权，不提交到公开仓库。
预期路径和 SHA-256 记录在 `docs/vendor/SHA256SUMS`。安装本地受控资料后，项目校验会检查
文件内容及设备目录与 ESI 的一致性。公开仓库的干净检出只校验元数据，并报告受控资料缺失。

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

离线检查只证明构建、链接、严格告警、静态分析和工程资料一致性，不能证明 EtherCAT 协议或运动逻辑。
工程发布检查必须要求全部受控供应商资料存在：

```sh
python3 tools/validate_project.py --root . --require-vendor-artifacts
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

按批准方案执行一次 SAFE-OP/OP 首帧前置验证（不会使能电机或产生运动）：

```sh
sudo build/linux-debug/tools/cia_preflight/emaster-cia-preflight prepare-cia
```

命令只使用部署配置中的网卡、拓扑和运行方案，要求输入 `SAFE-OP-CIA` 确认；它会发现并严格
核对当前 PDO，写入并读回 `1C32:01`、`1C33:01`、`6060:00`，配置过程数据映像和 DC，先以零
控制字及零目标发送首帧，再请求 SAFE-OP/OP 并解码 `6061`。验证后关闭 Sync0、恢复 INIT。
该流程不执行 CiA 402 控制字状态转换，不提供使能或运动接口。

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
- `docs/`：需求、决策、供应商资料说明和测试流程；
- `external/SOEM/`：固定版本的上游依赖。

模块按独立契约、依赖边界、失效模型和相应硬件证据逐个交付；尚未开发的运行时模块不会预先创建空目录。
架构边界和当前阶段门槛见 [`docs/architecture/overview.md`](docs/architecture/overview.md) 与
[`docs/requirements/phase-1.md`](docs/requirements/phase-1.md)。

配置字段、引用关系和安全边界见 [`config/README.md`](config/README.md)。

## 许可证

SOEM 2.0.0 采用 GPLv3 或商业许可证双重授权。本项目尚未确定最终许可证。在项目负责人选择
并记录兼容的授权方式之前，禁止闭源或商业化发布。
