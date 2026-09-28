#!/usr/bin/env python3
"""随时间变化的曲线。**自绘**，不用 QtChart——香橙派上装不到那个模块（见下）。

**为什么是 QPainter 而不是某个现成图表控件。** 台架上跑界面的机器是香橙派，
apt 带的只有 PyQt5 的 QtWidgets 那一套，`QtChart` 不在里面，而且那台机器没有网、装
不了。所以曲线只能自己画。这不是"想自己写"，是没有别的选择——所以这里尽量少做事：
只画折线，不做交互，不做图例之外的装饰。

**抽稀在收下的时候做，不在画的时候做。** 这是这个文件里最重要的一条决定。

主站按 1 kHz 出帧，一轴四条序列（实际位置/目标/跟随误差/力矩）。要是一股脑全存下来，
一分钟 24 万个点，画的时候每重画一次就要扫一遍——20 Hz 重画就是每秒五百万次比较，
足够吃掉香橙派的一整个核，而那个核本来该跑主站（本轮的要求是"别和主站抢资源"）。
把抽稀挪到收下的时候，代价就从"每次重画"变成"每来一帧一次"，而且只有当前显示的那
根轴在收。

抽稀手法是**按时间桶取最小/最大**（不是隔几个取一个）：

  - 一桶留两个数（最小、最大），画的时候画成一条竖线。**极值都在，包络就不丢**——
    隔点抽样会把尖峰整个漏掉，而尖峰（跟随误差、力矩冲击）恰恰是这里要看的东西。
  - 桶宽按显示能力定：一个视野留 1000 个桶。屏幕上一屏也就一千来列，再多存也画不出来。
    60 秒的窗 → 60 ms 一桶，比 1 kHz 的原始点粗 60 倍，内存和重画开销一起降 60 倍。
  - 代价写在明处：一个桶的**桶内形状**没了（只知道上下界，不知道中间怎么走的）。
    看趋势、看有没有尖峰是对的用途；要看某一毫秒的波形，那不归这个界面管。

**时间轴用帧自带的 monotonic_ns**，不是本机收到的时间。主站所有的帧来自同一个时钟，
用它才能把"命令发出去的这一刻"和"轴动起来的那一帧"对起来；用收到的本地时间会把
网络抖动算进曲线，而那恰恰是这里要排除的东西。
"""

import array

from PyQt5.QtCore import QPoint, Qt
from PyQt5.QtGui import QColor, QFontMetrics, QPainter, QPen
from PyQt5.QtWidgets import QSizePolicy, QVBoxLayout, QWidget

# 四条曲线的颜色。位置和目标是同一件事的两面，用近似色；误差和力矩各一色。
COLOR_ACTUAL = QColor(0x1f, 0x77, 0xb4)
COLOR_TARGET = QColor(0xff, 0x7f, 0x0e)
COLOR_ERROR = QColor(0xd6, 0x27, 0x28)
COLOR_TORQUE = QColor(0x2c, 0xa0, 0x2c)
COLOR_AXIS = QColor(0x88, 0x88, 0x88)
COLOR_TEXT = QColor(0x33, 0x33, 0x33)
COLOR_BG = QColor(0xfa, 0xfa, 0xfa)

# 一轴三条曲线各占一个横条（各自有自己的纵轴量纲，混在一张图上没法读）。
PANE_ACTUAL = 0
PANE_ERROR = 1
PANE_TORQUE = 2
PANE_TITLES = ("位置 / 目标（counts）", "跟随误差（counts）", "力矩（原始值）")
PANE_HEIGHT_WEIGHT = (3, 2, 2)      # 位置那条给高一点，它是主要看的
PANE_ORDER = (PANE_ACTUAL, PANE_ERROR, PANE_TORQUE)

LEFT_MARGIN = 92                    # 留给纵轴刻度的宽度
RIGHT_MARGIN = 8
TOP_MARGIN = 4
BOTTOM_MARGIN = 22                  # 留给时间轴
MIN_PANE_H = 24

# 横条顶上留给标题那一条的高度。标题写在 `top + 12`，纵轴上端的刻度值写在
# `to_y(hi) + 4`——两个都在 x=6，标题没占住地方的时候刻度值会压到标题上（P12.18）。
TITLE_H = 14
# 标题下面至少要留得下一段曲线，不然横条只是个标题。
MIN_PLOT_H = 16
# 曲线区上下各让出来的边距：极值不贴边框线，刻度值也不出界。
HEADROOM = 7
# 上端刻度值的基线，画在曲线区上边往下这么些像素。
LABEL_DROP = 4

