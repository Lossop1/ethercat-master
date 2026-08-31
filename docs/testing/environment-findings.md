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
