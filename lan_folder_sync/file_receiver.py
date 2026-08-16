import os
import socket

from framing import recv_file

HOST, PORT = "", 8765
DEST = "received"
os.makedirs(DEST, exist_ok=True)

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind((HOST, PORT))
server.listen(1)
print(f"Listening on {PORT} ... waiting for a file")

conn, addr = server.accept()
print(f"Connected from {addr[0]}:{addr[1]}")
with conn:
    path = recv_file(conn, DEST)
    print(f"Received -> {path}")
server.close()
