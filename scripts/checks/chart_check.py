#!/usr/bin/env python3
"""曲线模块自己的检查：抽稀会不会丢形状、断开对不对、画不画得出来。

跑法：python scripts/checks/chart_check.py

**为什么这条检查非写不可。** 曲线是这个界面上唯一"看起来对但其实可能错"的东西：
一条被抽稀抹平了的曲线，和一条本来就很平缓的曲线，画出来一模一样，肉眼分不出。
而抽稀是这一层唯一会真的吃掉机器的地方（1 kHz × 四条序列），所以它也是唯一一个
"为了性能而故意丢数据"的地方——故意丢数据的地方就该有检查钉着。

钉的是**包络**：一桶记最小和最大两个值，尖峰必须还在。这里拿"每隔几个取一个"
那种朴素抽样做对照——尖峰在那种做法下必然丢，而在 min/max 桶里必须在。对照组存在的
意义是让这条断言有牙齿：如果哪天抽稀被改成了隔点抽样，这条检查要红。

本检查不连任何主站，纯粹喂人造帧。用 offscreen 后端，不需要显示器。
"""

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts", "checks"))

from PyQt5.QtGui import QImage                       # noqa: E402
from PyQt5.QtWidgets import QApplication             # noqa: E402

from emaster_gui import charts                       # noqa: E402

AXIS_COUNT = 5


class CheckFailed(AssertionError):
    pass


def want(condition, message):
    if not condition:
        raise CheckFailed(message)


def make_frame(index, stamp_ns, positions, torques=None):
    """造一帧，形状与 observation_client 解出来的那份一致。"""
    if torques is None:
        torques = [0] * len(positions)
    return {
        "monotonic_ns": stamp_ns,
        "publish_index": index,
        "cycle": index,
        "wkc": 0,
        "flags": 0,
        "frame_interval_ns": 1_000_000,
        "axes": [{"actual_position": positions[axis],
                  "target_position": positions[axis],
                  "actual_torque": torques[axis],
                  "status_word": 0x0427,
                  "flags": 0}
                 for axis in range(len(positions))]
    }


def feed(chart, values, start_index=1, period_ns=1_000_000, start_offset=0, **kwargs):
    """按 1 ms 一拍喂一串轴0的位置值，返回喂完的帧数。

    `start_offset` 是这串值的**起始时间**（第几拍）。分两段喂的时候必须接着上一段的
    时间往下走——本检查第一版就是每段都从第 0 拍开始，于是第二段的时间倒着回去了，
    「跳号处断开」那条报出 41 处断开。**那一版错的是检查自己，不是被检查的代码**，
    但报错信息（"应该正好 1 处"）指不到这里，所以这个参数和解法一并写在这儿。
    """
    torques = kwargs.get("torques")
    for offset, value in enumerate(values):
        positions = [value] + [0] * (AXIS_COUNT - 1)
        chart.append([make_frame(start_index + offset,
                                 (start_offset + offset) * period_ns,
                                 positions,
                                 torques[offset] if torques else None)])
    return len(values)


# ---------------------------------------------------------------- 抽稀

def check_buckets_shrink_the_data(context):
    """1 kHz 喂进去，存下来的桶数要**远少于**帧数——不然抽稀等于没做。"""
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    # 窗口 10 秒、一个视野 1000 个桶 → 10 ms 一桶。
    want(abs(chart.bucket_ns - 10_000_000) < 1000,
         f"10 秒窗、1 kHz 的桶宽应当是 10 ms，实际 {chart.bucket_ns / 1e6} ms")

    fed = feed(chart, [offset * 3 for offset in range(1000)])
    stored = len(chart.actual)
    want(stored <= fed / 5,
         f"喂了 {fed} 帧却存了 {stored} 个桶，抽稀没起作用")
    span = chart.actual.span_ns()
    want(span >= 900_000_000,
         f"桶的跨度只有 {span / 1e9:.3f} 秒，1000 帧不该只覆盖这么点")


def check_bucket_width_floor_is_the_sample_rate(context):
    """窗口短到桶比采样还密时，桶宽要停在采样间隔上，不能存得比采的还细。"""
    narrow = charts.bucket_width_ns(0.5, 1000.0)     # 0.5 s / 1000 = 0.5 ms < 1 ms
    want(narrow == 1_000_000,
         f"短窗口把桶宽压到了采样间隔以下：{narrow / 1e6} ms（应当是 1 ms）")
    wide = charts.bucket_width_ns(60.0, 1000.0)
    want(wide == 60_000_000, f"60 秒窗的桶宽应当 60 ms，实际 {wide / 1e6} ms")


