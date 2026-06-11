"""
Smart Bat 序列 -> UDP 橋接 (跨平台 + 彈性欄位解析)

讓 Python 讀 STM32 的序列輸出,把每次揮棒事件用 UDP 丟給 Godot。

解析方式:
    不再寫死欄位順序,而是把整行裡所有 key=value 都抓出來,
    所以韌體多加 / 少加欄位(例如 start_rt, peak_rt)都不會壞。

執行需求:
    pip install pyserial

用法:
    python swing_bridge.py
    (會自動掃描並選用 ST-LINK 的虛擬序列埠;找不到才用下面的 PORT 預設值)
"""

import json
import re
import socket
import time

import serial
import serial.tools.list_ports


# 自動偵測失敗時的後備埠號(Windows 例如 "COM3",Linux 例如 "/dev/ttyACM0")
PORT = "COM3"
BAUD = 115200

UDP_IP = "127.0.0.1"
UDP_PORT = 4242


# 抓出整行裡所有 key=整數 的配對(可含負號)
kv_re = re.compile(r"(\w+)=(-?\d+)")


def parse_fields(line: str) -> dict:
    """把 'a=1 b=2 c=-3' 解析成 {'a':1,'b':2,'c':-3}。"""
    return {k: int(v) for k, v in kv_re.findall(line)}


def find_port() -> str:
    candidates = list(serial.tools.list_ports.comports())
    for p in candidates:
        text = " ".join(filter(None, [p.description, p.manufacturer, p.product]))
        if any(k in text for k in ("STLink", "ST-Link", "STMicro", "STM32")):
            print(f"Auto-detected ST-LINK port: {p.device}  ({p.description})")
            return p.device
    print("找不到 ST-LINK,改用預設 PORT =", PORT)
    for p in candidates:
        print(f"  {p.device}  -  {p.description}")
    return PORT


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    port = find_port()

    with serial.Serial(port, BAUD, timeout=1) as ser:
        print(f"Reading {port} @ {BAUD}, forwarding to {UDP_IP}:{UDP_PORT} ...")
        time.sleep(2)

        stm32_start_t = None
        start_pc_time = None

        while True:
            raw = ser.readline()
            if not raw:
                continue

            line = raw.decode(errors="replace").strip()
            if not line:
                continue
            print(line)

            # ---- SWING_START ----
            if "SWING_START" in line:
                f = parse_fields(line)
                if "t" in f:
                    stm32_start_t = f["t"]
                    start_pc_time = time.monotonic()
                continue

            # ---- SWING_END ----
            if "SWING_END" in line:
                f = parse_fields(line)
                end_pc_time = time.monotonic()

                stm32_end_t = f.get("t", 0)
                duration = f.get("dur", 0)
                peak_t = f.get("peak_t", stm32_end_t)
                peak_dps = f.get("peak_dps", 0)
                speed = f.get("speed_x100", 0) / 100.0
                drop = f.get("drop", 0)

                # 若沒收到對應的 SWING_START,用 duration 回推
                if stm32_start_t is None or start_pc_time is None:
                    stm32_start_t = stm32_end_t - duration
                    start_pc_time = end_pc_time - duration / 1000.0

                peak_offset_ms = peak_t - stm32_start_t
                peak_pc_time = start_pc_time + peak_offset_ms / 1000.0

                send_time = time.monotonic()
                payload = {
                    "type": "swing",
                    "peak_age_ms": (send_time - peak_pc_time) * 1000.0,
                    "peak_dps": peak_dps,
                    "speed": speed,
                    "duration": duration,
                    "drop": drop,
                }
                sock.sendto(json.dumps(payload).encode("utf-8"), (UDP_IP, UDP_PORT))
                print("  -> sent:", payload)

                stm32_start_t = None
                start_pc_time = None


if __name__ == "__main__":
    main()