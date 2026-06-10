import re
import serial
import time

PORT = "/dev/ttyACM0"   # Arch Linux 常見是 /dev/ttyACM0 或 /dev/ttyUSB0
BAUD = 115200

swing_end_re = re.compile(
    r"SWING_END t=(?P<t>\d+) "
    r"dur=(?P<dur>\d+) "
    r"peak_t=(?P<peak_t>\d+) "
    r"peak_dps=(?P<peak_dps>\d+) "
    r"speed_x100=(?P<speed_x100>\d+) "
    r"drop=(?P<drop>\d+)"
)

def main():
    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        print(f"Listening on {PORT} @ {BAUD} baud...")
        time.sleep(2)

        while True:
            raw = ser.readline()
            if not raw:
                continue

            line = raw.decode(errors="replace").strip()
            print(line)

            m = swing_end_re.search(line)
            if m:
                t = int(m.group("t"))
                dur = int(m.group("dur"))
                peak_t = int(m.group("peak_t"))
                peak_dps = int(m.group("peak_dps"))
                speed = int(m.group("speed_x100")) / 100.0
                drop = int(m.group("drop"))

                print("=== Swing Summary ===")
                print(f"end time     : {t} ms")
                print(f"duration     : {dur} ms")
                print(f"peak time    : {peak_t} ms")
                print(f"peak angular : {peak_dps} dps")
                print(f"est. speed   : {speed:.2f} m/s")
                print(f"dropped      : {drop}")
                print("=====================")

if __name__ == "__main__":
    main()