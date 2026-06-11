"""
Smart Bat -> Godot bridge.

Reads STM32 swing events from either:
  - UDP packets sent by firmware, or
  - UART debug logs from ST-LINK

Then forwards each completed swing to Godot as JSON over UDP 127.0.0.1:4242.

Godot payload:
  {
    "type": "swing",
    "peak_age_ms": <milliseconds from swing peak to bridge send time>,
    "peak_dps": <peak angular speed>,
    "speed": <estimated bat speed m/s>,
    "duration": <swing duration ms>,
    "drop": <dropped IMU samples>
  }
"""

import argparse
import json
import re
import socket
import time


try:
    import serial
    import serial.tools.list_ports
except ModuleNotFoundError:
    serial = None


DEFAULT_SERIAL_PORT = "COM3"
DEFAULT_BAUD = 115200

DEFAULT_STM32_LISTEN_HOST = "0.0.0.0"
DEFAULT_STM32_LISTEN_PORT = 5005
DEFAULT_STM32_TARGET_PORT = 5006

DEFAULT_GODOT_IP = "127.0.0.1"
DEFAULT_GODOT_PORT = 4242

HEARTBEAT_INTERVAL_S = 1.0

kv_re = re.compile(r"(\w+)=(-?\d+)")


def parse_fields(line: str) -> dict:
    return {k: int(v) for k, v in kv_re.findall(line)}


class SwingLineParser:
    def __init__(self):
        self.stm32_start_t = None
        self.start_pc_time = None
        self.last_end_t = None

    def parse_line(self, line: str):
        if "SWING_START" in line:
            fields = parse_fields(line)
            if "t" in fields:
                self.stm32_start_t = fields["t"]
                self.start_pc_time = time.monotonic()
            return None

        if "SWING_END" not in line:
            return None

        fields = parse_fields(line)
        end_pc_time = time.monotonic()

        stm32_end_t = fields.get("t", 0)
        duration = fields.get("dur", 0)
        peak_t = fields.get("peak_t", stm32_end_t)
        peak_dps = fields.get("peak_dps", 0)
        speed = fields.get("speed_x100", 0) / 100.0
        drop = fields.get("drop", 0)

        if stm32_end_t != 0 and stm32_end_t == self.last_end_t:
            return None

        self.last_end_t = stm32_end_t

        if self.stm32_start_t is None or self.start_pc_time is None:
            self.stm32_start_t = stm32_end_t - duration
            self.start_pc_time = end_pc_time - duration / 1000.0

        peak_offset_ms = peak_t - self.stm32_start_t
        peak_pc_time = self.start_pc_time + peak_offset_ms / 1000.0
        send_time = time.monotonic()

        self.stm32_start_t = None
        self.start_pc_time = None

        return {
            "type": "swing",
            "peak_age_ms": (send_time - peak_pc_time) * 1000.0,
            "peak_dps": peak_dps,
            "speed": speed,
            "duration": duration,
            "drop": drop,
        }


def send_to_godot(sock: socket.socket, godot_addr, payload: dict):
    sock.sendto(json.dumps(payload).encode("utf-8"), godot_addr)
    print("  -> godot:", payload)


def find_serial_port(default_port: str) -> str:
    if serial is None:
        raise RuntimeError("pyserial is required for --source serial. Install with: python3 -m pip install pyserial")

    candidates = list(serial.tools.list_ports.comports())
    for port in candidates:
        text = " ".join(filter(None, [port.description, port.manufacturer, port.product]))
        if any(keyword in text for keyword in ("STLink", "ST-Link", "STMicro", "STM32")):
            print(f"Auto-detected ST-LINK port: {port.device} ({port.description})")
            return port.device

    print(f"ST-LINK not found, using default serial port: {default_port}")
    for port in candidates:
        print(f"  {port.device} - {port.description}")
    return default_port


def run_serial(args, godot_sock: socket.socket, godot_addr):
    if serial is None:
        raise RuntimeError("pyserial is required for --source serial. Install with: python3 -m pip install pyserial")

    parser = SwingLineParser()
    port = args.serial_port or find_serial_port(DEFAULT_SERIAL_PORT)

    with serial.Serial(port, args.baud, timeout=1) as ser:
        print(f"Reading serial {port} @ {args.baud}, forwarding to {godot_addr[0]}:{godot_addr[1]}")
        time.sleep(2)

        while True:
            raw = ser.readline()
            if not raw:
                continue

            line = raw.decode(errors="replace").strip()
            if not line:
                continue

            print(line)
            payload = parser.parse_line(line)
            if payload is not None:
                send_to_godot(godot_sock, godot_addr, payload)


def run_udp(args, godot_sock: socket.socket, godot_addr):
    parser = SwingLineParser()

    stm32_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stm32_sock.bind((args.listen_host, args.listen_port))
    stm32_sock.settimeout(0.2)

    stm32_addr = None
    if args.stm32_ip is not None:
        stm32_addr = (args.stm32_ip, args.stm32_port)

    last_ping = 0.0

    print(f"Listening for STM32 UDP on {args.listen_host}:{args.listen_port}")
    print(f"Forwarding swings to Godot UDP {godot_addr[0]}:{godot_addr[1]}")
    if stm32_addr is not None:
        print(f"STM32 target preset: {stm32_addr[0]}:{stm32_addr[1]}")

    while True:
        now = time.monotonic()

        if stm32_addr is not None and now - last_ping >= HEARTBEAT_INTERVAL_S:
            stm32_sock.sendto(b"PING\n", stm32_addr)
            last_ping = now

        try:
            data, addr = stm32_sock.recvfrom(2048)
        except socket.timeout:
            continue

        if stm32_addr is None:
            stm32_addr = addr
            print(f"STM32 addr locked: {stm32_addr}")

        text = data.decode(errors="replace")
        for line in text.replace("\r", "\n").split("\n"):
            line = line.strip()
            if not line:
                continue

            print(line)
            payload = parser.parse_line(line)
            if payload is not None:
                send_to_godot(godot_sock, godot_addr, payload)


def parse_args():
    parser = argparse.ArgumentParser(description="Bridge STM32 smart bat events to Godot")
    parser.add_argument("--source", choices=("udp", "serial"), default="udp")

    parser.add_argument("--listen-host", default=DEFAULT_STM32_LISTEN_HOST)
    parser.add_argument("--listen-port", type=int, default=DEFAULT_STM32_LISTEN_PORT)
    parser.add_argument("--stm32-ip", default=None)
    parser.add_argument("--stm32-port", type=int, default=DEFAULT_STM32_TARGET_PORT)

    parser.add_argument("--serial-port", default=None)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)

    parser.add_argument("--godot-ip", default=DEFAULT_GODOT_IP)
    parser.add_argument("--godot-port", type=int, default=DEFAULT_GODOT_PORT)
    return parser.parse_args()


def main():
    args = parse_args()
    godot_addr = (args.godot_ip, args.godot_port)
    godot_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        if args.source == "serial":
            run_serial(args, godot_sock, godot_addr)
        else:
            run_udp(args, godot_sock, godot_addr)
    finally:
        godot_sock.close()


if __name__ == "__main__":
    main()
