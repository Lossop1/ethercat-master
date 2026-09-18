#!/usr/bin/env python3
"""独立于主站的图形界面：能及时下发控制，也能看着监控量。

**独立是什么意思。** 这个进程不加载、不起停、也不链接主站——它只是主站两个套接字的
另一个客户端。主站挂了它照样在，它挂了主站也不知道（命令口的空闲超时会把连接收掉）。
所以它可以随便关，不会把主站带下去。

**为什么默认只连观测口。** 两条通道的分量完全不同：

    观测口  只读。连上以后这个进程物理上不可能让轴动起来。
    命令口  能驱动电机。而且服务端只有一格 accept——被界面占着，台架脚本
            就再也连不上了（表现为"挂住"，不报错，很难查）。

台架上"主站开着就要开着监控"这条纪律，要的就是一个能一直开着的窗口。所以窗口一起来
只连观测；要用命令得点「接管」，用完点「释放」。

**慢速量（温度/母线电压/电流）在这里是「—」。** 它们不在任何套接字上，只在主站停机后
写的报告 JSON 里。本轮不做实时通路，界面上留位、如实显示为未知，而不是画一条假的。

用法：
    python -m emaster_gui --deployment orangepi-bench-quint-30deg
    python -m emaster_gui --tcp 192.168.124.81:5001
    QT_QPA_PLATFORM=offscreen python -m emaster_gui --obs-endpoint tcp:127.0.0.1:5002 \
        --selftest 20
"""

import argparse
import os
import sys
import traceback

