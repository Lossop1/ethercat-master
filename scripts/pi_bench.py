#!/usr/bin/env python
"""Orange Pi 台架命令的薄封装。

存在原因：Windows/Git Bash 会把以 / 开头的参数改写成 Windows 路径
（`/home/orangepi` -> `D:/Program Files/Git/home/orangepi`），导致远端路径
在到达 pi.py 之前就被破坏。Git Bash 中 `MSYS_NO_PATHCONV=1` 对部分内置
处理无效，因此在 Python 层显式设置并转交参数。

用法：
    pi_bench.py run <命令>
    pi_bench.py pushdir <本地目录> <远端目录>
    pi_bench.py push <本地文件> <远端文件>
    pi_bench.py pull <远端文件> <本地文件>
"""

import os
import subprocess
import sys

PI = r"D:\taili\.ssh-emaster\pi.py"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run([sys.executable, PI] + sys.argv[1:], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
