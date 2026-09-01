# 环境检查结论

## 2026-08-31 初始工程检查

- 目标：Orange Pi 6 Plus，Ubuntu 24.04.3，aarch64；
- 内核：`6.6.89-cix`，`CONFIG_PREEMPT=y`，不是 PREEMPT_RT，`CONFIG_HZ=250`；
- `orangepi` 账号没有实时调度额度，`ulimit -r` 为 `0`；
- CPU 和网卡 IRQ 隔离尚未配置；
- 两个相关网口都使用 Realtek `r8126` 驱动；
- `enp97s0` 是管理接口，禁止用于 EtherCAT；
- `enp49s0` 原本承担激光雷达/PTP 配置；2026-08-31 已建立持久化的
  `ethercat-dedicated` NetworkManager 连接，IPv4/IPv6、LLDP、mDNS 和 LLMNR 均禁用；
- 原 `z2mini-static` 连接保留用于回退，但已取消自动连接；
- `ptp4l-enp49s0.service`、`livo-drivers.service` 和 `robot-ui-preview.service` 已停止并取消
  开机启用，防止重启后重新占用该接口；
- 原网络和服务配置备份位于 Orange Pi 的
  `/home/orangepi/ethercat-network-backup.VuT6gU/`；
- 开发工作区日期为 `2026-08-31` 时，板端时钟报告
  `2026-08-19T18:48:23+08:00`。创建可追溯硬件记录前必须修正时钟同步。

内核、实时调度权限和时钟问题仍是阻断项。本次只修改了 `enp49s0` 的网络归属及其直接冲突
服务，没有修改内核、调度器、主机时钟或管理接口 `enp97s0`。

## 2026-08-31 远程复核阻断

- 从开发主机到 `192.168.124.92` 的 ICMP 往返小于 1 ms，TCP/22 可以完成握手；
- OpenSSH 和 Paramiko 均在认证前失败，远端未发送 SSH 协议标识并主动关闭连接；OpenSSH
  报告 `kex_exchange_identification: Connection closed by remote host`；
- 因无法建立经过认证的管理会话，本轮没有执行任何板端命令，也没有修改主机时间、NTP、网口、
  服务或 EtherCAT 状态；
- 恢复本机控制台或可靠 SSH 后，应先检查 `sshd` 日志和连接限制，再通过管理口启用 NTP，保存
  `timedatectl` 同步证据，最后重新执行构建、静态分析和受限 PRE-OP 检查。

## 2026-09-01 管理网络与时间恢复

- Windows 通过“以太网 6”向管理口共享网络，Orange Pi 的 `enp97s0` 通过 DHCP 获得
  `192.168.137.54/24`，默认路由指向 `192.168.137.1`；
- `enp49s0` 仍由 `ethercat-dedicated` 连接专用于 EtherCAT，无 IP 地址和路由；
- `ptp4l-enp49s0.service`、`livo-drivers.service`、`robot-ui-preview.service` 均保持停止；
- chrony 已选择 `time.neu.edu.cn`，`timedatectl` 报告 NTP 已同步，采集时系统时间与 NTP 的
  偏差约为 `0.07 ms`；
- Orange Pi 上的 Debug、RelWithDebInfo、资料一致性和 cppcheck 检查均已通过；
- 受限 PRE-OP 已确认一台从站身份与当前部署拓扑一致并恢复 INIT。首次报告中的
  `1C12:02`、`1C13:02` 失败来自读取计划无依据地预读第二个分配项，不能作为最终通过证据。
- 修正计划后重新采集的候选记录为 `orangepi-bench-20260901T023814Z.json`，SHA-256 为
  `bbd97746ca1cdcb3088f1e7d3ad9c21d1022198e2148d7f8126cbc53686917b6`。一台从站通过
  拓扑身份核对，44/44 项 SDO 成功，`1C12:00=1`、`1C13:00=1`，并确认恢复 INIT；该原始
  记录保留在 Orange Pi 上，尚未由项目负责人批准或提交。
- 动态 PDO 发现改造后，Orange Pi 于 `2026-09-01T03:36:44Z` 重新采集 schema 2 候选：
  `0x1C12/0x1C13` 各 1 张映射表、各 6 个条目、Rx/Tx 各 112 位（14 字节），38/38 项通用
  SDO 成功，身份和 PDO 严格匹配，且恢复 INIT 成功。该候选仍需项目负责人批准后才可进入
  后续过程数据开发。
- 最终同步代码后的复采候选为 `orangepi-bench-20260901T034149Z.json`，SHA-256 为
  `08e2fcc1bfb39489a2101b026c49d290acc746e9b890f5425c5ce6d85d54e71b`。项目负责人已批准
  该文件作为 `orangepi-current-bench` 部署、`bench-current-single-slave` 台架拓扑的当前
  单从站 PDO 基线并纳入仓库；批准不扩展到其他从站、最终产品拓扑、DC、SAFE-OP/OP 或运动。

## 2026-09-01 PRE-OP DC 参数准备真机验证

- Orange Pi 地址为 `192.168.137.54`，EtherCAT 专用接口为 `enp49s0`，当前检测到 1 台从站；
- 执行 `emaster-dc-prepare prepare-dc` 成功，过程包含身份核对、动态 PDO 严格匹配、SDO 写入与读回、
  `ecx_configdc()`、Sync0 配置和寄存器读回；退出前已关闭 Sync0 并将从站恢复到 INIT；
- 读回通过：`1C32:01=0x0001`、`1C33:01=0x0022`、`6060:00=8`，Sync0 周期为 `1000000 ns`，
  激活位与批准方案一致；
- `6061:00` 在 PRE-OP 下未提供有效模式显示反馈。该对象为只读 TPDO 反馈，当前未进入 SAFE-OP/OP，
  也未交换过程数据，因此记录为“延后验证”，不伪造为通过；
- 本次未执行 PDO 映射写入、过程数据交换、SAFE-OP/OP、控制字写入、电机使能或运动。
