# 配置模型

## 目的

配置目录只保存经过审查的工程输入，不保存运行时状态。设备型号、逻辑拓扑和物理部署分别变化，
因此分开管理。通用代码不得通过文件名、网卡名或从站数量推断产品与台架环境。

## 交付时这里是空的

产品交付时 `devices/` `device_policies/` `topologies/` `operation_profiles/`
`motion_profiles/` `deployments/` 六族和 `esi/` 都是空的，仓库里只有代码与三族与电机无关的
配置（`error_recovery_policies/`、`messages/`、本文件）。客户用手上的 ESI 生成那六族，命令见
`tools/gen/device/README.md`。本文件举到的型号、轴数、网口只用于说明字段语义。一族可以一个
文件都没有：校验器只抓引用了不存在 ID 这类真正的不一致。

## 目录职责

| 目录 | 描述的事实 | 允许引用 | 禁止包含 |
|---|---|---|---|
| `devices/` | 型号身份、协议能力、PDO、换算默认值及其资料来源 | 经审查的设备事实 | 轴号、主机名、网卡名 |
| `device_policies/` | 人对某一型号做的选择：选哪套 PDO、模式值、停机判据 | 设备描述（`esi/`） | 轴号、主机名、网卡名 |
| `topologies/` | 按总线顺序排列的逻辑从站及其设备配置 | `profile_id` | 主机名、网卡名、阶段安全授权 |
| `operation_profiles/` | 设备 PDO 方案上的同步、周期和模式候选 | `device_profile_id`、`pdo_set_id` | 主机名、网卡名、未批准默认值 |
| `motion_profiles/` | 一次完整运动的轨迹、坐标、每轴目标和安全边界 | 拓扑轴 ID、运行模式 ID | 网卡名、PDO 字节偏移、隐式默认值 |
| `deployments/` | 某台主机使用哪个物理接口运行哪个拓扑及已批准方案 | `topology_id`、`operation_profile_ids`、`motion_profile_id` | 设备协议细节、放宽工具能力的开关 |
| `messages/` | 面向操作者的本地化提示资源 | 消息 ID | EtherCAT 行为参数 |

JSON 不支持注释，本文件是字段语义的唯一配套说明；新增字段时同时更新校验器和本文件。

## 设备配置

每个 `devices/*.json` 描述一个可由 EtherCAT 身份三元组精确识别的设备修订版。

- `profile_id`：稳定且唯一的跨配置标识，发布后修改必须迁移引用；
- `identity`：供应商 ID、产品代码、修订号，必须与物理从站一致；
- `pdo_sets`：供应商声明的全部 PDO 方案。每个方向由有序的 `mappings` 组成，条目位长总和必须
  等于声明的字节数；
- `pdo_sets[].assignment_style`：这套 PDO 在总线上如何被选中。`module_slot`（缺省）用于模块化
  设备，运行期先写 `0xF030[module_slot]` 再写 `0x1C12`/`0x1C13`；`direct_1c12` 用于没有模块
  槽位的设备，只写 `0x1C12`/`0x1C13`，此时 `module_ident` 为 `null`、`module_slot` 为 0。
  两种写法不混用；
- `reference_pdo_set_id`：指纹基线的静态参照，只用于证据核对，不参与运行时选择；
- `protocol.supports_pdo_assignment` / `supports_pdo_configuration`：ESI 的 `CoE/PdoAssign` 与
  `CoE/PdoConfig` 能力，分别决定能否选择已有映射表、能否修改映射表条目，独立判断；
- `protocol.supports_distributed_clocks`：ESI 声明的能力，不表示运行方案必须启用 DC；
- `protocol.assign_activate_by_strategy`：ESI `<Dc><OpMode>` 里厂商写的 AssignActivate 值，键为
  `dc` 或 `sm`。运行方案的 `assign_activate` 必须取自这里，不得照标准默认值填：填错会把从站的
  同步生成配成另一套，主机侧只看到 DC 对不上。映射里没有哪种同步方式，表示厂商没有声明它，
  生成器拒绝为其生成运行方案；
- `protocol.supported_modes`：模式名 → 模式值。运行方案的每个 `mode_id` 必须在此表内且数值一致；
  模块化 ESI 声明了模块模式时，本表必须与之一致（ESI 是证据，本表是转述，不符时报错）。该表进
  `catalog.flat`，主站启动时再核对要跑的模式：表内没有报"方案与设备不符"，表内有而过程映像
  表达不了报"缺口在主站侧"。模式号跨厂商有歧义，主站只认这张表；
