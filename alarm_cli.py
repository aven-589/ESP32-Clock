#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Send one ALARM command to the running PC clock control port.

@版本 1.1.0
@日期 2026-11-09

口语化 Examples:
    alarm 每天7:30
    alarm "每天7:30 鸟叫 音量80 响20秒"
    alarm 查看闹钟
    alarm 关闭3号
    alarm 删除3号
    alarm 试听3号

高级原始命令也保留:
    alarm "ALARM LIST"
    alarm "ALARM ADD 07:30 daily chime 70 15"
    alarm "ALARM TUNE 0 C5:180,E5:180,G5:360"
"""

from __future__ import annotations

import argparse
import re
import socket
import sys


def _configure_windows_console() -> None:
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleOutputCP(65001)
        ctypes.windll.kernel32.SetConsoleCP(65001)
    except Exception:
        pass
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


_configure_windows_console()

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765

HELP_EXAMPLES = """常用说法（不用背 ALARM 参数）:
  每天7:30
  每天7点半，鸟叫，音量80，响20秒
  晚上8点，风铃
  明天早上7点响一次
  查看闹钟
  关闭3号
  打开3号
  删除3号
  清空闹钟
  试听3号
  试听鸟叫，音量70，响5秒
  停止响铃

高级原始命令:
  ALARM LIST
  ALARM ADD 07:30 daily chime 70 15
  ALARM ENABLE 0 0
  ALARM DEL 0
  ALARM TUNE 0 C5:180,E5:180,G5:360

说明:
  - 不说“每天/一次”时默认是每天。
  - “一次”不绑定具体日期，会在下一个该时刻响，响完自动删除。
  - 自定义音符仍用 ALARM TUNE，例如：
    ALARM TUNE 0 C5:180,E5:180,G5:360,0:120
