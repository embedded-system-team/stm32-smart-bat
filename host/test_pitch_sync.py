import serial
import serial.tools.list_ports
import time

PORT = "/dev/ttyACM0"
BAUD = 115200


def open_serial():
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.rtscts = False
    ser.dsrdtr = False
    ser.open()

    ser.dtr = False
    ser.rts = False

    return ser


def main():
    print("available ports:")
    for p in serial.tools.list_ports.comports():
        print(f"  {p.device}: {p.description}")

    print(f"opening {PORT}...")

    with open_serial() as ser:
        print("serial opened")
        print("waiting for STM32 boot / GAME_READY...")

        ready = False
        deadline = time.monotonic() + 20.0

        while time.monotonic() < deadline:
            try:
                raw = ser.readline()
            except serial.SerialException as e:
                print(f"serial error: {e}")
                return

            if not raw:
                continue

            line = raw.decode(errors="replace").strip()

            if line:
                print(line)

            if "GAME_READY" in line:
                ready = True
                break

        if not ready:
            print("ERROR: STM32 did not send GAME_READY")
            return

        print("send PITCH")
        ser.write(b"PITCH\n")
        ser.flush()

        deadline = time.monotonic() + 3.0

        while time.monotonic() < deadline:
            try:
                raw = ser.readline()
            except serial.SerialException as e:
                print(f"serial error after PITCH: {e}")
                return

            if not raw:
                continue

            line = raw.decode(errors="replace").strip()

            if line:
                print(line)

            if "PITCH_SYNC" in line:
                print("SYNC OK")
                return

        print("ERROR: no PITCH_SYNC received")


if __name__ == "__main__":
    main()