- `mit_gains`：`{"kp": <非负整数>, "kd": <非负整数>}`。设备声明了 MIT 模式时必须给，缺了生成器
  直接报错。原因：主站每拍把这两个值写进过程数据（`0x2013:1`/`0x2013:2`），工具编一个默认值，
  客户拿到的就是一份"看着能跑、刚度是猜的"的配置；不给就是每拍写 0，轴是松的，比拒开更危险。
  刻度是设备自己的：某一款实测下来值 1 等于没有刚度，量级要到十万以上才带得动轴，所以必须先在
  PRE-OP 只读这两个对象看驱动器里是多少、再按台架实测定，不要照 CAN 协议那两个 `mit_max_*`
  上限值——那是对 12 位定点数的量程，不是 EtherCAT 侧该用的增益；
- `protocol.safe_stop_status_mask` / `safe_stop_status_value`：停机时判定"设备已进入安全状态"的
  状态字掩码与期望值，必须台架实测填写，不得照标准默认值抄。掩码 0 表示该型号没有可靠的设备侧
  停机判据，运行期只走标准 CiA 402 状态判据（未使能/未就绪），期望值也必须为 0；
- `conversion`：可选。人对这台设备写下的换算与方向。
  `encoder_counts_per_motor_revolution_default`（608F:1/608F:2）、`gear_motor_revolutions_default`
  与 `gear_shaft_revolutions_default`（6091:1/6091:2，直驱写 1 和 1）三项必须同时给出且均为正
  整数，缺一项校验器即拒——运行期按一份完整换算使用，缺任一项会话判"运动无效"、轴一步不动。
  `polarity_default` 对应 607E 的 0/1，不声明时留空。**三项齐全时它压过设备自报的 608F/6091**：
  自报是驱动器的说法，这一份是人对着机器写下的判据，两者冲突按判据（台架从站 1 自报 6091=28000，
  真值 28）。声明缺项才退回自报，两边都没有则该轴换算不可用。压过不等于抹掉：自报值照读照写，
  报告与终端都写出两份并在不一致时标出 `device_agrees=false`。资料里没有的项留缺省，不得编造。
  实际生效的换算是运动方案里每轴的值；
- `slow_telemetry`：需周期读取的遥测对象表。`add-device` 新建策略档时从 ESI 对象字典
  起草：标准索引（0x6078 电流、0x6079 母线电压、0x606C/0x60FF 速度、0x603F 错误码、
  0x607A 目标位置回读）按标准认；厂商私有对象（如温度、电压）只接受与已知测量名完全
  同名的条目，PDO 映射表里的同名子项不取。一个语义只留一条，标准索引优先。已有策略档
  以档里的清单为准，缺这一项时 `add-device` 只提示。每项含 `name`（设备内唯一）、`index`
  （十六进制字符串）、`subindex`（0..255）、`type`（`u8`/`i8`/`u16`/`i16`/`u32`/`i32`，决定
  读取宽度）、`semantic`、`unit`（可为空）。`semantic` 取 `none`、`actual_current`、
  `error_code`、`bus_voltage`、`mosfet_temperature`、`motor_temperature`、`actual_velocity`、
  `target_velocity`、`extended_error_code`、`servo_error_code`、`target_position_readback`、
  `max_torque`、`max_motor_speed`，决定对象落入哪个会话结果槽位。名称与 `index:subindex` 唯一；
  缺省或为空表示该设备不提供遥测。通用代码遍历该表，不写死供应商对象号。
  `target_position_readback`（0x607A）是驱动器自己持有的目标位置，不是反馈，用于区分"值没到
  驱动器"与"到了驱动器但不执行"；`max_torque`（0x6072）与 `max_motor_speed`（0x6080）是配置型
  读数，只在 OP 期间读一遍并落在报告审计段，不进遥测字段；
- `source`：设备事实的来源记录，仅供人工追溯。

生成的 C 只从扁平文件读条目，不含按型号或份数推导的编译期常量：条目数运行期读
`config-runtime/catalog.flat` 的 `profile.count`。增加新型号不修改生成器、不需重编主站，重新
生成扁平文件并重启即可。

