# 配置模型

## 目的

配置目录只保存经过审查的工程输入，不保存运行时状态。设备型号、逻辑拓扑和物理部署分别变化，
因此必须分开管理。通用代码不得通过文件名、网卡名或从站数量推断产品与台架环境。

## 目录职责

| 目录 | 描述的事实 | 允许引用 | 禁止包含 |
|---|---|---|---|
| `devices/` | 型号身份、协议能力、PDO、换算默认值、受控资料来源 | ESI 和供应商资料 | 轴号、主机名、网卡名 |
| `topologies/` | 按总线顺序排列的逻辑从站及其设备配置 | `profile_id` | 主机名、网卡名、阶段安全授权 |
| `operation_profiles/` | 设备 PDO 方案上的同步、周期和模式候选 | `device_profile_id`、`pdo_set_id` | 主机名、网卡名、未批准默认值 |
| `deployments/` | 某台主机使用哪个物理接口运行哪个拓扑及已批准方案 | `topology_id`、`operation_profile_ids` | 设备协议细节、放宽工具能力的开关 |
| `messages/` | 面向操作者的本地化提示资源 | 消息 ID | EtherCAT 行为参数 |

JSON 不支持注释，本文件是字段语义的唯一配套说明；新增字段时必须同时更新校验器和本文件。

## 设备配置

每个 `devices/*.json` 描述一个可由 EtherCAT 身份三元组精确识别的设备修订版。

- `profile_id`：跨配置引用的稳定且唯一的标识；发布后不得在不迁移引用的情况下修改；
- `identity`：供应商 ID、产品代码和修订号，必须与受控 ESI 及物理指纹一致；
- `pdo_sets`：供应商声明的全部 PDO 方案；每个方向由有序的 `mappings` 数组组成，条目位长总和必须与声明字节数一致；
- `reference_pdo_set_id`：当前指纹基线使用的静态参照，只用于证据核对，不是运行时选择；
- `protocol.supports_pdo_configuration`：ESI 的 `CoE/PdoConfig` 能力；为 `false` 时禁止通过 SDO 重映射；
- `protocol.supports_distributed_clocks`：ESI 声明的能力，不表示所有运行方案都必须启用 DC；
- `conversion`：设备资料提供的换算来源和默认值，不能替代每台物理从站的采集值；
- `source`：受控 ESI 的仓库相对路径和 SHA-256。

构建系统扫描全部 `devices/*.json`，按 `profile_id` 稳定排序后生成只读 C 目录。增加新型号不应修改
生成器或 CMake 中的设备文件名。

## 拓扑配置

每个 `topologies/*.json` 描述一个非空、任意规模的逻辑从站序列。

- `topology_id`：供部署引用的稳定且唯一的标识；
- `status`：该实例的证据状态，例如暂定、已观察或已批准；
- `slaves`：按 EtherCAT 物理顺序排列，采用与 SOEM 探测报告一致的 `1..N` 位置；`N` 只由该
  实例的实际列表长度决定，不代表固定产品轴数；
- `axis_id`：拓扑内唯一的逻辑轴标识；
- `joint_assignment`：产品物理关节分配，未确认时必须为 `null`，不能臆测；
- `profile_id`：引用已存在的设备配置。

`status` 只记录该拓扑的证据状态（例如 `provisional`、`observed` 或 `approved`）。配置基础设施
原样保存该字段，不把任何字符串擅自解释为安全授权；是否允许某个硬件操作由对应流程的明确门槛
决定。未确认的工程参数必须保持缺失或 `null`，不能由生成器补入默认值。

每个 PDO 条目必须声明 `index`、`subindex`、`bits`、`data_type` 和稳定的 `name`。填充只能使用
`0x0000:00`、`PADDING` 和正定位位宽表示。`data_type` 来自受控 ESI，用于解释物理描述符，不是
主站运行时自动推断的控制参数。

`robot_12_axis.json` 是最终产品目标的具体实例；`bench_single_slave.json` 是当前台架连接事实的具体
实例。它们共用同一套生成、发现和校验机制，不存在“单轴版主站”或“12 轴版主站”。

## 部署配置

每个 `deployments/*.json` 把逻辑拓扑绑定到明确的主机网络资源。

- `deployment_id`：部署记录的稳定且唯一的标识；
- `hostname`：运行主站的主机名；
- `topology_id`：该部署选择的逻辑拓扑；
- `ethercat_interface`：只承载 EtherCAT 二层帧的物理接口；
- `management_interface`：可选的管理接口，用于 SSH、NTP 等 IP 服务，禁止与 EtherCAT 接口相同。

同一主机和 EtherCAT 接口只能由一个仓库部署记录占用。`enp49s0` 只出现在 Orange Pi 部署实例
和测试证据中，通用代码与拓扑配置均不感知该名称。

构建阶段会把拓扑、运行方案和部署配置生成只读 C 目录。运行时通过 `deployment_id` 查找部署，接口、
主机、拓扑和从站顺序均从该目录取得；不提供绕过配置直接指定网卡、拓扑或 EtherCAT 参数的接口。

## 运行方案

`operation_profiles/` 中的每个 JSON 是一个候选方案，必须明确引用设备目录中的 `pdo_set_id`。同步
策略之外的 `selected_mode_id`、AssignActivate、周期、Sync0 偏移及 SM2/SM3 同步类型均可为
`null`，表示尚未由项目负责人确认；生成器不会补默认值。方案状态为 `approved` 后，部署才可以
引用它，且实际模式和所有必需同步参数都必须存在。模式值必须与设备目录的 ESI 事实一致，模式
声明的收发字段必须实际存在于所选 PDO 方案。

部署中的 `operation_profile_ids` 为空表示禁用过程数据会话；非空时必须为拓扑使用的每种设备配置
恰好启用一个已批准方案，且所有方案周期一致。会话启动命令不接受模式、周期或同步参数覆盖。

当前 Orange Pi 部署只用于 PRE-OP 指纹采集，未激活任何运行方案；仓库中的 SM/DC 文件均为草案。

## 提示资源

`messages/zh_CN.json` 是当前中文提示的唯一正文来源。构建阶段由 `tools/generate_messages.py` 生成
只读 C 表，并校验每条 `printf` 格式参数；业务代码只引用消息 ID，不得嵌入操作者提示文本。

## 安全授权

第一阶段工具的最高能力由代码边界固定为 PRE-OP：不映射 PDO、不配置 DC、不请求 SAFE-OP/OP、
不写 SDO。普通 JSON 不能提高这一能力，因此当前不建立可修改的阶段策略配置。未来确有多个经
批准的策略时，应使用独立策略层、签名或发布门，并接受单独安全评审。
