import socket
import sys
from framing import send_file

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 8765
path = sys.argv[2] if len(sys.argv) > 2 else "bigfile.bin"

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((HOST, PORT))
send_file(sock, ".", path)
print(f"Sent {path}")
sock.close()