`devices/` 不手写，由 `config/device_policies/<型号>.json` 合成：策略档只写人做的选择（选哪几套
`pdo_sets`、每套的模式值、`supported_modes`、停机判据、换算与遥测对象），身份、PDO 映射、位宽、
数据类型由 `esi/` 里的设备描述推导。策略档里没有的事实不得臆造，合成器直接报错退出。

## 拓扑配置

每个 `topologies/*.json` 描述一个非空、任意规模的逻辑从站序列。

- `topology_id`：供部署引用的稳定且唯一的标识；
- `status`：该实例的证据状态（`provisional`、`observed`、`approved` 等）。配置基础设施原样
  保存，不把任何字符串解释为安全授权；
- `slaves`：按 EtherCAT 物理顺序排列，位置与 SOEM 探测报告一致（`1..N`）；`N` 由该实例的实际
  列表长度决定，不代表产品轴数；
- `axis_id`：拓扑内唯一的逻辑轴标识；
- `joint_assignment`：产品物理关节分配，未确认时必须为 `null`；
- `profile_id`：引用已存在的设备配置；
- `serial_number`：可选。从站 ESC 序列号（`0x1018:04`），十进制或无符号 32 位范围内的正整数，
  缺省或 `null` 表示未登记。同一拓扑内不得重复。登记后它就是轴到物理从站的键：主站起
  会话时在 PRE-OP 读总线上每台的序列号，按序列号挑从站；读不到或不相等即拒绝启动。
  `init-site --from-scan` 用扫描输出里的 `serial=` 自动填入（扫描读不到则留 `null`）。
  未登记的轴保持原行为：按 vendor/product/revision 取首个未分配的从站。

未确认的工程参数必须保持缺失或 `null`，生成器不补默认值。

每个 PDO 条目必须声明 `index`、`subindex`、`bits`、`data_type` 和稳定的 `name`。填充只能使用
`0x0000:00`、`PADDING` 和正定位位宽表示。`data_type` 来自受控 ESI，用于解释物理描述符，不是
主站运行时的控制参数。

单轴与多轴走同一套生成、发现和校验机制，不存在"单轴版主站"或"12 轴版主站"：轴数来自实际拓扑
列表长度，不来自编译期常量。交付时这一族为空，由 `init-site` 按你的环生成。

## 部署配置

每个 `deployments/*.json` 把逻辑拓扑绑定到明确的主机网络资源。

- `deployment_id`：稳定且唯一的标识；
- `hostname`：运行主站的主机名；
- `topology_id`：该部署选择的逻辑拓扑；
- `ethercat_interface`：只承载 EtherCAT 二层帧的物理接口；
- `management_interface`：可选的管理接口，用于 SSH、NTP 等 IP 服务，禁止与 EtherCAT 接口相同；
- `run_report_path`：每次会话原子覆盖的运行审计报告路径；运行产物目录不纳入 Git；
- `realtime`：可选。主站进程的实时调度参数，属于主机而非设备。整块缺省表示不做实时设置，进程
  照常运行，只是不保证时序。
  - `scheduler_priority`：`SCHED_FIFO` 优先级，1..99 或 null（不设）。台架取 80，高于驱动中断
    （通常 50–60），低于看门狗（通常 90+）；
  - `cpu_core`：绑定的 CPU 核编号，0..65535 或 null（不绑）。台架取 11，依据见
    `apps/master/main.c` 文件头；
  - `required`：布尔，缺省 false。false 时实时参数未能生效只记警告并继续；true 时任一项失败即
    拒绝启动。**计时类实验必须用 true**：参数没绑定不产生报错，只让抖动变大。核编号是否存在
    取决于运行时主机，校验器只查范围不查存在性。
- `external_target_timeout_ms`：可选。外部控制器超过此时间未更新目标即退化为保持（HOLD），
  1..600000 或 null，缺省 200（200 个 1 kHz 周期）。没有"永不超时"这个选项。CST 下这个超时
  不是保持，而是把目标回零——力矩里的"保持"等于继续出力。