"""

TIME_RE = re.compile(r"(?<!\d)(\d{1,2})\s*:\s*(\d{1,2})(?!\d)")
NOTE_RE = re.compile(r"(?:[A-Ga-g][#b]?\d|0)\s*:\s*\d+(?:\s*,\s*(?:[A-Ga-g][#b]?\d|0)\s*:\s*\d+)*")
VOLUME_RE = re.compile(r"(?:音量|响度|声音大小)\s*[:：]?\s*(\d{1,3})")
VOLUME_FALLBACK_RE = re.compile(r"声音\s*(\d{1,3})")
VOLUME_EN_RE = re.compile(r"(?:volume|vol)\s*[:：]?\s*(\d{1,3})", re.IGNORECASE)
DURATION_RE = re.compile(r"(?:响铃|响|持续|时长|播放)\s*(\d{1,3})\s*秒")
DURATION_FALLBACK_RE = re.compile(r"(\d{1,3})\s*秒")
DURATION_EN_RE = re.compile(r"(\d{1,3})\s*(?:s|sec|secs|seconds)\b", re.IGNORECASE)
SLOT_WORD_RE = re.compile(r"(?:闹钟|闹铃|第)?\s*([0-9零一二三四五六七八])\s*号")
SLOT_NUMBER_RE = re.compile(r"(?:闹钟|闹铃)\s*(\d{1,2})")
NUMBER_RE = re.compile(r"(?<!\d)(\d{1,2})(?!\d)")

SOUND_ALIASES = (
    ("bird", ("鸟叫", "小鸟", "鸟鸣", "鸟", "bird")),
    ("chime", ("风铃", "铃声", "叮咚", "钟声", "清脆", "chime")),
    ("beep", ("滴滴", "哔哔", "蜂鸣", "短促", "哔", "beep")),
    ("alarm", ("警报", "闹铃", "急促", "alarm")),
    ("custom", ("自定义", "custom")),
)

CN_DIGITS = {
    "零": 0, "一": 1, "二": 2, "三": 3, "四": 4,
    "五": 5, "六": 6, "七": 7, "八": 8,
}


class FriendlyError(ValueError):
    pass


class HelpRequested(Exception):
    pass


def normalize(text: str) -> str:
    """Turn common Chinese time spellings into HH:MM."""
    text = text.strip()
    text = text.translate(str.maketrans("０１２３４５６７８９：　", "0123456789: "))
    text = re.sub(r"\s+", " ", text)
    # Only turn the time particle into a colon. Replacing every “点” would
    # corrupt phrases like “声音小一点” / “响久一点”.
    text = re.sub(r"(\d{1,2})\s*点半", r"\1:30", text)
    text = re.sub(r"(\d{1,2})\s*点", r"\1:", text)
    text = re.sub(r"(\d{1,2})\s*时", r"\1:", text)
    text = re.sub(r"(\d)\s*分", r"\1", text)
    text = re.sub(r"(\d{1,2})\s*:\s*(\d{1,2})", r"\1:\2", text)
    text = re.sub(r"(\d{1,2})\s*:\s*(?!\d)", r"\1:00", text)
    return text.strip()


def normalized_slot(text: str) -> int | None:
    m = SLOT_WORD_RE.search(text)
    if m:
        token = m.group(1)
        if token.isdigit():
            value = int(token)
        else:
            value = CN_DIGITS[token]
        if 0 <= value < 8:
            return value
    m = SLOT_NUMBER_RE.search(text)
    if m:
        value = int(m.group(1))
        if 0 <= value < 8:
            return value
    numbers = NUMBER_RE.findall(text)
    if len(numbers) == 1:
        value = int(numbers[0])
        if 0 <= value < 8:
            return value
    return None


def find_sound(text: str) -> str | None:
    for sound, words in SOUND_ALIASES:
        if any(word in text for word in words):
            return sound
    return None


def find_volume(text: str) -> int | None:
    m = VOLUME_RE.search(text) or VOLUME_FALLBACK_RE.search(text) or VOLUME_EN_RE.search(text)
    if m:
        value = int(m.group(1))
        if not 0 <= value <= 100:
            raise FriendlyError("音量要在 0 到 100 之间。")
        return value
    if re.search(r"最大声|最大|大声", text):
        return 90
    if re.search(r"小声|小一点|轻一点|小点", text):
        return 40
    if re.search(r"中等|正常", text):
        return 70
    return None


def find_duration(text: str) -> int | None:
    m = DURATION_RE.search(text) or DURATION_FALLBACK_RE.search(text) or DURATION_EN_RE.search(text)
    if m:
        value = int(m.group(1))
        if not 1 <= value <= 300:
            raise FriendlyError("响铃时长要在 1 到 300 秒之间。")
        return value
    if re.search(r"久一点|长一点|久些", text):
        return 30
    if re.search(r"短一点|短些", text):
        return 5
    return None


def find_tail_values(text: str, sound: str | None) -> tuple[int | None, int | None]:
    if not sound:
        return None, None
    m = re.search(rf"\b{re.escape(sound)}\b(.*)$", text, re.IGNORECASE)
    if not m:
        return None, None
    values = [int(n) for n in re.findall(r"(?<!\d)(\d{1,3})(?!\d)", m.group(1))]
    if len(values) >= 2:
        return values[0], values[1]
    if len(values) == 1:
        return values[0], None
    return None, None


def apply_period(hour: int, text: str) -> int:
    if re.search(r"下午|晚上|傍晚|夜里", text) and hour < 12:
        hour += 12
    if re.search(r"凌晨", text) and hour == 12:
        hour = 0
    return hour


def find_time(text: str) -> tuple[int, int] | None:
    m = TIME_RE.search(text)
    if not m:
        return None
    hour = apply_period(int(m.group(1)), text)
    minute = int(m.group(2))
    if hour > 23 or minute > 59:
        raise FriendlyError("时间不对，请用 07:30、19:00 这种格式。")
    return hour, minute


def build_add(text: str) -> str:
    time_value = find_time(text)
    if time_value is None:
        raise FriendlyError("没找到时间。可以说：每天7:30，或者 明天早上7点响一次。")
    hour, minute = time_value
    mode = "daily"
    if re.search(r"一次|单次|只响一次|once", text):
        mode = "once"
    elif re.search(r"每天|每日|天天|daily", text):
        mode = "daily"
    elif re.search(r"明天|后天|今天", text):
        mode = "once"

    sound = find_sound(text) or "chime"
    if sound == "custom":
        raise FriendlyError("自定义声音请先用原始命令设置，例如：ALARM TUNE 0 C5:180,E5:180,G5:360")
    volume = find_volume(text)
    duration = find_duration(text)
    if volume is None or duration is None:
        tail_volume, tail_duration = find_tail_values(text, sound)
        if volume is None:
            volume = tail_volume
        if duration is None:
            duration = tail_duration
    return f"ALARM ADD {hour:02d}:{minute:02d} {mode} {sound} {volume if volume is not None else 70} {duration if duration is not None else 15}"


def build_test(text: str) -> str:
    slot = normalized_slot(text)
    if slot is not None:
        return f"ALARM TEST {slot}"
    sound = find_sound(text) or "chime"
    volume = find_volume(text)
    duration = find_duration(text)
    if volume is None or duration is None:
        tail_volume, tail_duration = find_tail_values(text, sound)
        if volume is None:
            volume = tail_volume
        if duration is None:
            duration = tail_duration
    return f"ALARM TEST {sound} {volume if volume is not None else 70} {duration if duration is not None else 5}"


def friendly_to_alarm(command: str) -> str:
    raw = command.strip()
    if not raw:
        raise HelpRequested()

    # Keep an explicitly uppercase raw protocol command, for example:
    #   ALARM ADD 07:30 daily chime 70 15
    # Accept the convenience form typed in the console:
    #   alarm add 1:00 once
    # by removing the leading "alarm" word and parsing the rest normally.
    if raw.startswith("ALARM "):
        return raw
    if raw.lower().startswith("alarm "):
        raw = raw[6:].strip()

    text = normalize(raw)
    lower = text.lower()

    note_match = NOTE_RE.search(text)
    if note_match and re.search(r"[A-Ga-g]\d", note_match.group(0)):
        slot = normalized_slot(text)
        if slot is None:
            raise FriendlyError("自定义声音要指定编号，例如：3号自定义 C5:180,E5:180,G5:360")
        pattern = note_match.group(0).replace(" ", "")
        return f"ALARM TUNE {slot} {pattern}"

    if re.search(r"帮助|怎么用|用法|help", lower):
        raise HelpRequested()
    if re.search(r"查看|列出|列表|清单|看看|有哪些|有什么|查询|list|show", lower):
        return "ALARM LIST"
    if re.search(r"清空|清除|全部删|全部删除|删除所有|delete all|clear", lower):
        return "ALARM CLEAR"
    if re.search(r"停止|别响|不要响|关闭声音|关掉声音|关闭响铃|关掉响铃|闭嘴|stop", lower):
        return "ALARM STOP"
    if re.search(r"试听|测试|听听|test", lower):
        return build_test(text)

    slot = normalized_slot(text)
    if re.search(r"删除|删掉|移除|去掉|del|delete|remove", lower):
        if slot is None:
            raise FriendlyError("删除几号？例如：删除3号，或 delete alarm3。")
        return f"ALARM DEL {slot}"
    if re.search(r"关闭|停用|禁用|关掉|暂停|\bclose\b|\boff\b|\bdisable\b", lower):
        if slot is None:
            raise FriendlyError("关闭几号？例如：关闭3号，或 close alarm3。")
        return f"ALARM ENABLE {slot} 0"
    if re.search(r"启用|恢复|打开|开启|\bopen\b|\bon\b|\benable\b", lower):
        if slot is None:
            raise FriendlyError("打开几号？例如：打开3号，或 open alarm3。")
        return f"ALARM ENABLE {slot} 1"

    if find_time(text) is not None or re.search(r"添加|新增|设置|add|set|new", lower):
        return build_add(text)

    raise FriendlyError("没听懂。可以说：每天7:30、查看闹钟、关闭3号、删除3号、试听3号。")


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="alarm",
        description="ESP32 PC Clock 闹钟命令客户端（支持口语化说法）",
        epilog=HELP_EXAMPLES,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("command", nargs=argparse.REMAINDER, help="例如：每天7:30 / 查看闹钟 / ALARM LIST")
    args = parser.parse_args()

    command = " ".join(args.command).strip()
    if not command:
        parser.print_help()
        return 2

    try:
        translated = friendly_to_alarm(command)
    except HelpRequested:
        print(HELP_EXAMPLES)
        return 0
    except FriendlyError as exc:
        print(f"[ERR] {exc}")
        print()
        print(HELP_EXAMPLES)
        return 2

    try:
        with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
            sock.settimeout(args.timeout)
            sock.sendall((translated + "\n").encode("utf-8"))
            chunks: list[bytes] = []
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                chunks.append(data)
    except ConnectionRefusedError:
        print("[ERR] 控制端口没有打开。先运行 start_clock.cmd，保持它运行，再执行本命令。")
        return 1
    except Exception as exc:
        print(f"[ERR] 连接 {args.host}:{args.port} 失败: {exc}")
        return 1

    text = b"".join(chunks).decode("utf-8", "replace").strip()
    if translated != command:
        print(f"[PC] 已理解：{translated}")
    print(text if text else "[ERR] 没有收到 ESP32 回复")
    return 0 if text and not text.startswith("ERR") else 1


if __name__ == "__main__":
    sys.exit(main())
