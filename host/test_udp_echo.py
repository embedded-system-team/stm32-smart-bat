import socket
import time

PC_PORT = 5005

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PC_PORT))
sock.settimeout(0.2)

stm32_addr = None
last_ping = 0.0

while True:
    try:
        data, addr = sock.recvfrom(2048)
        text = data.decode(errors="replace").rstrip()
        print(f"from {addr}: {text}")

        stm32_addr = addr

        if "HELLO_FROM_STM32" in text:
            sock.sendto(b"PING\n", stm32_addr)
            print(f"sent immediate PING to {stm32_addr}")

    except socket.timeout:
        pass

    if stm32_addr is not None:
        now = time.time()
        if now - last_ping >= 1.0:
            sock.sendto(b"PING\n", stm32_addr)
            print(f"sent periodic PING to {stm32_addr}")
            last_ping = now