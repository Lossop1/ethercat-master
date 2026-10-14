# 设备上手工具

换一款电机时跑这一套：把厂商给的 ESI 变成一份设备档。主站构建和运行都不调用这里，
只在加设备型号时用。**面向客户的那条命令是 `tools/gen/emaster_config.py add-device`**，
本文说清它背后做了哪两步、以及为什么这两步必须分开。

## 为什么拆两步

设备档里混着两类东西：

- **硬件事实** —— 身份、PDO 映射、每个条目的位宽和数据类型。ESI 里全有，换电机就变，
  人不需要对它们做决定。
- **工程策略** —— 选哪套 PDO、进 OP 前把模式写成几、停机判据用状态字哪一位、读哪些遥测、
  换算从哪个对象读。这些是人决定的。

手抄这两类就是交付的断点：第三方换电机时不知道哪些该改、哪些该照抄。所以拆成
`facts`（自动推导）+ `policy`（人写）两份，再合成。策略档的字段语义见 `config/README.md`。

## 怎么用

```bash
python tools/gen/emaster_config.py add-device esi/<你的型号>/<文件>.xml
```

一条命令做三件事：推导硬件事实、写一份策略档草稿、合成设备档。跑完它会列出
**还需要人工确认的那几个字段** —— 那些是 ESI 里没有、只能由人和台架给的（当前
模式值、支持的模式集合、停机判据、换算期望），不确认就往下走等于拿猜的跑真轴。

它内部就是下面四步，分开跑可以用来排查：

1. 把 ESI 放进 `esi/<你的型号>/`。
2. 看事实报告：`python tools/gen/device/inspect_esi.py esi/<你的型号>/<文件>.xml`
3. 推导硬件事实：

   ```bash
   python tools/gen/device/derive_device_facts.py esi/<你的型号>/<文件>.xml --output facts.json
   ```

   推不出来时**直接非零退出**，不会给你一份空的 `pdo_sets`（见"生效 PDO 是怎么定下来的"）。
4. 改策略档草稿（`config/device_policies/<生成的>.json`），再合成设备档：

   ```bash
   python tools/gen/device/merge_device_profile.py --facts facts.json \
     --policy config/device_policies/<型号>.json \
     --esi esi/<型号>/<文件>.xml --output config/devices/<型号>.json
   ```

接着跑 `init-site` 出拓扑与部署，再 `tools/gen/emaster_config.py build` 生成扁平文件。
**不需要重新编译主站。** 要跑主站内置的固定轨迹，就在 `init-site` 之后照它打印的轴 ID
写 `config/motion_profiles/<ID>.json`，再带 `--motion <ID> --force` 重跑一次 `init-site`
把它挂到部署上 —— 只写文件不挂上去，它不会生效。

## 生效 PDO 是怎么定下来的

ESI 有两种写法，工具按设备有没有 `<Module>` 分两条路：

- **有模块**：模块就是全部可能，一个模块出一套 PDO 方案，运行期写 `0xF030` 选模块。
  策略档用 `module_ident` 指出选哪几套。
- **没有模块**：PDO 直接挂在 `<Device>` 下，出厂默认接的是哪一套由 `0x1C12`/`0x1C13`
  的默认分配说了算。这一套要与"哪几张映射表带 `Sm` 属性"互相印证 —— 两条规则给的是
  同一套，才算定下来。对上了，出一套方案（`assignment_style` 为 `direct_1c12`）；
  对不上或者只有一条能用，**拒绝推导并非零退出**。

为什么要两条规则对：条目顺序决定位偏移，顺序猜错，设备照样进 OP、反馈值照样有数，只是全对不上。
签合同前跑不起来是安全的，按猜出来的映射跑起来才是事故。所以宁可失败。

推不出来的三种情形（都非零退出，不留产物）：既没有 `<Module>` 也没有默认分配；默认分配与 `Sm`
互相矛盾；默认分配引用了 ESI 里根本没有的映射表。

`tests/checks/device_facts_check.py` 用自带的合成样本把这三条和上面的顺序、位宽都钉住 ——
它不读仓库里任何 ESI。

## 边界

- `facts` 不带任何工程决策。`supported_modes` 只在 ESI 的 `Module` type 里写了模式名时才有值；
  非模块化 ESI 观察不到模式集合，这一份只能由策略档写 —— 而策略档写的那份就是唯一来源。
- `--esi` 只写进设备档的 `source.esi`，仅作溯源记录。设备档是自足的，运行期不读 ESI。
- 策略档里 `conversion` 与 `slow_telemetry` 可以缺省。缺省的含义是"厂商资料没给"，
  是句实话；工具不会替厂商编一个默认值填上。
- 同步怎么使能（AssignActivate）也不是工程选择：它跟着 ESI 走，落在设备档的
  `protocol.assign_activate_by_strategy` 里，`init-site` 直接用它。ESI 里没有 `<Dc>` 那一段
  就说明厂商没声明 DC，此时 `init-site` 拒绝生成 —— 补 ESI，不要在配置里编一个值。
