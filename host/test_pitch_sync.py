import serial
import serial.tools.list_ports
import time

PORT = "/dev/ttyACM0"
BAUD = 115200

with serial.Serial(PORT, BAUD, timeout=1) as ser:
    print("waiting for STM32 boot / GAME_READY...")

    ready = False
    deadline = time.monotonic() + 20.0

    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue

        line = raw.decode(errors="replace").strip()
        if line:
            print(line)

        if "GAME_READY" in line:
            ready = True
            break

    if not ready:
        print("ERROR: no GAME_READY received")
        raise SystemExit(1)

    print("send PITCH")
    ser.write(b"PITCH\n")
    ser.flush()

    got_pitch_sync = False
    got_swing_end = False

    deadline = time.monotonic() + 15.0

    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue

        line = raw.decode(errors="replace").strip()
        if line:
            print(line)

        if "PITCH_SYNC" in line:
            got_pitch_sync = True

        if "SWING_END" in line:
            got_swing_end = True
            break

    if got_pitch_sync and got_swing_end:
        print("SYNC + SWING OK")
    elif got_pitch_sync:
        print("PITCH_SYNC OK, but no SWING_END received")
    else:
        print("ERROR: no PITCH_SYNC received")