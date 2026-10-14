#!/usr/bin/env python3
"""Orange Pi 远程操作工具 - 通用 SSH 工具，支持密码认证"""
import paramiko
import os
import sys
import argparse

# Windows 控制台默认 GBK，远端输出里的非 GBK 字节会在 print 时抛 UnicodeEncodeError，
# 连命令结果都看不到。统一按 UTF-8 出不认识的字节用替代字符。
for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8", errors="replace")

# 台架地址会随网络环境变化（ICS 共享网络 / 独立路由器给的不是同一个网段），
# 写成环境变量可覆盖，免得每次改代码。
HOST = os.environ.get("EMASTER_PI_HOST", "192.168.124.81")
USER = os.environ.get("EMASTER_PI_USER", "orangepi")
PASSWORD = os.environ.get("EMASTER_PI_PASSWORD", "orangepi")


def connect():
    """建立 SSH 连接"""
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=PASSWORD, timeout=10)
    return client


def run_command(client, command, use_pty=False):
    """执行命令并返回输出"""
    stdin, stdout, stderr = client.exec_command(command, get_pty=use_pty)
    output = stdout.read().decode('utf-8', errors='replace')
    error = stderr.read().decode('utf-8', errors='replace')
    return output, error, stdout.channel.recv_exit_status()


def upload_file(client, local_path, remote_path):
    """上传文件。

    远程路径要写成相对路径（相对登录后的家目录）。这台机器上的 sftp-server 对
    "/tmp/xxx" 这类绝对路径一律回 ENOENT —— 报的是"没有那个文件"，看着像本地文件
    不存在，其实是远端不收绝对路径。写成 "xxx" 或 "ethercat-master/src/xxx" 即可。
    """
    with open(local_path, "rb") as source:
        data = source.read()
    sftp = client.open_sftp()
    with sftp.open(remote_path, "wb") as target:
        target.write(data)
    sftp.close()


def download_file(client, remote_path, local_path):
    """下载文件，远程路径同样要写相对路径（见 upload_file）。"""
    sftp = client.open_sftp()
    with sftp.open(remote_path, "rb") as source:
        data = source.read()
    sftp.close()
    with open(local_path, "wb") as target:
        target.write(data)


def main():
    parser = argparse.ArgumentParser(description="Orange Pi 远程操作工具")
    subparsers = parser.add_subparsers(dest='command', help='命令')

    # run 子命令
    run_parser = subparsers.add_parser('run', help='执行命令')
    run_parser.add_argument('cmd', help='要执行的命令')
    run_parser.add_argument('--sudo', action='store_true', help='使用 sudo')
    run_parser.add_argument('--pty', action='store_true', help='使用伪终端')

    # upload 子命令
    upload_parser = subparsers.add_parser('upload', help='上传文件')
    upload_parser.add_argument('local', help='本地文件路径')
    upload_parser.add_argument('remote', help='远程文件路径')

    # download 子命令
    download_parser = subparsers.add_parser('download', help='下载文件')
    download_parser.add_argument('remote', help='远程文件路径')
    download_parser.add_argument('local', help='本地文件路径')

    # shell 子命令
    shell_parser = subparsers.add_parser('shell', help='交互式 shell')

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    try:
        client = connect()

        if args.command == 'run':
            cmd = args.cmd
            if args.sudo:
                cmd = f"sudo {cmd}"
            output, error, exit_code = run_command(client, cmd, use_pty=args.pty)
            print(output, end='')
            if error:
                print(error, end='', file=sys.stderr)
            sys.exit(exit_code)

        elif args.command == 'upload':
            upload_file(client, args.local, args.remote)
            print(f"已上传: {args.local} -> {args.remote}")

        elif args.command == 'download':
            download_file(client, args.remote, args.local)
            print(f"已下载: {args.remote} -> {args.local}")

        elif args.command == 'shell':
            import pty
            import os
            import select

            channel = client.invoke_shell()

            def resize():
                tty_height, tty_width = os.get_terminal_size()
                channel.resize_pty(width=tty_width, height=tty_height)

            resize()

            try:
                while True:
                    r, w, e = select.select([channel, sys.stdin], [], [])
                    if channel in r:
                        try:
                            data = channel.recv(1024)
                            if len(data) == 0:
                                break
                            sys.stdout.buffer.write(data)
                            sys.stdout.flush()
                        except:
                            break
                    if sys.stdin in r:
                        data = sys.stdin.buffer.read(1)
                        if len(data) == 0:
                            break
                        channel.send(data)
            except KeyboardInterrupt:
                pass

        client.close()

    except Exception as e:
        print(f"错误: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
