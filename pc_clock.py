#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PC side clock sync for ESP32-S3 PC Clock.
@版本 1.2.0
@日期 2026-11-09

自动扫描串口 -> 握手 -> 首次发送电脑时间，之后每小时校准一次。
同时在 127.0.0.1:8765 提供闹钟控制端口，供 alarm_cli.py 使用。
ESP32 未同步前每秒广播 PC_CLOCK_HELLO，电脑优先被动监听 beacon，
找不到再主动 PING；收到首次 SYNC 后 ESP32 停止广播并自行走时。

协议：
    ESP -> PC : PC_CLOCK_HELLO 1\n（未同步前每秒）
    PC -> ESP : SYNC <UTC秒> <时区偏移分钟>\n
    ESP -> PC : OK <UTC秒> <时区偏移分钟>\n

依赖：
    pip install pyserial
"""

from __future__ import annotations

import argparse
import socket
import socketserver
import sys
import threading
import time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("缺少 pyserial，请先运行：pip install pyserial")
    raise SystemExit(1)

BAUD = 115200
DEFAULT_INTERVAL = 3600.0
SYNC_TIMEOUT = 4.0
CONTROL_PORT = 8765

serial_guard = threading.Lock()
active_serial = None

# Common USB-serial chips used by ESP32 boards.
KNOWN_USB = {
    (0x303A, None),       # Espressif native USB
    (0x10C4, 0xEA60),     # CP210x
    (0x1A86, 0x7523),     # CH340
    (0x0403, 0x6001),     # FTDI
}


def local_offset_minutes() -> int:
    """Current PC UTC offset in minutes (handles DST)."""
    offset = datetime.now().astimezone().utcoffset()
    if offset is None:
        return 0
    return int(offset.total_seconds() // 60)


def is_known_esp_port(port) -> bool:
    if port.vid is None:
        return False
    if (port.vid, None) in KNOWN_USB:
        return True
    return (port.vid, port.pid) in KNOWN_USB


def scan_ports(explicit: str | None) -> list[str]:
    if explicit:
        return [explicit]

    ports = list(list_ports.comports())
    known: list[str] = []
    maybe: list[str] = []

    for port in ports:
        desc = (port.description or "").lower()
        if "bluetooth" in desc and not is_known_esp_port(port):
            continue
        if is_known_esp_port(port):
            known.append(port.device)
        elif any(word in desc for word in ("usb", "uart", "serial", "cp210", "ch340", "espressif")):
            maybe.append(port.device)
        else:
            maybe.append(port.device)

    # Known ESP USB devices first; unknown ports are still probed so a board
    # with an unusual USB-serial chip can be found automatically.
    return known + [p for p in maybe if p not in known]


def list_serial_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("没有发现串口。")
        return
    for port in ports:
        vid_pid = f"{port.vid:04x}:{port.pid:04x}" if port.vid is not None and port.pid is not None else "-"
        print(f"{port.device:10s} {vid_pid:10s} {port.description or ''}")


def handshake(port: str, baud: int, verbose: bool) -> serial.Serial | None:
    print(f"[PC] 尝试串口 {port} ...")
    try:
        ser = serial.Serial(port=port, baudrate=baud, timeout=0.2, write_timeout=0.5)
    except Exception as exc:
        # Always show this: "拒绝访问" usually means another program (often the
        # Arduino serial monitor) still owns the port.
        print(f"[PC] {port} 打开失败: {exc}")
        return None

    try:
        # Avoid holding the ESP in reset/bootloader on boards that wire DTR/RTS.
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        time.sleep(0.25)
        ser.reset_input_buffer()

        # Passive discovery: an unsynced ESP32 broadcasts PC_CLOCK_HELLO once
        # per second, so just opening the port is enough to find it. This also
        # catches boards that reset when the serial port is opened.
        beacon_deadline = time.time() + 2.5
        while time.time() < beacon_deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if verbose:
                print(f"[ESP] {line}")
            if "PC_CLOCK_HELLO" in line or "PC_CLOCK_READY" in line:
                print(f"[PC] 收到 ESP beacon，已连接 {port}")
                return ser

        deadline = time.time() + 5.0
        while time.time() < deadline:
            try:
                ser.write(b"PING\n")
                ser.flush()
            except Exception:
                break

            end = time.time() + 0.6
            while time.time() < end:
                line = ser.readline().decode("utf-8", "replace").strip()
                if not line:
                    continue
                if verbose:
                    print(f"[ESP] {line}")
                if "PC_CLOCK" in line or line.startswith("PONG"):
                    print(f"[PC] 已连接 {port}")
                    return ser
            time.sleep(0.2)
    except Exception as exc:
        if verbose:
            print(f"[PC] 握手失败: {exc}")

    try:
        ser.close()
    except Exception:
        pass
    return None


def stream_time(ser: serial.Serial, port: str, interval: float, verbose: bool) -> None:
    last_ok = time.time()
    count = 0

    while True:
        started = time.time()
        offset = local_offset_minutes()
        command = f"SYNC {int(started)} {offset}\n"

        got_ok = False
        # Hold the serial lock for the short sync exchange. A control command
        # arriving at the same moment waits at most ~0.35 s.
        with serial_guard:
            try:
                ser.write(command.encode("ascii"))
                ser.flush()
            except Exception as exc:
                raise RuntimeError(f"串口写入失败: {exc}") from exc

            # Wait only for the immediate OK. The previous implementation kept
            # reading until 0.7 s and then slept another full interval, so the
            # effective sync period was ~1.7 s instead of 1 s.
            deadline = time.time() + min(0.35, max(0.2, interval * 0.35))
            while time.time() < deadline:
                try:
                    line = ser.readline().decode("utf-8", "replace").strip()
                except Exception as exc:
                    raise RuntimeError(f"串口读取失败: {exc}") from exc
                if not line:
                    continue
                if verbose or not line.startswith("OK"):
                    print(f"[ESP] {line}")
                if line.startswith("OK"):
                    got_ok = True
                    last_ok = time.time()
                    break

        if not got_ok and time.time() - last_ok > SYNC_TIMEOUT:
            raise TimeoutError("ESP32 已停止回复 OK")

        stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[PC] {port} 已同步 {stamp}  UTC{offset:+d} 分钟")

        count += 1
        elapsed = time.time() - started
        time.sleep(max(0.05, interval - elapsed))


class ControlHandler(socketserver.StreamRequestHandler):
    """One command per TCP connection, forwarded to the ESP32 alarm parser."""

    def handle(self) -> None:
        raw = self.rfile.readline(512)
        if not raw:
            return
        command = raw.decode("utf-8", "replace").strip()
        if not command:
            self.wfile.write(b"ERR empty command\n")
            return

        with serial_guard:
            ser = active_serial
            if ser is None or not ser.is_open:
                self.wfile.write(b"ERR PC_CLOCK not connected\n")
                return

            try:
                ser.write((command + "\n").encode("utf-8"))
                ser.flush()
            except Exception as exc:
                self.wfile.write(f"ERR serial write: {exc}\n".encode("utf-8"))
                return

            lines: list[str] = []
            deadline = time.time() + 3.0
            while time.time() < deadline:
                try:
                    line = ser.readline().decode("utf-8", "replace").strip()
                except Exception as exc:
                    lines.append(f"ERR serial read: {exc}")
                    break
                if not line:
                    continue
                lines.append(line)
                if line.startswith("OK") or line.startswith("ERR"):
                    break

        if not lines:
            lines.append("ERR PC_CLOCK timeout")
        self.wfile.write(("\n".join(lines) + "\n").encode("utf-8"))


class ControlServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def start_control_server(port: int) -> ControlServer | None:
    try:
        server = ControlServer(("127.0.0.1", port), ControlHandler)
    except Exception as exc:
        print(f"[PC] 控制端口 {port} 启动失败: {exc}")
        return None
    threading.Thread(target=server.serve_forever, daemon=True, name="alarm-control").start()
    print(f"[PC] 闹钟控制端口已启动: 127.0.0.1:{port}")
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32-S3 PC Clock 时间同步")
    parser.add_argument("--port", help="指定串口，例如 COM5；不填则自动扫描")
    parser.add_argument("--baud", type=int, default=BAUD, help="波特率，默认 115200")
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL, help="同步间隔秒，默认 3600（每小时）")
    parser.add_argument("--list", action="store_true", help="列出串口后退出")
    parser.add_argument("--verbose", action="store_true", help="打印 ESP32 返回的每一行")
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT, help="闹钟控制端口，默认 8765；0 表示关闭")
    args = parser.parse_args()

    if args.list:
        list_serial_ports()
        return 0

    global active_serial
    if args.control_port:
        start_control_server(args.control_port)

    if args.interval < 0.2:
        print("--interval 不能小于 0.2 秒")
        return 2

    print("[PC] 自动扫描串口；Ctrl+C 退出")
    try:
        while True:
            candidates = scan_ports(args.port)
            if not candidates:
                print("[PC] 没有发现串口，2 秒后重试 ...")
                time.sleep(2)
                continue

            connected = None
            for port in candidates:
                connected = handshake(port, args.baud, args.verbose)
                if connected:
                    break

            if not connected:
                print("[PC] 没有找到 ESP32 时钟，2 秒后重试 ...")
                print("[PC] 请检查：串口监视器是否占用端口；ESP32-S3 的 USB CDC On Boot 是否 Enabled；换一个 USB 口/数据线。")
                print("[PC] 需要原始串口日志时运行：start_clock.cmd --verbose")
                time.sleep(2)
                continue

            with serial_guard:
                active_serial = connected
            try:
                stream_time(connected, connected.port, args.interval, args.verbose)
            except KeyboardInterrupt:
                raise
            except Exception as exc:
                print(f"[PC] 连接中断: {exc}")
            finally:
                with serial_guard:
                    active_serial = None
                try:
                    connected.close()
                except Exception:
                    pass
            print("[PC] 重新扫描串口 ...")
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[PC] 已停止")
        return 0


if __name__ == "__main__":
    sys.exit(main())
