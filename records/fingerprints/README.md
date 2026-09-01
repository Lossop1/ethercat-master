# 物理从站指纹记录

将 `emaster-fingerprint` 生成且经过批准、未经修改的输出放在此目录，并同时保存测试记录和
SHA-256。文件名必须标识日期和物理测试装置，不能只用 EtherCAT 线缆位置作为设备身份。

`orangepi-bench-20260901T034149Z.json` 已由项目负责人批准为 `orangepi-current-bench` 部署、
`bench-current-single-slave` 台架拓扑的单从站 PDO 基线。该批准不扩展到最终产品拓扑、其他物理
从站、DC 参数、SAFE-OP/OP 或电机运动；文件哈希记录在 `docs/testing/environment-findings.md`。