from PyQt5.QtCore import QTimer
from PyQt5.QtWidgets import (QApplication, QComboBox, QDoubleSpinBox, QGridLayout,
                             QGroupBox, QHBoxLayout, QHeaderView, QLabel, QMainWindow,
                             QMessageBox, QPlainTextEdit, QPushButton, QSizePolicy,
                             QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

import emaster_client
import emaster_endpoint
import rt_affinity

from emaster_client import MASTER_STATES
from emaster_console import DEFAULT_JOG_SPEED, DEFAULT_RANGE_DEG
from emaster_gui import charts, poller

# 表格里那几列的标题。顺序就是"出事时你会想先看谁"。
AXIS_COLUMNS = ["轴", "实际(相对)", "已提交目标", "本地目标", "本次曾到过",
                "余量(负/正)", "状态字", "错误码", "状态"]


def _fmt_deg(value):
    return "—" if value is None else f"{value:+.3f}°"


def _fmt_counts(value):
    return "—" if value is None else f"0x{value:04x}"


def _wrap_note(text):
    """一段会跟着窗口折行的说明文字。

    **必须配 Ignored 的水平尺寸策略。** 中文没有空格，Qt 的自动折行找不到断点，于是
    `minimumSizeHint()` 会把整整一句话的宽度报上来——布局就照这个数把窗口顶宽（实测
    一段说明文字把动作区顶到了 1586 像素）。`Ignored` 是告诉布局"别拿它当最小宽度"，
    文字照样按可用宽度折行。
    """
    label = QLabel(text)
    label.setWordWrap(True)
    label.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
    label.setStyleSheet("color: #666;")
    return label


class MainWindow(QMainWindow):
    def __init__(self, args, repo_root):
        super().__init__()
        self.args = args
        self.repo_root = repo_root

        endpoints = emaster_endpoint.resolve_endpoints(
            args, self._parser(), need=("observation",))
        self.obs_endpoint = endpoints["observation"]
        # 命令端点这里只解析、不连接：gated behind 「接管」。
        self.cmd_endpoint = None
        if args.deployment or args.cmd_endpoint or args.tcp:
            cmd = emaster_endpoint.resolve_endpoints(
                args, self._parser(), need=("command",)).get("command")
            self.cmd_endpoint = cmd

        self.affinity = rt_affinity._Decision(
            requested=rt_affinity.parse_cpu_list(args.cpu) if args.cpu else None,
            log=self._log)
        self.poller = None
        self.session = None
        self.last_obs = None
        self.last_snapshot = None

        self.setWindowTitle(f"EtherCAT 主站监控 · {self.obs_endpoint.display}")
        self.resize(1180, 720)
        self._build()
        self._start_observation()

    def _parser(self):
        # resolve_endpoints 出错时要 parser.error()，给它一个能认这些属性的壳即可。
        return self.args._parser

    # ---------------------------------------------------------------- 界面
    def _build(self):
        root = QWidget()
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)

        outer.addWidget(self._build_link_row())
        outer.addWidget(self._build_axes(), 2)
        outer.addWidget(self._build_charts(), 3)

        lower = QHBoxLayout()
        lower.addWidget(self._build_health(), 1)
        lower.addWidget(self._build_log(), 2)
        outer.addLayout(lower, 2)

        outer.addWidget(self._build_actions())
        self.statusBar().showMessage("就绪")

    def _build_link_row(self):
        box = QGroupBox("连接")
        row = QHBoxLayout(box)
        self.obs_label = QLabel("观测：未连")
        self.cmd_label = QLabel("命令：未接管")
        row.addWidget(self.obs_label, 1)
        row.addWidget(self.cmd_label, 1)
        self.takeover_button = QPushButton("接管命令通道")
        self.takeover_button.clicked.connect(self._toggle_takeover)
        row.addWidget(self.takeover_button)
        row.addWidget(_wrap_note("默认只连观测口（只读）。命令口能驱动电机，"
                                 "且主站只接受一个客户端——要发命令才点「接管」。"), 2)
        return box

    def _build_axes(self):
        box = QGroupBox("轴")
        layout = QVBoxLayout(box)
        self.axes_table = QTableWidget(0, len(AXIS_COLUMNS))
        self.axes_table.setHorizontalHeaderLabels(AXIS_COLUMNS)
        self.axes_table.verticalHeader().setVisible(False)
        self.axes_table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.axes_table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        layout.addWidget(self.axes_table)
        self.axes_hint = _wrap_note("轴表要先「接管」才有——位置/状态字只在命令通道的 "
                                    "status 里；观测帧里的反馈在下面的健康量里。")
        layout.addWidget(self.axes_hint)
        return box

    def _build_charts(self):
        box = QGroupBox("曲线（自绘；一次显示一根轴）")
        layout = QVBoxLayout(box)

        row = QHBoxLayout()
        row.addWidget(QLabel("轴"))
        self.chart_axis_combo = QComboBox()
        self.chart_axis_combo.currentIndexChanged.connect(self._select_chart_axis)
        self.chart_axis_combo.setEnabled(False)
        row.addWidget(self.chart_axis_combo)
        self.chart_hint = _wrap_note("等第一帧观测到了才知道有几根轴。")
        row.addWidget(self.chart_hint, 1)
        layout.addLayout(row)

        # 曲线控件要等知道轴数才建（见 _ensure_charts），先留个格子。
        self.chart_holder = QVBoxLayout()
        layout.addLayout(self.chart_holder, 1)
        self.chart_panel = None
        return box

    def _ensure_charts(self, frame):
        """第一帧到手才建曲线控件：轴数和主站的出帧周期都要看过帧才知道。

        出帧周期不能拿 `--tick-hz` 顶替——那是**控制拍**（默认 50 Hz，是我发命令的
        节奏），而帧是主站按自己的周期发的（1 kHz 量级）。拿 50 Hz 当采样率会把桶宽
        的下限定到 20 ms，比真实周期粗 20 倍，曲线上的细节先被自己的抽稀抹一遍。
        所以直接读帧里那个数：它本来就是主站报出来的事实。
        """
        axis_count = len(frame["axes"])
        if self.chart_panel is not None or axis_count <= 0:
            return
        interval_ns = frame.get("frame_interval_ns") or 0
        rate_hz = 1e9 / interval_ns if interval_ns > 0 else 1000.0
        self.chart_panel = charts.ChartPanel(axis_count, seconds=self.args.chart_seconds,
                                             hz=rate_hz)
        self.chart_holder.addWidget(self.chart_panel)
        for index in range(axis_count):
            self.chart_axis_combo.addItem(f"轴{index + 1}")
        self.chart_axis_combo.setEnabled(True)
        active = self.chart_panel.active()
        self.chart_hint.setText(
            f"{axis_count} 根轴，主站出帧约 {rate_hz:.0f} Hz。桶宽 "
            f"{active.bucket_ns / 1e6:.1f} ms，窗口 {self.args.chart_seconds:g} 秒"
            f"（一桶记最小/最大两个值，包络不丢）。切换轴时那根轴的曲线从切的那一刻"
            f"开始（没显示的不收数据）；点一下图清空。")

    def _select_chart_axis(self, index):
        if self.chart_panel is not None:
            self.chart_panel.select(index)

    def _build_health(self):
        box = QGroupBox("主站健康量（来自观测帧）")
        layout = QGridLayout(box)
        self.health_labels = {}
        rows = [
            ("link", "观测链路"), ("frames", "收到帧数"),
            ("publish", "publish_index"), ("cycle", "cycle"),
            ("wkc", "WKC"), ("interval", "帧距"),
            ("flags", "帧级标志"), ("gaps", "跳号/估算丢失"),
            ("axes_flags", "轴级标志"),
            ("slow", "温度/电压/电流"),
        ]
        for row, (key, title) in enumerate(rows):
            layout.addWidget(QLabel(title), row, 0)
            value = QLabel("—")
            value.setWordWrap(True)
            layout.addWidget(value, row, 1)
            self.health_labels[key] = value
        layout.setColumnStretch(1, 1)
        self.health_labels["slow"].setText("—（不在套接字上，只在停机报告里）")
        return box

    def _build_log(self):
        box = QGroupBox("消息")
        layout = QVBoxLayout(box)
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(500)
        layout.addWidget(self.log_view)
        return box

    def _build_actions(self):
        """动作区。**三行各自独立**，不是一张网格。

        这里连着踩了两次同一类坑，都跟"窗口最小宽度被顶爆"有关，所以都写在这儿：

          1. 全部按钮摆一行 → 最小宽度 2127 px，比 1920 的屏还宽。窗口拉不到那么宽，
             右侧的按钮就永远看不见，而那些按钮里有急停。
          2. 改成一格网格（按钮在上、输入在下）→ 还是 1586。因为网格把**同一列里上下
             两行**的最小宽度取最大值，而跨列的那段说明文字，Qt 会**每跨一列就记一遍**，
             四个列就是四倍。

        所以现在是三个独立的横排，各算各的。按钮文字也砍短了：`halt 1` 这种命令名
        放悬停提示里就够了，脸上写着「急停」比「急停 halt 1」更快看懂，"命令名写脸上"
        反而看不出哪个是急停。
        """
        box = QGroupBox("动作")
        column = QVBoxLayout(box)

        self.halt_button = QPushButton("急停")
        self.halt_button.setToolTip("发 halt 1：主站控制字的 bit8，粘性暂停（冻结目标）")
        self.halt_button.clicked.connect(lambda: self._request("halt"))
        self.resume_button = QPushButton("复位（清故障）")
        self.resume_button.setToolTip("发 halt 0 再 fault_reset：解除暂停并把驱动器拉回使能")
        self.resume_button.clicked.connect(lambda: self._request("resume"))
        self.quick_stop_button = QPushButton("快停")
        self.quick_stop_button.setToolTip(
            "发 quick_stop：驱动器按 0x605A 自己减速停车（CiA402 的 bit2）。"
            "降速期间轴是真在动的，恢复要用「复位」")
        self.quick_stop_button.clicked.connect(self._quick_stop)
        self.home_button = QPushButton("回启动位置")
        self.home_button.setToolTip("所有轴一起回到本次会话的启动位置")
        self.home_button.clicked.connect(lambda: self._request("home"))
        self.reanchor_button = QPushButton("设为零点")
        self.reanchor_button.setToolTip("把当前位置设成新的零点（只改本地角度口径）")
        self.reanchor_button.clicked.connect(lambda: self._request("reanchor"))

        stops = QHBoxLayout()
        for button in (self.halt_button, self.resume_button, self.quick_stop_button,
                       self.home_button, self.reanchor_button):
            stops.addWidget(button)
        stops.addWidget(_wrap_note("这五个都不会让主站停机——真正的停机是给主站 "
                                   "SIGINT。命令协议里的 stop/shutdown 是空操作。"), 1)
        column.addLayout(stops)

        walk_row = QHBoxLayout()
        walk_row.addWidget(QLabel("轴号"))
        self.axis_spin = QDoubleSpinBox()
        self.axis_spin.setRange(1, 16)
        self.axis_spin.setDecimals(0)
        walk_row.addWidget(self.axis_spin)
        walk_row.addWidget(QLabel("相对度数"))
        self.degree_spin = QDoubleSpinBox()
        self.degree_spin.setRange(-360.0, 360.0)
        self.degree_spin.setDecimals(3)
        self.degree_spin.setSingleStep(0.5)
        walk_row.addWidget(self.degree_spin)
        walk = QPushButton("走过去")
        walk.clicked.connect(self._walk)
        walk_row.addWidget(walk)
        walk_row.addWidget(QLabel("点动"))
        for label, delta in (("−0.5°", -0.5), ("+0.5°", 0.5), ("−2°", -2.0), ("+2°", 2.0)):
            button = QPushButton(label)
            button.clicked.connect(lambda _checked, d=delta: self._nudge(d))
            walk_row.addWidget(button)
        walk_row.addStretch(1)
        column.addLayout(walk_row)
        return box

    # ---------------------------------------------------------------- 观测
    def _start_observation(self):
        self.poller = poller.ObservationPoller(
            self.obs_endpoint, hz=self.args.obs_hz, window=self.args.obs_window,
            affinity=self.affinity)
        self.poller.frames.connect(self._on_frames)
        self.poller.link.connect(self._on_obs_link)
        self.poller.note.connect(self._log)
        self.poller.start()

    def _on_obs_link(self, payload):
        self.obs_label.setText(payload["text"])
        self.obs_label.setStyleSheet("color: #0a0;" if payload["ok"] else "color: #a00;")
        self.health_labels["link"].setText(payload["text"])

    def _on_frames(self, payload):
        self.last_obs = payload
        frames = payload["frames"]
        if not frames:
            return
        last = frames[-1]

        # 轴数只有拿到帧才知道，所以曲线控件在这里才建得出来。
        self._ensure_charts(last)
        if self.chart_panel is not None:
            self.chart_panel.append(frames)

        self.health_labels["frames"].setText(
            f"{payload['received']}（本次这批 {len(frames)} 帧）")
        self.health_labels["publish"].setText(
            f"{last['publish_index']}（可读下界 {payload['oldest']}，"
            f"上界 {payload['newest']}，环 {payload['capacity']} 槽）")
        self.health_labels["cycle"].setText(str(last["cycle"]))
        self.health_labels["wkc"].setText(str(last["wkc"]))
        self.health_labels["interval"].setText(
            f"{last['frame_interval_ns'] / 1e6:.3f} ms")
        self.health_labels["flags"].setText(emaster_client.frame_flags_text(last["flags"]))
        self.health_labels["gaps"].setText(
            f"跳号 {payload['gaps']} 处，估算丢失 {payload['missed']} 帧，"
            f"重新对齐 {payload['resyncs']} 次")

        axis_flags = 0
        for frame in frames:
            for axis in frame["axes"]:
                axis_flags |= axis["flags"]
        self.health_labels["axes_flags"].setText(
            emaster_client.axis_flags_text(axis_flags))
        self._fill_axes_from_frames(last, axis_flags)

    def _fill_axes_from_frames(self, frame, axis_flags):
        """只读模式下也能填轴表——用观测帧的反馈量，填不了的格子写「—」。

        这不是把两套数据凑成一套：观测帧里**没有** planned/err/state/mode（那几项只在
        命令通道的 status 里，见 emaster_client 开头）。所以这两栏在只读模式下是未知，
        接管之后才由快照填上。把未知写成 0 才是错的。
        """
        if self.session is not None:
            return      # 接管时快照更全（有 planned/err/state），由它来填
        axes = frame["axes"]
        self.axes_table.setRowCount(len(axes))
        for index, axis in enumerate(axes):
            values = [
                str(index + 1),
                f"{axis['actual_position']} counts",
                "—（要接管）",
                "—（要接管）",
                "—（要接管）",
                "—（要接管）",
                f"0x{axis['status_word']:04x}",
                "—（要接管）",
                emaster_client.axis_flags_text(axis["flags"]),
            ]
            self._set_row(index, values)
        self.axes_hint.setText("只读模式：位置是观测帧的原始 counts（没有角度换算——"
                               "那要用命令通道的拓扑）。「接管」之后按相对启动位置显示。")

    # ---------------------------------------------------------------- 命令
    def _toggle_takeover(self):
        if self.session is not None:
            self._release()
        else:
            self._takeover()

    def _takeover(self):
        if self.cmd_endpoint is None:
            self._alert("没有命令端点",
                        "这次启动只给了观测端点。要接管请给 --deployment 或 --tcp HOST:PORT。")
            return
        allowed, why = poller.command_preflight(self.args.deployment, self.repo_root)
        if not allowed:
            self._alert("这个部署不能接管", why)
            return
        if not self.args.no_confirm:
            answer = QMessageBox.question(
                self, "接管命令通道",
                f"要连上命令通道 {self.cmd_endpoint.display}。\n\n"
                f"连上之后这个窗口就能驱动电机了。动手之前确认：\n"
                f"  1) 轴周围没人、没有夹具；\n"
                f"  2) 5 号从站（小电机）刚跑过的话先降温；\n"
                f"  3) 别的客户端（台架脚本、TUI 面板）没占着命令口。\n\n"
                f"接管吗？",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if answer != QMessageBox.Yes:
                return

        self.session = poller.CommandSession(
            self.cmd_endpoint, self.args.deployment, self.repo_root,
            jog_speed=self.args.jog_speed, range_deg=self.args.range_deg,
            tick_hz=self.args.tick_hz, status_hz=self.args.status_hz,
            affinity=self.affinity)
        self.session.snapshot.connect(self._on_snapshot)
        self.session.note.connect(self._log)
        self.session.link.connect(self._on_cmd_link)
        self.session.start()
        self.takeover_button.setText("释放命令通道")
        self._log(f"命令：接管中（{self.cmd_endpoint.display}）")

    def _release(self):
        if self.session is None:
            return
        self.session.stop()
        if not self.session.wait(3000):
            self._log("命令：线程没在 3 秒内退出，先不等了（它会自己收尾）")
        self.session = None
        # last_snapshot **不清**：它是会话的结算（各轴最后停在哪儿、发过几条目标），
        # 释放之后再清掉，事后就没有任何凭据了。轴表回不回到只读由 session 是否为
        # None 决定（见 _fill_axes_from_frames），不靠这个字段。
        self.takeover_button.setText("接管命令通道")
        self.cmd_label.setText("命令：未接管")
        self._log("命令：已释放")

    def _on_cmd_link(self, payload):
        self.cmd_label.setText(payload["text"])
        self.cmd_label.setStyleSheet("color: #0a0;" if payload["ok"] else "color: #a00;")

    def _on_snapshot(self, snapshot):
        self.last_snapshot = snapshot
        state = snapshot["state"]
        name = MASTER_STATES.get(state, f"未知({state})")
        self.setWindowTitle(f"EtherCAT 主站监控 · {self.obs_endpoint.display}"
                            f" · 接管中 · 主站 {name}")
        axes = snapshot["axes"]
        self.axes_table.setRowCount(len(axes))
        for index, axis in enumerate(axes):
            headroom = axis["headroom"]
            values = [
                str(index + 1),
                _fmt_deg(axis["pos_deg"]),
                _fmt_deg(axis["planned_deg"]),
                _fmt_deg(axis["desired_deg"]),
                _fmt_deg(axis["peak_deg"]),
                ("—" if headroom is None
                 else f"{headroom[0]:+.2f}° / {headroom[1]:+.2f}°"),
                _fmt_counts(axis["status_word"]),
                f"0x{axis['error_code']:04x}",
                axis["state_label"],
            ]
            self._set_row(index, values)
        self.axes_hint.setText(
            f"接管中：{snapshot['deployment']}，{snapshot['axis_count']} 轴，"
            f"点动速度 {snapshot['jog_speed']:g}°/s，软范围 ±{snapshot['range_deg']:g}°，"
            f"已发 {snapshot['sent_count']} 条目标。角度都是相对启动位置。"
            + ("　**已急停**" if snapshot["halted"] else ""))

    def _set_row(self, row, values):
        for column, text in enumerate(values):
            item = self.axes_table.item(row, column)
            if item is None:
                item = QTableWidgetItem()
                self.axes_table.setItem(row, column, item)
            item.setText(text)

    # ---------------------------------------------------------------- 动作
    def _request(self, action, *args):
        if self.session is None:
            self._log(f"「{action}」要先把命令通道接管起来")
            return
        self.session.request(action, *args)

    def _quick_stop(self):
        if not self.args.no_confirm:
            answer = QMessageBox.question(
                self, "快停", "快停会按驱动器 0x605A 的方式减速停车，期间轴是真的在动。\n"
                              "确认要发吗？",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if answer != QMessageBox.Yes:
                return
        self._request("quick_stop")

    def _walk(self):
        if not self.args.no_confirm:
            answer = QMessageBox.question(
                self, "让轴动起来",
                f"轴 {int(self.axis_spin.value())} 要走到启动位置 "
                f"{self.degree_spin.value():+.3f}°。\n确认吗？",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if answer != QMessageBox.Yes:
                return
        self._request("move", [(int(self.axis_spin.value()) - 1,
                               float(self.degree_spin.value()))])

    def _nudge(self, degrees):
        if not self.args.no_confirm:
            answer = QMessageBox.question(
                self, "点动",
                f"轴 {int(self.axis_spin.value())} 点动 {degrees:+.2f}°。\n确认吗？",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if answer != QMessageBox.Yes:
                return
        self._request("nudge", int(self.axis_spin.value()) - 1, float(degrees))

    def _log(self, text):
        self.log_view.appendPlainText(text)

    def _alert(self, title, text):
        """告诉人一件他必须知道的事。

        **无头模式（--no-confirm）下只记消息，绝不弹窗。** 离屏后端里的模态对话框
        没人能点，实测直接把进程打成访问违例（退出码 3221225477）——而无头正是台架上
        跑这一层的唯一方式。所以无头下消息同时进消息区和 stdout，好让脚本能断言。
        """
        self._log(f"{title}：{text}")
        if self.args.no_confirm:
            print(f"{title}：{text}", flush=True)
            return
        QMessageBox.warning(self, title, text)

    def shutdown(self):
        """把两条后台线程收干净。关窗和自检跑完都走这里。

        无头自检里没人点关闭，closeEvent 根本不会跑，`app.exec_()` 一返回解释器就
        开始收尾——而取数线程那时多半还在一次 socket 调用里。它接着抛的异常没人接得
        住，痕迹是 stderr 上冒一句"界面内部异常"、退出码却还是 0：看着像界面崩了，
        其实只是收尾没等人。
        """
        if self.session is not None:
            self._release()
        if self.poller is not None:
            self.poller.stop()
            self.poller.wait(3000)

    def closeEvent(self, event):
        self.shutdown()
        event.accept()

    # ---------------------------------------------------------------- 自检
    def _report_selftest(self):
        """无头自检的收尾报告。台架脚本与离线回归都解析这一段。

        打的是**事实**：收到了多少帧、跳了多少号、快照里各轴在哪儿。没有证据的
        断言不往这里写——"没证据"本身要能看出来。
        """
        print("---- GUI 自检 ----")
        # 最小宽度是**能被布局自己顶坏**的东西：中文说明文字没有空格、跨列摆放会被
        # 重复计入。这里连着踩过两次（2127 px、1586 px），而症状是"右侧按钮看不见"
        # ——包括急停。所以把它当成一个要守的数报出来，让离线检查盯着。
        print(f"界面：最小宽度 {self.minimumSizeHint().width()} px，"
              f"窗口 {self.width()}x{self.height()}")
        if self.chart_panel is not None:
            print(f"界面：曲线区已经拿到 {self.chart_panel.height()} px"
                  f"（至少要 {charts.MIN_CHART_H}）")
        print(f"观测端点：{self.obs_endpoint.display}")
        print(f"命令端点：{self.cmd_endpoint.display if self.cmd_endpoint else '（未给，不会接管）'}")
        print(f"接管中：{'是' if self.session is not None else '否'}")
        if self.last_obs is None:
            print("观测：一帧都没收到")
        else:
            payload = self.last_obs
            last = payload["frames"][-1]
            print(f"观测：收到 {payload['received']} 帧，跳号 {payload['gaps']} 处，"
                  f"估算丢失 {payload['missed']} 帧，重新对齐 {payload['resyncs']} 次")
            print(f"观测：publish_index={last['publish_index']} cycle={last['cycle']} "
                  f"wkc={last['wkc']} 帧距={last['frame_interval_ns'] / 1e6:.3f}ms "
                  f"帧级标志={emaster_client.frame_flags_text(last['flags'])}")
        self._report_charts()
        if self.last_snapshot is None:
            print("轴表：没有快照（这次自检从头到尾没接管过）")
        else:
            snapshot = self.last_snapshot
            print(f"轴表：快照来自"
                  f"{'当前' if self.session is not None else '已释放的那次'}会话")
            print(f"主站：state={snapshot['state']} "
                  f"({MASTER_STATES.get(snapshot['state'], '未知')}) "
                  f"已发 {snapshot['sent_count']} 条目标，"
                  f"相邻目标最大增量 {snapshot['max_step_sent']} counts")
            for axis in snapshot["axes"]:
                print(f"  轴{axis['index'] + 1}: 实际={_fmt_deg(axis['pos_deg'])} "
                      f"已提交={_fmt_deg(axis['planned_deg'])} "
                      f"本地目标={_fmt_deg(axis['desired_deg'])} "
                      f"曾到过={_fmt_deg(axis['peak_deg'])} "
                      f"状态字={_fmt_counts(axis['status_word'])} "
                      f"错误码=0x{axis['error_code']:04x} {axis['state_label']}")
        print("---- 自检结束 ----")

    def _report_charts(self):
        if self.chart_panel is None:
            print("曲线：没有控件（一帧都没收到，不知道有几根轴）")
            return
        print(f"曲线：{len(self.chart_panel.charts)} 根轴，当前显示轴"
              f"{self.chart_panel.current + 1}，窗口 {self.args.chart_seconds:g} 秒")
        for chart in self.chart_panel.charts:
            stats = chart.stats()
            if not stats["fed"]:
                print(f"  轴{stats['axis']}: 没喂过帧（当前不是它）")
                continue
            print(f"  轴{stats['axis']}: 喂了 {stats['fed']} 帧，"
                  f"存成 {stats['buckets']} 个桶（每桶 {stats['bucket_ms']:.1f} ms），"
                  f"跨度 {stats['span_s']:.2f} 秒，断开 {stats['gaps']} 处，"
                  f"时间倒序 {stats['disordered']} 帧")


def build_parser():
    ap = argparse.ArgumentParser(
        description="EtherCAT 主站监控界面（独立于主站，默认只读）")
    emaster_endpoint.add_endpoint_arguments(ap, command=True, observation=True)
    ap.add_argument("--obs-hz", type=float, default=20.0,
                    help="观测取数频率（默认 20 Hz；× 窗口帧数就是取数能力）")
    ap.add_argument("--obs-window", type=int, default=poller.DEFAULT_OBS_WINDOW,
                    help=f"每次取多少帧（默认 {poller.DEFAULT_OBS_WINDOW}）")
    ap.add_argument("--tick-hz", type=float, default=50.0, help="控制拍频率（默认 50 Hz）")
    ap.add_argument("--status-hz", type=float, default=10.0,
                    help="查 status / 心跳频率（默认 10 Hz；必须快过服务端 5 秒空闲超时）")
    ap.add_argument("--jog-speed", type=float, default=DEFAULT_JOG_SPEED,
                    help=f"点动速度 度/秒（默认 {DEFAULT_JOG_SPEED:g}）")
    ap.add_argument("--range-deg", type=float, default=DEFAULT_RANGE_DEG,
                    help=f"软范围 度（默认 ±{DEFAULT_RANGE_DEG:g}；0 = 不限制）")
    ap.add_argument("--chart-seconds", type=float, default=60.0,
                    help="曲线窗口长度，秒（默认 60；桶宽按它摊，见 charts.py）")
    ap.add_argument("--cpu", default=None,
                    help="绑到这些核上（如 0-3）。默认自动躲开主站的实时核")
    ap.add_argument("--no-confirm", action="store_true",
                    help="不弹二次确认（仅用于无头回归；台架上别开）")
    ap.add_argument("--selftest", type=float, default=None, metavar="秒",
                    help="无头自检：连上后跑这么多秒再退出（配 QT_QPA_PLATFORM=offscreen）")
    ap.add_argument("--selftest-takeover", action="store_true",
                    help="自检时也接管命令通道并走一遍小幅度动作")
    ap.add_argument("--selftest-recip", type=float, default=None, metavar="度",
                    help="自检时走经典的来回：接管后全轴同时从起点走到 +这么多度（如 30），"
                         "再走回起点，如此往复——只在正方向这一侧，与经典台架测试同形。"
                         "每趟的秒数由点动速度定，见 --selftest-traverse")
    ap.add_argument("--selftest-traverse", type=float, default=3.0, metavar="秒",
                    help="来回每趟想走多少秒（默认 3，与经典台架测试一致；"
                         "实际不会快过 度数 ÷ 点动速度）")
    return ap


def install_excepthook():
    """让界面里没接住的异常**留下一行遗言**，而不是把进程直接抹掉。

    PyQt5 里跑在虚函数（paintEvent、信号槽）里的 Python 异常不会往上抛给人看：PyQt
    把控制权交给 sys.excepthook，之后把它当致命错误处理——**默认结果是进程直接没**，
    没有 traceback、没有退出码可读。本次开发就撞上了：paintEvent 里一个解包写错，
    表现为"跑起来什么都没有、退出码 127"，查了很久才定位到那一行。

    台架上这个长相更糟：窗口开着开着不见了，日志里一个字都没有。所以这里把
    excepthook 换掉——按下 traceback 全部打出来，然后明确地以 3 退出（3 是"界面自己
    崩了"，好让台架脚本和别的失败区分开）。

    这不是把错误藏起来，是把错误**从无声改成有声**。
    """
    def hook(exc_type, value, tb):
        print("---- 界面内部异常 ----", file=sys.stderr, flush=True)
        traceback.print_exception(exc_type, value, tb, file=sys.stderr)
        print("---- 界面内部异常结束 ----", file=sys.stderr, flush=True)
        os._exit(3)
    sys.excepthook = hook


def main(argv=None):
    install_excepthook()
    ap = build_parser()
    args = ap.parse_args(argv)
    # resolve_endpoints 出错时要 parser.error()，把 parser 挂到 args 上带进去。
    args._parser = ap

    app = QApplication(sys.argv[:1])
    repo_root = poller.repo_root_of(__file__)

    try:
        window = MainWindow(args, repo_root)
    except emaster_endpoint.EndpointError as exc:
        print(f"端点写错了：{exc}", file=sys.stderr)
        return 2

    window.show()

    if args.selftest is not None:
        # 无头自检：到点自己退出，退出前把看到的关键量打出来。台架上就靠这段
        # 在 offscreen 下跑完整条路，不需要任何显示器。
        if args.selftest_recip is not None:
            _schedule_selftest_reciprocate(window, args.selftest, args.selftest_recip,
                                           args.selftest_traverse)
        elif args.selftest_takeover:
            _schedule_selftest_drive(window, args.selftest)
        QTimer.singleShot(int(args.selftest * 1000), _finish_selftest(window, app))

    code = app.exec_()
    # 自检是无头的：没人关窗口，closeEvent 不会跑，得在这儿补一次收尾。
    window.shutdown()
    return code


def _finish_selftest(window, app):
    def finish():
        window._report_selftest()
        app.quit()
    return finish


def _schedule_selftest_drive(window, seconds):
    """自检时走一遍动作，节奏按总时长摊开。

    只做**小幅度**的东西：接管 → 点动一点 → 回起点 → 急停 → 复位 → 释放。幅度刻意
    压得很小（0.2° 量级），因为这段代码在台架上也会跑，而台架上的轴是带着真实负载的。
    大步往返留给人在场的验证环节——那一段在 _schedule_selftest_reciprocate 里。
    """
    span = max(1.0, seconds)
    plan = [
        (0.10, "takeover", None),
        (0.25, "nudge", 0.2),
        (0.40, "nudge", -0.2),
        (0.55, "home", None),
        (0.70, "halt", None),
        (0.80, "resume", None),
        (0.92, "release", None),
    ]
    for fraction, action, amount in plan:
        delay = int(span * fraction * 1000)
        QTimer.singleShot(delay, _selftest_step(window, action, amount))


def _selftest_step(window, action, amount):
    def step():
        if action == "takeover":
            window._takeover()
        elif action == "release":
            window._release()
        elif action == "nudge":
            window._request("nudge", 0, amount)
        else:
            window._request(action)
    return step


# 往返自检的四个时间量。都摆在这儿，好让台架脚本和离线回归知道这段的节奏从哪来。
RECIP_LEAD_S = 1.0          # 起来之后先等这么久再点接管
RECIP_SETTLE_S = 0.6        # 每趟走完留的余量：等它停稳、等观测帧跟上
RECIP_TAIL_S = 8.0          # 收尾预算：最后一趟 + 回起点 + 释放
RECIP_ATTACH_WAIT_S = 5.0   # 接管后用这么久等命令通道真的接上
RECIP_POLL_MS = 200         # 上面那个等待的轮询间隔


def _selftest_axes(window):
    """接上了就把轴号报出来，没接上返回 None。

    判据用的是**快照里真的有轴**，不是 `window.session is not None`：会话对象是点接管
    那一刻就建出来的，那时候引擎还没跟主站说上话，轴表是空的，这时候发出去的目标会被
    丢掉（poller 会记一句"还没接上主站"）。
    """
    snapshot = window.last_snapshot
    if snapshot is None:
        return None
    count = int(snapshot.get("axis_count") or 0)
    return list(range(count)) if count > 0 else None


def _selftest_positions(window):
    """一趟走完，各轴**实际**在哪儿。

    只有观测帧里的实际位置能证明"轴动了"，本地目标、已提交目标都是这一侧自己的账。
    一帧没收到就如实说是"无从判断"，不拿本地账顶上。
    """
    snapshot = window.last_snapshot
    if snapshot is None or not snapshot["axes"]:
        return "实到？（一帧观测都没收到，无从判断）"
    return "实到 " + " ".join(f"轴{a['index'] + 1} {a['pos_deg']:+.2f}°"
                              for a in snapshot["axes"])


def _schedule_selftest_reciprocate(window, seconds, degrees, traverse_s):
    """自检：全轴同时走经典的来回（默认 30°，每趟 3 秒），按自检总时长摊开。

    **波形跟经典那条一致：起点 → +A → 起点 → +A**（scripts/test_external_motion_client.py
    的 triangle，周期 2×单程，全程匀速）。不是 ±A 来回摆：经典那条只在正方向一侧走，
    每一趟都回到起点，所以这里也是"去、回起点、去、回起点"。

    **为什么要这一段。** 台架上经典那条往返是**另一个客户端**发的
    （scripts/bench_reciprocate_30deg.sh）。这一轮要证的是"界面发的命令也能把轴走到位"，
    所以复用界面上本来就有的那个动作（相对启动位置的多轴移动），不加新协议、不改控制律，
    只加一段节奏。

    **节奏为什么由点动速度定，不是由 --traverse 定。** 引擎每拍最多走
    点动速度 ÷ 50 度，所以 度数 ÷ 点动速度 秒是一趟的**下限**：默认 10°/s 走 30°
    正好 3 秒，与经典的 --traverse 3 对得上。要得比这个快，跑出来还是这个——这里明说，
    不闷着。

    **每趟的实到位置都记一笔。** "界面发了命令"和"轴真的动了"是两件事：面板上的目标、
    引擎的本地账都证明不了后者，只有观测帧里的实际位置能。所以每趟开下一趟之前先把上一趟
    的实到位置打出来；接不上、走不满、时间不够，都照样打，不静默跳过。

    **趟数取双数。** 双数趟意味着最后一趟正好把轴送回起点，收尾时轴是停在家里的；
    再补一条"回起点"只是保险（落空时它什么都不做）。
    """
    span = max(2.0, float(seconds))
    jog_speed = max(0.1, float(window.args.jog_speed))
    kinematic_s = abs(degrees) / jog_speed
    requested_s = max(0.2, float(traverse_s))
    leg_s = max(requested_s, kinematic_s) + RECIP_SETTLE_S

    budget = span - RECIP_LEAD_S - RECIP_TAIL_S
    slots = max(0, int(budget // leg_s))
    if slots % 2:
        slots -= 1

    def say(text):
        window._log(text)
        # 同时进 stdout：台架脚本和离线回归都从这里捞这一段的证据。
        print(text, flush=True)

    if slots < 2:
        say(f"往返：自检 {span:g} 秒放不下一个来回（每趟 {leg_s:.1f} 秒），"
            f"这一轮只接管不发目标")
    if kinematic_s > requested_s:
        say(f"往返：要的每趟 {requested_s:g} 秒比点动速度给得起的 {kinematic_s:.1f} 秒"
            f"（{abs(degrees):g}° ÷ {jog_speed:g}°/s）快，实际按后者走")
    say(f"往返：{slots} 趟，每趟 {leg_s:.1f} 秒，幅度 +{abs(degrees):g}°（每趟回起点），"
        f"全轴同时——与经典台架测试一个形状")

    state = {"leg": 0, "prev": None, "waited": 0.0, "done": False}

    def flush():
        if state["prev"] is None:
            return
        leg, target = state["prev"]
        state["prev"] = None
        say(f"往返：第 {leg} 趟 目标 {target:+.1f}° {_selftest_positions(window)}")

    def wrap_up():
        """收尾：回起点 → 释放。

        **释放不是礼貌。** 命令口只有一格 accept，占着不放，台架脚本就再也连不上了
        （表现为挂住，不报错）。
        """
        state["done"] = True
        flush()
        window._request("home")
        QTimer.singleShot(1000, window._release)

    def tick():
        if state["done"]:
            return
        flush()
        if state["leg"] >= slots:
            wrap_up()
            return
        axes = _selftest_axes(window)
        if axes is None:
            if state["waited"] >= RECIP_ATTACH_WAIT_S:
                say(f"往返：接管后 {RECIP_ATTACH_WAIT_S:g} 秒还没接上命令通道，来回不走了")
                wrap_up()
                return
            state["waited"] += RECIP_POLL_MS / 1000.0
            QTimer.singleShot(RECIP_POLL_MS, tick)
            return
        # 偶数趟去 +A，奇数趟回起点——每趟都从这一头走到那一头，全程匀速。
        target = abs(float(degrees)) if state["leg"] % 2 == 0 else 0.0
        window._request("move", [(index, target) for index in axes])
        state["prev"] = (state["leg"] + 1, target)
        state["leg"] += 1
        QTimer.singleShot(int(leg_s * 1000), tick)

    def begin():
        window._takeover()
        QTimer.singleShot(int(RECIP_LEAD_S * 1000), tick)

    QTimer.singleShot(0, begin)


if __name__ == "__main__":
    sys.exit(main())