- `external_target_max_offset_millidegrees`：可选。外部目标的偏移上限，单位负载侧毫度，
  0..3600000 或 null，缺省 90000（90°）。主站唯一保留的外部目标判据：`|目标 - 当前实际位置|`
  超过它即丢弃该条目标。不是限幅——主站不裁剪客户端给的值；判据是距离而非增量，与客户端发送
  频率无关；上限按每轴自己读回的 608F/6091 折算。0 与 null 同义，都走内置默认值。触发后果是
  拒收这一条而非中止会话：其余轴照常运行，计数与现场进报告。主站不限制目标增量和速度，也不设
  力矩上限。

实时参数是否生效看报告里的 `thread_schedstat`（内核实况，非进程自述）：每个线程的 `policy`、
`priority` 和 `cpus_allowed`（如 `"11"`）；`required` 为 false 的轮次靠它判断。

同一主机和 EtherCAT 接口允许出现多条部署记录（单从站、多从站、不加载固定运动的外部控制等互斥
场景，一次只能运行一个）；物理独占由运行时抢占网卡保证。校验器只约束 `deployment_id` 与
`run_report_path` 唯一，不约束网口唯一——后者防止不同场景的审计报告互相覆盖。网卡名（如
`enp49s0`）只出现在部署实例与测试证据里，通用代码与拓扑配置均不感知。

构建阶段把拓扑、运行方案和部署配置生成"填充代码"，只读扁平文件的条目数，不含具体值。运行时按
`deployment_id` 在 `config-runtime/*.flat` 里查找部署，接口、主机、拓扑和从站顺序均取自那里；
不提供绕过配置直接指定网卡、拓扑或 EtherCAT 参数的接口。

## 运行方案

`operation_profiles/` 中的每个 JSON 是一个候选方案，必须明确引用设备目录中的 `pdo_set_id`。除
同步策略外的 `selected_mode_id`、AssignActivate、周期、过程帧目标相位、DC 启动周期、Sync0 偏移
及 SM2/SM3 同步类型可为 `null`，表示尚未确认；生成器不补默认值。状态为 `approved` 后部署才能
引用，此时实际模式和所有必需同步参数必须存在。模式值必须与设备目录声明一致，模式声明的收发
字段必须实际存在于所选 PDO 方案。供应商资料不参与自动校验；参数依据由人工审查和真机读回确认。

`safeop_to_op_sdo_writes` 声明 SAFE-OP 到 OP 前的有序 SDO 初始化列表：对象、子索引、精确类型和
值，写入后立即按相同类型读回。设备模块的模式初始化与此列表按顺序执行。声明的值同时是"该对象
在本次会话里应当是多少"：设备把同一对象也映射进 RxPDO 时，周期数据每拍都在写它，那一次 SDO 写
会被顶掉，因此主站每拍把声明的值填进对应的过程数据字段；对象不在 RxPDO 里则仍只有那一次 SDO 写。
声明的类型决定字段位宽与符号；位宽对不上、或该字段本来就是主站每拍要写的量（控制字、模式、
目标），会话不启动。

`mode_display_policy` 为 `required` 或 `diagnostic`。`diagnostic` 允许保持位置会话记录模式不匹配，
但不允许变化轨迹绕过模式匹配；固定 PDO 没有模式字段时通过 SOEM 周期邮箱异步获取反馈。

每个模式必须声明 `quantity`，取 `none`、`position`、`velocity` 或 `torque`：目标通道收的那个数是
什么量。主站不按模式号猜——同一取值在不同厂商的说明书里指的不是同一件事。量决定三件事：目标写进
哪个 PDO 字段、反馈从哪一路读、断供之后写什么。`position` 断供保持在原位，并判"目标离实际位置
太远就拒收"；`velocity` 与 `torque` 断供写 0，且不判距离——拿速度去减实际位置得到的量没有意义，
判下去的结果是每一条目标都被拒。`none` 表示该模式没有周期目标（回零：参考点搜寻由驱动器自己
走），反馈仍是实际位置。

回零（`quantity: none`）另需 `completion_timeout_ms`：从主站第一次在 Operation enabled 下置控制字
bit4 起，到驱动器报状态字 bit12（homing attained）的最长允许时间，毫秒。部署选中的回零模式必须
给正数；方案里未选中的回零条目写 0，校验器也要求如此。驱动器报 bit13（homing error）或超过时限，
会话按故障停机；全部轴到点后自动停机。回零方法（0x6098；17–35 是常见方法号，不是运行模式号）
与两段速度（0x6099:1/2）用 `safeop_to_op_sdo_writes` 声明，不给就保持驱动器里存的值。主站不产生
回零轨迹。`init-site --mode homing` 要求同时给 `--homing-timeout-ms`，并可给
`--homing-method` / `--homing-speed-switch` / `--homing-speed-zero`。设备档的 `supported_modes`
里没有 6 时，生成器拒绝回零方案。

