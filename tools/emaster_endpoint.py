#!/usr/bin/env python3
"""端点：把"怎么连主站"从各客户端里抽出来，收成一份。

**为什么要有这一层。** 主站的两个套接字是 AF_UNIX，路径写死在唯一一处构造点
（include/emaster/bus/command_socket_path.h）。这对同机客户端没问题，对别的机器
就是死路：路径名到不了另一台机器。要在 Windows 上跑界面、同时主站在香橙派上，
中间必须有一条转发（tools/socket_bridge.py）。于是"连哪儿"这件事就分成了两类：

    unix:/tmp/emaster-<id>.sock      同机，直连
    tcp:<host>:<port>                跨机，经桥

此前每个客户端各自把 `socket.AF_UNIX` 写死在自己的 connect 里，加 TCP 就得逐个
改、逐个漏。这里收成一份：客户端只认 Endpoint，不再自己拼路径、也不再自己挑
地址族。

**路径规则仍是两处。** `deployment_sockets()` 与 command_socket_path.h 说的是同一
条规则，但一个在 C 构建里、一个在 Python 里，共享不了那个头文件。改命名时两处
一起改——这一条是照着 closed_loop_client.py 原来的注释留的，不是新问题。

**端口约定**：桥的命令口与观测口是连着的两个端口，`--tcp host:5001` 意味着命令在
5001、观测在 5002。要单独指定观测口用 `--obs-tcp`。

**本机（Windows）没有 AF_UNIX**（本机 Python 是 MSC 构建，`socket` 里根本没有这个
常数）。所以 unix 端点在这里报的是一句能读懂的话，而不是 AttributeError。
"""

import argparse
import os
import socket

# 与 include/emaster/bus/command_socket_path.h 的 EMASTER_DEPLOYMENT_ENV 同名同义。
DEPLOYMENT_ENV = "EMASTER_DEPLOYMENT"

# 命令口与观测口在桥上的默认端口，以及两者的固定间隔。观测口 = 命令口 + 这个偏移，
# 所以客户端只要给一个端口就够；要让两处分开部署再用 --obs-tcp 覆盖。
DEFAULT_TCP_COMMAND_PORT = 5001
OBSERVATION_PORT_OFFSET = 1

UNIX_PREFIX = "unix:"
TCP_PREFIX = "tcp:"


def deployment_sockets(deployment_id):
    """按主站的命名规则由部署 ID 推出命令与观测两个路径。

    格式串与 include/emaster/bus/command_socket_path.h 是同一套：命令
    /tmp/emaster-<id>.sock，观测 /tmp/emaster-<id>-obs.sock。两处各写一份是
    有意的——它们分属 C 与 Python 两个构建，共享不了那个头文件；但规则是同一
    条，改命名时两处一起改。
    """
    return (f"/tmp/emaster-{deployment_id}.sock",
            f"/tmp/emaster-{deployment_id}-obs.sock")


class EndpointError(Exception):
    """端点写错了。是可读的用法错误，不是运行期故障。"""


def _split_host_port(text):
    """把 `host:port`、`:port`、`port` 都解成 (host, port)。host 缺省为本机。

    三种写法是同一个意思，不该有一个悄悄不认：`tcp:5001` 与 `tcp::5001` 都是
    本机的 5001。只给一个端口号最容易写出来，所以它必须能用。
    """
    host, sep, port_text = text.rpartition(":")
    if not sep:
        host, port_text = "", text
    try:
        port = int(port_text)
    except ValueError:
        raise EndpointError(
            f"端口不是数字：{port_text!r}（要写成 host:port，或只给一个端口）") from None
    if not 0 < port < 65536:
        raise EndpointError(f"端口超出范围：{port}")
    return (host or "127.0.0.1"), port


class Endpoint:
    """一个连法。自己不知道协议，只负责产出一个连好的 socket。"""

    def __init__(self, kind, target):
        self.kind = kind
        self.target = target

    @classmethod
    def unix(cls, path):
        return cls("unix", path)

    @classmethod
    def tcp(cls, host, port):
        return cls("tcp", (host, int(port)))

    @classmethod
    def parse(cls, spec):
        """解析一个端点的字面写法。

        认三种：`unix:/path`、`tcp:host:port`，以及裸的 `/path`（当成 unix）。
        裸路径这一条是为了兼容——此前的客户端、脚本、文档里到处都是直接给
        路径，它们不该因为这一层而失效。
        """
        if isinstance(spec, Endpoint):
            return spec
        if not isinstance(spec, str) or not spec:
            raise EndpointError(f"端点不能为空（收到 {spec!r}）")

        if spec.startswith(UNIX_PREFIX):
            path = spec[len(UNIX_PREFIX):]
            if not path:
                raise EndpointError("unix: 后面要跟套接字路径")
            return cls.unix(path)

        if spec.startswith(TCP_PREFIX):
            host, port = _split_host_port(spec[len(TCP_PREFIX):])
            return cls.tcp(host, port)

        if spec.startswith("/"):
            return cls.unix(spec)

        raise EndpointError(
            f"认不出的端点 {spec!r}；要给 /path、unix:/path 或 tcp:host:port")

    @property
    def display(self):
        """给人看的写法。unix 端点就是那条路径，与加这一层之前一模一样。"""
        if self.kind == "unix":
            return self.target
        host, port = self.target
        return f"tcp:{host}:{port}"

    @property
    def host(self):
        return self.target[0] if self.kind == "tcp" else None

    @property
    def port(self):
        return self.target[1] if self.kind == "tcp" else None

    @property
    def path(self):
        return self.target if self.kind == "unix" else None

    def observation(self):
        """同一部署的观测端点。只有 tcp 端点能这样推——unix 那边要部署 ID。"""
        if self.kind != "tcp":
            raise EndpointError(
                "unix 端点推不出观测端点（命名规则要部署 ID，见 deployment_sockets）")
        host, port = self.target
        return Endpoint.tcp(host, port + OBSERVATION_PORT_OFFSET)

    def connect(self, timeout=5.0):
        """连上并返回 socket。失败抛 OSError，与原来的 connect 一致。"""
        if self.kind == "unix":
            if not hasattr(socket, "AF_UNIX"):
                raise EndpointError(
                    f"本机 Python 没有 AF_UNIX，连不了 {self.target}。"
                    f"跨机器请用 tcp:<主站IP>:<端口>（香橙派上先起 tools/socket_bridge.py）")
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.settimeout(timeout)
                sock.connect(self.target)
            except OSError:
                sock.close()
                raise
            return sock

        # create_connection 会做地址解析并自己设好超时。
        return socket.create_connection(self.target, timeout)

    def exists(self):
        """同机 unix 端点的套接字文件在不在。tcp 端点答不了这个问题——返回 True，
        免得调用方把"没法检查"读成"不存在"。"""
        if self.kind == "unix":
            return os.path.exists(self.target)
        return True

    def __str__(self):
        return self.display

    def __repr__(self):
        return f"Endpoint({self.display!r})"

    def __eq__(self, other):
        if not isinstance(other, Endpoint):
            return NotImplemented
        return (self.kind, self.target) == (other.kind, other.target)

    def __hash__(self):
        return hash((self.kind, str(self.target)))