# 一张图至少要这么高。三个横条（位置 3 份、误差 2 份、力矩 2 份）各要留得下刻度和
# 一条像样的曲线，低于这个数就只剩三条细缝了。容器的最小高度也用它。
MIN_CHART_H = 210

# 一个视野留多少个桶。屏幕上一屏也就一千来列，这是"存多了也画不出来"那个上限。
BUCKETS_PER_WINDOW = 1000


def bucket_width_ns(seconds, sample_hz):
    """一个视野配多少纳秒一桶。

    下限是采样间隔：桶比采样还密就只是原样存下来，白占内存。上限不设——窗口拉得
    越长，桶越粗，这是对的，看长趋势本来也不需要毫秒分辨率。
    """
    sample_ns = int(1e9 / max(1.0, sample_hz))
    return max(sample_ns, int(seconds * 1e9 / BUCKETS_PER_WINDOW))


class Series:
    """一条随时间走的序列，按时间桶存最小/最大。点存在 array 里，不放 Python 对象。

    配套的两件事在这里做掉，画的时候就不必再想：

      1. **断开**：帧号跳号的地方是真的丢了数据，不是"值没变"。跳号落在哪个桶上
         记一笔，画的时候那个桶空着，免得把两段不相干的数据连成一条线——那条线
         看着像"轴快速走过去了"。
      2. **上限**：只留最近 max_buckets 个桶，从头部成批丢。删 array 前 N 个元素是
         O(剩余长度)，所以按块丢（一次四分之一）而不是每来一个桶丢一个。
    """

    def __init__(self, name, color, bucket_ns, max_buckets):
        self.name = name
        self.color = color
        self.bucket_ns = max(1, int(bucket_ns))
        self.max_buckets = max(4, int(max_buckets))
        self.t = array.array("q")       # 桶起点（相对会话起点，纳秒）
        self.lo = array.array("q")
        self.hi = array.array("q")
        self.breaks = array.array("q")  # 跳号所在的桶起点
        self.last_index = None
        self.disordered = 0             # 时间倒着来的帧数（正常为 0，见 append）

    def __len__(self):
        return len(self.t)

    def __bool__(self):
        return bool(self.t)

    def append(self, stamp_ns, value, publish_index=None):
        bucket = (int(stamp_ns) // self.bucket_ns) * self.bucket_ns
        if publish_index is not None and self.last_index is not None \
                and publish_index != self.last_index + 1:
            self.breaks.append(bucket)
        if publish_index is not None:
            self.last_index = publish_index

        if self.t and bucket < self.t[-1]:
            # 时间倒着来了。正常跑不会有（帧按 publish_index 递增取回，monotonic_ns
            # 单调），但"正常不会发生"是假设不是保证。塞进去会破坏桶的时间序，摊列时
            # 画出鬼影；悄悄丢掉则是丢了数据没人知道。所以记一笔、丢弃、并且计数——
            # 计数会出现在自检报告里，不会烂在内存里。
            self.disordered += 1
            self.breaks.append(bucket)
            return
        if self.t and self.t[-1] == bucket:
            if value < self.lo[-1]:
                self.lo[-1] = value
            if value > self.hi[-1]:
                self.hi[-1] = value
            return
        self.t.append(bucket)
        self.lo.append(value)
        self.hi.append(value)
        if len(self.t) > self.max_buckets:
            drop = max(1, len(self.t) // 4)
            del self.t[:drop]
            del self.lo[:drop]
            del self.hi[:drop]

    def reset(self):
        self.t = array.array("q")
        self.lo = array.array("q")
        self.hi = array.array("q")
        self.breaks = array.array("q")
        self.last_index = None
        self.disordered = 0

    def columns(self, t0, t1, width):
        """摊到像素列上，返回 (每列下界, 每列上界, 断开的列)。

        桶本来就比像素列少（一个视野 1000 桶），所以这里走一遍桶、不做排序，
        合并是顺带发生的：两个桶落进同一列就把上下界并起来。
        """
        span = t1 - t0
        if span <= 0 or width <= 0:
            return [], [], set()
        lows = [None] * width
        highs = [None] * width
        scale = width / span
        for stamp, low, high in zip(self.t, self.lo, self.hi):
            if stamp < t0 or stamp > t1:
                continue
            column = int((stamp - t0) * scale)
            if column < 0:
                column = 0
            elif column >= width:
                column = width - 1
            if lows[column] is None:
                lows[column] = low
                highs[column] = high
            else:
                if low < lows[column]:
                    lows[column] = low
                if high > highs[column]:
                    highs[column] = high
        broken = set()
        for stamp in self.breaks:
            if t0 <= stamp <= t1:
                column = int((stamp - t0) * scale)
                if 0 <= column < width:
                    broken.add(column)
        return lows, highs, broken

    def span_ns(self):
        if not self.t:
            return 0
        return self.t[-1] - self.t[0]


class AxisChart(QWidget):
    """一根轴的三条曲线：位置/目标、跟随误差、力矩。三个横条共用一条时间轴。

    数据只往里追加（append），不在画的时候算——draw 里做的事越少，主站越不受影响。
    """

    def __init__(self, axis_index, seconds=60.0, hz=1000.0, parent=None):
        super().__init__(parent)
        self.axis_index = axis_index
        self.seconds = max(0.5, float(seconds))
        self.bucket_ns = bucket_width_ns(self.seconds, hz)
        # 留两个视野的桶：视野是一窗，多留一窗免得刚滚出去的又回来了。
        max_buckets = 2 * BUCKETS_PER_WINDOW + 8
        self.actual = Series("实际", COLOR_ACTUAL, self.bucket_ns, max_buckets)
        self.target = Series("目标", COLOR_TARGET, self.bucket_ns, max_buckets)
        self.error = Series("跟随误差", COLOR_ERROR, self.bucket_ns, max_buckets)
        self.torque = Series("力矩", COLOR_TORQUE, self.bucket_ns, max_buckets)
        self.origin_ns = None
        self.latest_ns = None
        self.fed = 0                # 喂进来的原始帧数（存下来的桶数少得多）
        self.gaps = 0

        # 摊列结果的缓存：数据一变或窗口一变就失效。有了它，重画只是读几个列表。
        self._cache = None
        self._cache_key = None
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setMinimumHeight(MIN_CHART_H)

    # ---------------------------------------------------------------- 数据
    def append(self, frames):
        grew = False
        for frame in frames:
            stamp = frame["monotonic_ns"]
            if self.origin_ns is None:
                self.origin_ns = stamp
            self.latest_ns = stamp
            relative = stamp - self.origin_ns
            index = frame["publish_index"]
            axis = frame["axes"][self.axis_index]
            actual = axis["actual_position"]
            target = axis["target_position"]
            self.actual.append(relative, actual, index)
            self.target.append(relative, target, index)
            self.error.append(relative, target - actual, index)
            self.torque.append(relative, axis["actual_torque"], index)
            grew = True
        if not grew:
            return
        self.fed += len(frames)
        self.gaps = len(self.actual.breaks)
        self._cache = None
        self.update()

    def clear(self):
        for series in (self.actual, self.target, self.error, self.torque):
            series.reset()
        self.origin_ns = None
        self.latest_ns = None
        self.fed = 0
        self.gaps = 0
        self._cache = None
        self.update()

    def window(self):
        """当前视野 (t0, t1)，单位是相对纳秒。"""
        if self.latest_ns is None:
            return 0, 1
        t1 = self.latest_ns - self.origin_ns
        return t1 - int(self.seconds * 1e9), t1

    def stats(self):
        """给自检报告用的实况。**数字都是事实**：喂了多少帧、真正留了几个桶。"""
        return {
            "axis": self.axis_index + 1,
            "fed": self.fed,
            "buckets": len(self.actual),
            "bucket_ms": self.bucket_ns / 1e6,
            "span_s": self.actual.span_ns() / 1e9,
            "gaps": self.gaps,
            "disordered": self.actual.disordered,
            "window_s": self.seconds,
        }

    # ---------------------------------------------------------------- 绘制
    def _ensure_columns(self, width):
        key = (self.fed, len(self.actual), width, self.seconds)
        if self._cache is not None and self._cache_key == key:
            return self._cache
        t0, t1 = self.window()
        plot = max(0, width - LEFT_MARGIN - RIGHT_MARGIN)
        panes = {}
        for name, series in (("actual", self.actual), ("target", self.target),
                             ("error", self.error), ("torque", self.torque)):
            panes[name] = series.columns(t0, t1, plot)
        self._cache = (t0, t1, plot, panes)
        self._cache_key = key
        return self._cache

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, False)
        painter.fillRect(self.rect(), COLOR_BG)

        width = self.width()
        height = self.height()
        if width <= LEFT_MARGIN + RIGHT_MARGIN or height <= TOP_MARGIN + BOTTOM_MARGIN:
            painter.setPen(COLOR_TEXT)
            painter.drawText(8, 20, f"轴{self.axis_index + 1}：窗口太小，画不下")
            return

        t0, t1, plot, panes = self._ensure_columns(width)
        for pane, top, pane_h in self._pane_bands(height):
            self._draw_pane(painter, pane, top, pane_h, panes)

        self._draw_time_axis(painter, height, t0, t1)

    @staticmethod
    def _pane_bands(height):
        """一张图的高度切成哪几个横条：[(横条, 上边, 高)]，按从上到下的顺序。

        画和检查都走这一份，检查里就不用把切法再抄一遍——抄的那份一旦跟这里走岔了，
        检查会绿着而屏幕上不是那么回事。
        """
        plot_height = height - TOP_MARGIN - BOTTOM_MARGIN
        weight_sum = sum(PANE_HEIGHT_WEIGHT)
        bands = []
        top = TOP_MARGIN
        for pane in PANE_ORDER:
            pane_h = int(plot_height * PANE_HEIGHT_WEIGHT[pane] / weight_sum)
            bands.append((pane, top, pane_h))
            top += pane_h
        return bands

    def _pane_series(self, pane, panes):
        if pane == PANE_ACTUAL:
            return (("actual", panes["actual"], COLOR_ACTUAL),
                    ("target", panes["target"], COLOR_TARGET))
        if pane == PANE_ERROR:
            return (("error", panes["error"], COLOR_ERROR),)
        return (("torque", panes["torque"], COLOR_TORQUE),)

    def _draw_frame(self, painter, pane, top, pane_h, entries):
        """画一个横条的外框与标题，并把纵轴范围算出来。范围算不出来就返回 None。"""
        painter.setPen(QPen(COLOR_AXIS, 1, Qt.DotLine))
        painter.drawLine(LEFT_MARGIN, top + pane_h, self.width() - RIGHT_MARGIN,
                         top + pane_h)
        painter.setPen(COLOR_TEXT)
        painter.drawText(6, top + 12, f"轴{self.axis_index + 1} {PANE_TITLES[pane]}")

        lo = hi = None
        for _name, (lows, highs, _broken), _color in entries:
            for value in lows:
                if value is not None and (lo is None or value < lo):
                    lo = value
            for value in highs:
                if value is not None and (hi is None or value > hi):
                    hi = value
        if lo is None:
            painter.setPen(COLOR_AXIS)
            painter.drawText(LEFT_MARGIN + 8, top + pane_h // 2, "还没有数据")
            return None
        if hi <= lo:
            # 一条平直的线：给个上下各一点的余量，否则除以零。
            lo, hi = lo - 1, hi + 1
        pad = (hi - lo) * 0.08
        lo -= pad
        hi += pad
        return lo, hi

    @staticmethod
    def _pane_geometry(top, pane_h):
        """一个横条的几何：曲线区的上下边、标题的基线。

        曲线区上下各让出 `HEADROOM`：极值贴着边框线不好看，而且上端的刻度值是画在
        曲线区上边**往下** `LABEL_DROP` 处的，不留这点它就出界了。上边另外还让出
        标题那一条 `TITLE_H`。

        **上端刻度值的基线必须落在标题基线以下。** 标题和刻度值都从 x=6 起画，标题
        在 `top + 12`；曲线区上边要是跑到标题基线上面去（`TITLE_H` 没让出来就是），
        刻度值就压在标题上——P12.18 就是这个，两个只差一个像素，看着像字糊了。
        几何单独拿出来，就是为了让 `scripts/checks/chart_check.py` 能钉住这一条。
        """
        title_baseline = top + 12
        plot_top = top + TITLE_H + HEADROOM
        plot_bottom = top + pane_h - HEADROOM
        return plot_top, plot_bottom, title_baseline

    def _draw_pane(self, painter, pane, top, pane_h, panes):
        # 连标题带一段曲线都放不下就整个横条不画——画了也只是一条压在一起的字。
        if pane_h < max(MIN_PANE_H, TITLE_H + MIN_PLOT_H):
            return
        entries = self._pane_series(pane, panes)
        bounds = self._draw_frame(painter, pane, top, pane_h, entries)
        if bounds is None:
            return
        lo, hi = bounds
        span = hi - lo
        plot_top, plot_bottom, _title = self._pane_geometry(top, pane_h)
        inner = max(1, plot_bottom - plot_top)

        def to_y(value):
            return plot_bottom - int((value - lo) * inner / span)

        painter.setPen(COLOR_TEXT)
        painter.drawText(6, to_y(hi) + LABEL_DROP, f"{int(hi)}")
        painter.drawText(6, to_y(lo) + LABEL_DROP, f"{int(lo)}")

        for _name, (lows, highs, broken), color in entries:
            painter.setPen(QPen(color, 1))
            # 一列一条竖线，从这一列的最小值画到最大值。极值都在，包络就丢不了。
            for column, low in enumerate(lows):
                if low is None or column in broken:
                    continue        # 没有数据的列、跳号所在的列，都空着
                high = highs[column]
                x = LEFT_MARGIN + column
                if low == high:
                    painter.drawPoint(QPoint(x, to_y(low)))
                else:
                    painter.drawLine(x, to_y(low), x, to_y(high))

    def _draw_time_axis(self, painter, height, t0, t1):
        plot_width = self.width() - LEFT_MARGIN - RIGHT_MARGIN
        if plot_width <= 0:
            return
        painter.setPen(COLOR_AXIS)
        y = height - BOTTOM_MARGIN + 2
        painter.drawLine(LEFT_MARGIN, y, LEFT_MARGIN + plot_width, y)
        painter.setPen(COLOR_TEXT)
        metrics = QFontMetrics(painter.font())
        # 只标起止和中间：刻度再密在这块地方也读不清。
        span_s = (t1 - t0) / 1e9
        for fraction, text, align in ((0.0, f"-{span_s:.1f}s", "left"),
                                      (0.5, f"-{span_s / 2:.1f}s", "center"),
                                      (1.0, "现在", "right")):
            x = LEFT_MARGIN + int(plot_width * fraction)
            offset = {"left": 0,
                      "center": metrics.width(text) // 2,
                      "right": metrics.width(text)}[align]
            painter.drawText(x - offset, y + 14, text)

    def mousePressEvent(self, event):
        """点一下清空——看一段新动作之前不用重启界面。"""
        if event.button() == Qt.LeftButton:
            self.clear()


class ChartPanel(QWidget):
    """五根轴的曲线，一次显示选中的那一根。切换由外面的下拉负责（见 app.py）。

    **为什么不做成五张并排**：三条曲线 × 五根轴 = 十五条线挤在一屏里没法读，而台架上
    要回答的问题是"这一根轴这次走得对不对"。所以要看得过来，而不是看得全。

    **一次只喂当前这一根轴。** 五根全收是五倍的内存和五倍的开销，而屏幕上一次只看得
    到一根。代价是切轴时曲线从切的那一刻开始——界面上标注了这一点。
    """

    def __init__(self, axis_count, seconds=60.0, hz=1000.0, parent=None):
        super().__init__(parent)
        # 五张图都塞进同一个格子，靠 show/hide 切换。没显示的那几张不画、也不收数据。
        stack = QVBoxLayout(self)
        stack.setContentsMargins(0, 0, 0, 0)
        self.charts = []
        self.current = 0
        for index in range(axis_count):
            chart = AxisChart(index, seconds=seconds, hz=hz)
            chart.hide()
            stack.addWidget(chart)
            self.charts.append(chart)
        if self.charts:
            self.charts[0].show()
        # 一次只显示一张，所以这个容器的最小高度就是**一张**图的最小高度。
        # 不写这一句的话，外层布局给这个容器的份额会少于里面那张图要的（实测 121 对
        # 170），三个横条就被压成三条细缝——而曲线恰恰是这一层最该看清楚的东西。
        self.setMinimumHeight(MIN_CHART_H)

    def append(self, frames):
        if 0 <= self.current < len(self.charts):
            self.charts[self.current].append(frames)

    def select(self, index):
        if not (0 <= index < len(self.charts)) or index == self.current:
            return
        self.charts[self.current].hide()
        self.current = index
        self.charts[index].show()
        self.charts[index].update()

    def active(self):
        """当前显示的那根轴的图表；一根轴都没有时是 None。"""
        return self.charts[self.current] if self.charts else None

    def clear(self):
        for chart in self.charts:
            chart.clear()
