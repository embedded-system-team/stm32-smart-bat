import socket

HOST = "0.0.0.0"
PORT = 5005

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((HOST, PORT))

print(f"listening UDP on {HOST}:{PORT}")

while True:
    data, addr = sock.recvfrom(2048)
    print(f"from {addr}: {data.decode(errors='replace').rstrip()}")