只要选中的 PDO 组把 `0x6072`（最大力矩）或 `0x6080`（最大电机转速）映射进 RxPDO，方案里
就必须用 `safeop_to_op_sdo_writes` 给它们固定值。主站每拍写整块输出，漏声明就是每拍写 0；
MT 驱动器在 `0x6072=0` 时使能成功、状态字正常，但拒绝出力。构建校验会拒绝这种方案，
值取台架实测或厂商资料，实际生效的值在 `status` 与报告审计段里能读到。

`final_sdo_reads` 声明退出周期、进入 PRE-OP 后读取的诊断对象、名称和数据类型。总线层按配置用
SOEM SDO API 读取，审计报告记录真实原始字节和结果；新增设备或模式不需要修改诊断代码。

部署中的 `operation_profile_ids` 为空表示禁用过程数据会话；非空时必须为拓扑使用的每种设备配置
恰好启用一个已批准方案，且所有方案周期一致。会话启动命令不接受模式、周期或同步参数覆盖。DC
方案的过程帧目标相位必须早于 Sync0，`dc_startup_cycles` 指定进入 OP 前连续交换的周期数。

`motion_profiles/` 不提供默认运动。每份方案必须显式给出坐标系、持续时间、稳定时间和全部轴的
有符号相对角度、跟随误差边界和预期 `608F/6091`；CSV 还必须给出目标速度、加速度、减速度和速度
误差边界，CST 还必须给出目标转矩、转矩变化率和转矩误差边界。未使用的模式字段必须显式为零。
每轴换算优先取设备档 `conversion` 里三项齐全的声明值；声明缺项时退回真机读回的 608F/6091。
两者都没有、或换算出不来、或与方案前提不一致时，主站拒绝运动。部署只允许引用一份已批准且完整
覆盖当前拓扑的方案。

部署引用的方案就是实际生效的方案：主程序按它配置 DC、模式和周期，并执行 CiA 402 运行使能；目标
位置由启动时的 `6064` 初始化，变化目标由部署引用的 `motion_profile_id` 决定。未被部署引用的方案
不会被加载。PRE-OP 指纹工具不会读取它来提高状态。

`deployments/*.json` 里的 `motion_profile_id` 由 `init-site --motion <ID>` 写入，不给就是 `null`。
这一项挂不上是安静的：方案照样进 `runtime.flat`，部署那一行写 `motion_profile_id_present=false`，
主站照常起、照常空转，编进去的换算与限幅一次都用不上。生成器两处拦截：`init-site --motion`
引用了还没写的方案时就地拒绝，`build` 对"写了方案却没有部署引用它"报告警。

产品运行时的位置目标不应通过修改 JSON 作为实时命令。应用层通过控制会话的位置目标来源回调提交
完整轴集合；回调输入使用当前周期反馈，输出已完成换算的目标计数，不规定输入频率，也不负责生成
目标轨迹。部署未绑定该回调时，主程序继续执行部署引用的固定运动方案，保留 `CSP36°成功` 作为真机
功能基线。

## 提示资源

`messages/zh_CN.json` 是当前中文提示的唯一正文来源。构建阶段由 `tools/gen/generate_messages.py`
生成只读 C 表，并校验每条 `printf` 格式参数；业务代码只引用消息 ID，不得嵌入操作者提示文本。

## 运行记录

日常报告由 `run_report_path` 指定，建议放入已忽略的 `runtime/reports/`。报告版本 2 删除了不再
执行的 OP 即时 SDO 字段，新增 `first_cycle_failure`、`shutdown_al` 和审计截断计数。
`actual_wkc` 是最后一次交换结果，首次异常的 WKC 单独保留；最终 SDO 属于退出周期后的快照。
每个轴的 `timing` 记录主机时间、参考 DC 时间和由传播延迟推导的 Sync0 裕量；它不表示从站内部
SM2 完成时间戳。标准诊断工具和主程序具有不同职责，诊断工具不充当主程序必须经过的启动阶段。
