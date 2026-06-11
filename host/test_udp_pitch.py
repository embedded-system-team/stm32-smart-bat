import socket
import time

PC_PORT = 5005

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PC_PORT))
sock.settimeout(0.2)

print(f"PC listening on UDP 0.0.0.0:{PC_PORT}")

stm32_addr = None
round_id = 1
last_pitch = 0.0

while True:
    try:
        data, addr = sock.recvfrom(2048)
        text = data.decode(errors="replace").rstrip()
        print(f"from {addr}: {text}")

        if stm32_addr is None:
            stm32_addr = addr
            print(f"STM32 addr locked: {stm32_addr}")

    except socket.timeout:
        pass

    if stm32_addr is not None:
        now = time.time()

        if now - last_pitch >= 3.0:
            msg = f"PITCH round={round_id}\n".encode()
            sock.sendto(msg, stm32_addr)
            print(f"sent: {msg.decode().strip()} to {stm32_addr}")

            round_id += 1
            last_pitch = now