def check_envelope_survives(context):
    """**这条是重点。** 只占一个采样点的尖峰，抽稀之后必须还在。

    对照：把同一串数按"每隔 k 个取一个"抽样，尖峰必丢。所以这条断言不是废话——
    它区分了"min/max 桶"和"隔点抽样"。
    """
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    values = [0] * 1000
    values[137] = 987_654        # 正尖峰，只占一拍
    values[612] = -654_321       # 负尖峰，也只在中间的桶里
    feed(chart, values)

    lo_min = min(chart.actual.lo)
    hi_max = max(chart.actual.hi)
    want(hi_max == 987_654,
         f"正尖峰没了：存下来的最大是 {hi_max}，喂进去的是 987654")
    want(lo_min == -654_321,
         f"负尖峰没了：存下来的最小是 {lo_min}，喂进去的是 -654321")

    # 对照组：朴素隔点抽样在同一串数上会丢掉两个尖峰。这一步是让上面两句有分量。
    stride = 16
    naive = values[::stride]
    want(max(naive) != 987_654 and min(naive) != -654_321,
         f"对照组居然也保住了尖峰（step={stride}，max={max(naive)}）——"
         f"那这条检查就证明不了 min/max 桶比隔点抽样强，得换个步长")


def check_flat_series_still_stores(context):
    """值一直不变时桶也要在——不然一条平线画不出来，看着像没有数据。"""
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    feed(chart, [1234] * 500)
    want(len(chart.actual) > 0, "一条平的序列一个桶都没存")
    lows, highs, _broken = chart.actual.columns(0, chart.actual.span_ns() + 1, 400)
    drawn = [index for index, value in enumerate(lows) if value is not None]
    want(drawn, "平序列摊到像素列上以后一列都没有")


# ---------------------------------------------------------------- 断开

def check_gap_breaks_the_line(context):
    """帧号跳号处要标成断开，画的时候那一列空着，不能连成一条直线。"""
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    feed(chart, [0] * 50, start_index=1, start_offset=0)
    # 跳过 50 帧：帧号从 50 直接到 101，时间接着第 50 拍往下走（不是回到第 0 拍）。
    feed(chart, [0] * 50, start_index=101, start_offset=50)

    want(chart.gaps == 1, f"报告了 {chart.gaps} 处断开，应该正好 1 处")
    want(len(chart.actual.breaks) == 1, "断开没记在序列上")

    t0, t1 = 0, chart.actual.span_ns() + 1
    lows, _highs, broken = chart.actual.columns(t0, t1, 200)
    want(len(broken) == 1, f"摊到列上没有断开标记：{broken}")
    column = next(iter(broken))
    want(lows[column] is not None,
         "断开的列恰好没有数据，那这条检查证明不了「断开处是空着的」")
    # 断开的列不参与绘制（_draw_pane 里跳过它），所以它两侧不会连起来。


def check_nothing_dropped_when_contiguous(context):
    """帧号连续时一处断开都不该有——不然曲线会凭空裂开。"""
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    feed(chart, [0] * 300, start_index=7)
    want(chart.gaps == 0, f"连续帧号却报了 {chart.gaps} 处断开")


def check_out_of_order_frame_is_visible(context):
    """时间倒着来的帧要**记一笔**再丢，不能悄悄塞进去把顺序搞乱。

    正常跑不会出现（帧按 publish_index 递增取回来，monotonic_ns 单调），但"正常不会
    发生"是假设不是保证。悄悄塞进去的话，桶的时间就不是有序的了，摊列时会画出
    鬼影；悄悄丢掉的话，丢了数据却没人知道。所以：记成断开 + 计数。
    """
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    feed(chart, [0] * 100)
    before = len(chart.actual)
    chart.append([make_frame(500, 10_000_000, [0] * AXIS_COUNT)])   # 回到第 10 拍
    want(len(chart.actual) == before,
         "倒着来的帧被存进去了，桶的时间顺序被破坏")
    want(chart.actual.disordered == 1,
         f"倒着来的帧被悄悄丢了（disordered={chart.actual.disordered}）")


# ---------------------------------------------------------------- 摊列与绘制

def check_columns_merge_buckets(context):
    """两个桶落进同一列时，上下界要并起来，不能只留其中一个。"""
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    values = [10, 20, 30, 40, 500, 60, 1]
    feed(chart, values)
    # 摊成 1 列：所有桶都挤进同一列，上下界应当是全体的最小/最大。
    lows, highs, _broken = chart.actual.columns(0, chart.actual.span_ns() + 1, 1)
    want(lows[0] == min(values),
         f"合并后下界是 {lows[0]}，应当是 {min(values)}")
    want(highs[0] == max(values),
         f"合并后上界是 {highs[0]}，应当是 {max(values)}")


