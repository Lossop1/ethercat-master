# external：第三方源码

## SOEM

`SOEM/` 是 SOEM（Simple Open EtherCAT Master）的源码，以源码形式随本产品交付。主站用它收发 EtherCAT 帧。

| 项 | 值 |
|---|---|
| 上游仓库 | `https://github.com/OpenEtherCATsociety/SOEM.git` |
| 本树所含提交 | `304d1c05eab77dc0d426f1a5cf09c8cc7dc03713`（`v1.3.1-293-g304d1c0`） |
| 许可 | 见 `SOEM/LICENSE.md` |

**不得修改 `SOEM/` 里的任何文件。** 本产品只在 `external/SOEM` 下构建它，不改动它的源码。改动会与上游分叉，且改错了不会报警。

更新方法：取上游新提交，整目录替换，再改上表第二行。
