"""独立于主站的图形界面（PyQt5）。入口：python -m emaster_gui --help。

包内分工：

    app.py       窗口、动作按钮、参数解析。所有 Qt 控件只在这里。
    poller.py    两条后台 I/O 线程（观测 20 Hz / 命令 50 Hz）。套接字只在这里。
    视图模块     轴表、曲线、健康量（下一步补；现在轴表与健康量在 app.py 里）

控制律不在这里：Engine（tools/emaster_console.py）是控制律的唯一一份实现，TUI 面板
与本界面共用它。这里只负责"把它的状态画出来、把人的操作递进去"。
"""

import os
import sys

# 让包内的 import 能用平铺的名字（emaster_client / emaster_endpoint / emaster_console
# / rt_affinity 都在 tools/ 下），与仓库里其它工具一致。
_TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)
