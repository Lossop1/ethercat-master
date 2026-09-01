# EtherCAT 主站

本仓库是面向 12 轴机器人的 EtherCAT 主站全新工程基线。项目使用 SOEM 作为 EtherCAT
协议实现，并将设备知识、实时执行、CiA 402 控制、安全策略和应用接口划分为独立模块。

## 当前状态

第一阶段正在进行。项目尚未实现驱动器使能和运动控制。PRE-OP 上限由受限指纹工具的代码能力
保证，不能通过普通配置提高。

仓库当前提供：

- 固定到 v2.0.0 的 SOEM 子模块；
- 由 JSON 生成、并与供应商 ESI 校验的强类型设备目录；
- 由拓扑和部署配置生成的只读运行时目录；
- 作为产品目标的暂定 12 位置拓扑和作为当前台架事实的单从站拓扑；
- 相互分离的设备、拓扑和部署配置及其离线校验；
- Linux 专用的 PRE-OP 指纹工具，只执行 SDO 读取；
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

## 仓库结构

- `config/devices/`：设备型号和经过审查的设备事实；
- `config/topologies/`：任意规模的逻辑从站序列；
- `config/deployments/`：主机、专用 EtherCAT 网口和所选拓扑；
- `include/emaster/`：强类型模块契约，禁止出现 SOEM 类型；
- `src/catalog/`：平台无关的设备目录；
- `src/config/`：拓扑和部署配置生成目标；
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