def check_paints_without_crashing(context):
    """offscreen 下真的跑一遍 paintEvent。除零、数组越界这类错在这里会现形。

    只断言"画出来有东西"：背景是浅灰，只要有一批像素不是背景色，就说明笔真的落下去
    了。这条能抓住的是"曲线画成了一片空白"——而那正是抽稀写错时的长相。
    """
    chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
    chart.resize(640, 300)
    values = [int(200_000 * (offset % 100) / 100.0) for offset in range(2000)]
    feed(chart, values)

    image = QImage(640, 300, QImage.Format_RGB32)
    image.fill(charts.COLOR_BG)
    chart.render(image)

    background = charts.COLOR_BG.rgb() & 0x00FFFFFF
    painted = 0
    for y in range(0, 300, 3):
        for x in range(0, 640, 3):
            if (image.pixel(x, y) & 0x00FFFFFF) != background:
                painted += 1
    want(painted > 50,
         f"渲染完只有 {painted} 个采样点不是背景色，曲线基本没画出来")


def check_paints_empty_and_flat(context):
    """没数据、以及一条平线，都要能画出来不炸——这两种是最常见的开场状态。"""
    for label, values in (("空", []), ("平线", [4321] * 200)):
        chart = charts.AxisChart(0, seconds=10.0, hz=1000.0)
        chart.resize(400, 220)
        if values:
            feed(chart, values)
        image = QImage(400, 220, QImage.Format_RGB32)
        image.fill(charts.COLOR_BG)
        chart.render(image)      # 不炸就算过
        want(True, label)


def check_panel_only_feeds_the_shown_axis(context):
    """一次只收当前显示的那根轴——五根全收是五倍的开销，而屏幕上只看得到一根。"""
    panel = charts.ChartPanel(AXIS_COUNT, seconds=10.0, hz=1000.0)
    feed(panel.charts[0], [1] * 10)     # 直接喂 0 号，不经过 panel
    panel.append([make_frame(1, 0, [5] * AXIS_COUNT)])
    want(panel.charts[0].fed == 11,
         f"当前轴的帧数不对：{panel.charts[0].fed}")
    for index in range(1, AXIS_COUNT):
        want(panel.charts[index].fed == 0,
             f"轴{index + 1} 也收了数据，但当时显示的不是它")

    panel.select(2)
    panel.append([make_frame(2, 1_000_000, [5] * AXIS_COUNT)])
    want(panel.charts[2].fed == 1, "切过去之后新轴没有开始收数据")
    want(panel.charts[0].fed == 11, "切换时把上一根轴的数据动了")


CHECKS = [
    ("抽稀真的发生：1 kHz 存下来的桶数远少于帧数", check_buckets_shrink_the_data),
    ("桶宽下限停在采样率上", check_bucket_width_floor_is_the_sample_rate),
    ("包络不丢：单拍尖峰抽稀之后还在（带隔点抽样对照）", check_envelope_survives),
    ("平序列照样存桶、照样摊得出列", check_flat_series_still_stores),
    ("帧号跳号处标成断开", check_gap_breaks_the_line),
    ("帧号连续时一处断开都没有", check_nothing_dropped_when_contiguous),
    ("倒着来的帧记一笔再丢，不悄悄破坏时间序", check_out_of_order_frame_is_visible),
    ("两个桶落进同一列时上下界并起来", check_columns_merge_buckets),
    ("offscreen 下真的画得出来（有像素落下）", check_paints_without_crashing),
    ("没数据和一条平线都画得出来", check_paints_empty_and_flat),
    ("只喂当前显示的那根轴", check_panel_only_feeds_the_shown_axis),
]

NOT_COVERED = [
    "**画出来的形状对不对**：这里只数了「有像素落下」，没有比对像素位置。断言某个值"
    "该落在第几行，等于把 to_y 重抄一遍，抄错了照样绿。台架上人看一眼曲线跟轴的实际"
    "动作对不对得上——这条只能靠眼睛。",
    "**长时间跑的内存与耗时**：桶数有上限（一视野 1000 桶的两倍），理论上会停住，但"
    "这里没跑够长时间去验。台架上跑长一段，看常驻内存平不平。",
]


def main():
    # 和界面本身装的是同一个钩子。**这条不是装饰**：跑在 paintEvent 里的 Python 异常
    # PyQt 会当致命错误处理，默认长相是"进程直接没了、一个字都没有"，本次开发就在这上面
    # 迷了很久（退出码 127，连 -X faulthandler 都不给 traceback）。装上之后至少能看到
    # 是哪一行。
    from emaster_gui.app import install_excepthook
    install_excepthook()
    app = QApplication(sys.argv[:1])
    failures = []
    for name, function in CHECKS:
        try:
            function({})
        except Exception as error:      # noqa: BLE001 —— 检查项里什么都可能抛
            print(f"FAIL  {name}")
            print(f"      {type(error).__name__}: {error}")
            failures.append(name)
        else:
            print(f"ok    {name}")

    print()
    print(f"{len(CHECKS) - len(failures)}/{len(CHECKS)} 项通过")
    if failures:
        print("失败：")
        for name in failures:
            print(f"  - {name}")
    if failures:
        print()
        print("没证到的：")
        for item in NOT_COVERED:
            print(f"  - {item}")
    del app
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
