import socket
import time

PC_PORT = 5005
STM32_IP = "192.168.0.159"
STM32_PORT = 5006

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PC_PORT))
sock.settimeout(0.2)

print(f"PC listening on UDP 0.0.0.0:{PC_PORT}")
print(f"STM32 target {STM32_IP}:{STM32_PORT}")

last_ping = 0.0

while True:
    now = time.time()

    if now - last_ping >= 1.0:
        sock.sendto(b"PING\n", (STM32_IP, STM32_PORT))
        print("sent: PING")
        last_ping = now

    try:
        data, addr = sock.recvfrom(2048)
        print(f"from {addr}: {data.decode(errors='replace').rstrip()}")
    except socket.timeout:
        pass