def add_endpoint_arguments(parser, command=True, observation=False):
    """把端点相关的选项挂到一个 argparse 上。

    优先级与 C 侧 CLI 工具（emaster_cli_resolve_socket）保持一致：
    显式端点 > --deployment / $EMASTER_DEPLOYMENT。没有可用信息时报错退出，
    **不猜默认部署**——连错主站却看不出原因，是这条规矩当初立起来的原因。
    """
    parser.add_argument(
        "--deployment", default=None,
        help=f"部署 ID（与主站 --deployment 同一个）；也可用 ${DEPLOYMENT_ENV}")
    if command:
        parser.add_argument(
            "--cmd-endpoint", "--socket", dest="cmd_endpoint", default=None,
            help="命令通道端点：/path、unix:/path 或 tcp:host:port")
    if observation:
        parser.add_argument(
            "--obs-endpoint", "--obs-socket", dest="obs_endpoint", default=None,
            help="观测通道端点：/path、unix:/path 或 tcp:host:port")
    parser.add_argument(
        "--tcp", default=None, metavar="HOST:PORT",
        help=f"跨机器：命令口在这个 HOST:PORT，观测口在 PORT+{OBSERVATION_PORT_OFFSET}；"
             f"HOST 可省（写 :{DEFAULT_TCP_COMMAND_PORT}）")
    return parser


def _parse_tcp_option(text):
    host, port = _split_host_port(text)
    return Endpoint.tcp(host, port)


def resolve_endpoints(args, parser, need=("command", "observation")):
    """填上 args 里缺的端点。返回 {kind: Endpoint}。

    `need` 说明调用方要哪几条通道。只要观测的（比如 GUI 的只读模式）就传
    ("observation",)，这样命令端点缺失不算错。
    """
    deployment = getattr(args, "deployment", None) or os.environ.get(DEPLOYMENT_ENV)

    explicit = {}
    if "command" in need:
        raw = getattr(args, "cmd_endpoint", None)
        if raw:
            explicit["command"] = Endpoint.parse(raw)
    if "observation" in need:
        raw = getattr(args, "obs_endpoint", None)
        if raw:
            explicit["observation"] = Endpoint.parse(raw)

    tcp_option = getattr(args, "tcp", None)
    if tcp_option:
        base = _parse_tcp_option(tcp_option)
        if "command" in need and "command" not in explicit:
            explicit["command"] = base
        if "observation" in need and "observation" not in explicit:
            explicit["observation"] = base.observation()

    if deployment:
        cmd_path, obs_path = deployment_sockets(deployment)
        if "command" in need and "command" not in explicit:
            explicit["command"] = Endpoint.unix(cmd_path)
        if "observation" in need and "observation" not in explicit:
            explicit["observation"] = Endpoint.unix(obs_path)

    missing = [kind for kind in need if kind not in explicit]
    if missing:
        wanted = "、".join("命令" if kind == "command" else "观测" for kind in missing)
        parser.error(
            f"没有{wanted}通道的连法：给显式端点、--tcp HOST:PORT，"
            f"或 --deployment（或设 {DEPLOYMENT_ENV}）")
    return explicit


def connect(identifier, timeout=5.0):
    """便捷入口：接一个端点写法，返回连好的 socket。

    保留这个函数是为了让 observation_client 那类"只连一条通道"的小工具不必
    自己 new 一个 Endpoint。
    """
    return Endpoint.parse(identifier).connect(timeout)


if __name__ == "__main__":
    # 自查用：把命令行给的端点解析出来打给人看，便于在台架上确认自己会连哪儿。
    ap = argparse.ArgumentParser(description="端点解析自查")
    ap.add_argument("endpoint")
    parsed = Endpoint.parse(ap.parse_args().endpoint)
    print(f"kind   = {parsed.kind}")
    print(f"display= {parsed.display}")
    if parsed.kind == "tcp":
        print(f"观测口  = {parsed.observation().display}")
