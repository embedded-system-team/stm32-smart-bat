import random
import re
import serial
import time

PORT = "/dev/ttyACM0"
BAUD = 115200

swing_start_re = re.compile(
    r"SWING_START t=(?P<t>\d+)"
)

swing_end_re = re.compile(
    r"SWING_END t=(?P<t>\d+) "
    r"dur=(?P<dur>\d+) "
    r"peak_t=(?P<peak_t>\d+) "
    r"peak_dps=(?P<peak_dps>\d+) "
    r"speed_x100=(?P<speed_x100>\d+) "
    r"drop=(?P<drop>\d+)"
)

def classify_reaction(reaction_ms: float) -> str:
    if reaction_ms < 150:
        return "TOO EARLY"
    if reaction_ms <= 450:
        return "GOOD"
    if reaction_ms <= 800:
        return "LATE"
    return "MISS / TOO LATE"

def wait_for_swing(ser: serial.Serial):
    start_pc_time = None
    stm32_start_t = None

    while True:
        raw = ser.readline()
        if not raw:
            continue

        line = raw.decode(errors="replace").strip()
        print(line)

        m_start = swing_start_re.search(line)
        if m_start:
            start_pc_time = time.monotonic()
            stm32_start_t = int(m_start.group("t"))
            continue

        m_end = swing_end_re.search(line)
        if m_end:
            end_pc_time = time.monotonic()

            stm32_end_t = int(m_end.group("t"))
            duration = int(m_end.group("dur"))
            peak_t = int(m_end.group("peak_t"))
            peak_dps = int(m_end.group("peak_dps"))
            speed = int(m_end.group("speed_x100")) / 100.0
            drop = int(m_end.group("drop"))

            if start_pc_time is None or stm32_start_t is None:
                start_pc_time = end_pc_time
                stm32_start_t = stm32_end_t - duration

            peak_offset_ms = peak_t - stm32_start_t
            peak_pc_time = start_pc_time + peak_offset_ms / 1000.0

            return {
                "stm32_start_t": stm32_start_t,
                "stm32_end_t": stm32_end_t,
                "duration": duration,
                "peak_t": peak_t,
                "peak_dps": peak_dps,
                "speed": speed,
                "drop": drop,
                "start_pc_time": start_pc_time,
                "peak_pc_time": peak_pc_time,
                "end_pc_time": end_pc_time,
            }

def main():
    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        print(f"Listening on {PORT} @ {BAUD} baud...")
        time.sleep(2)

        round_id = 1

        while True:
            input("\nPress Enter to start next pitch...")

            print(f"\n=== Round {round_id} ===")
            print("READY")

            delay = random.uniform(1.0, 3.0)
            time.sleep(delay)

            ser.reset_input_buffer()

            pitch_time = time.monotonic()
            print("PITCH!")

            swing = wait_for_swing(ser)

            reaction_start_ms = (swing["start_pc_time"] - pitch_time) * 1000.0
            reaction_peak_ms = (swing["peak_pc_time"] - pitch_time) * 1000.0

            result = classify_reaction(reaction_peak_ms)

            print("\n=== Result ===")
            print(f"start reaction : {reaction_start_ms:.0f} ms")
            print(f"peak timing    : {reaction_peak_ms:.0f} ms")
            print(f"result         : {result}")
            print(f"duration       : {swing['duration']} ms")
            print(f"peak angular   : {swing['peak_dps']} dps")
            print(f"est. speed     : {swing['speed']:.2f} m/s")
            print(f"dropped        : {swing['drop']}")
            print("==============")

            round_id += 1

if __name__ == "__main__":
    main()