"""python -m emaster_gui 的入口。

单独放一个文件（而不是在 __init__.py 里跑），是为了让 `import emaster_gui` 保持
无副作用——无头回归要能只导入包、拿到里面的类来单测，不必起窗口。
"""

import sys

from emaster_gui.app import main

if __name__ == "__main__":
    sys.exit(main())
