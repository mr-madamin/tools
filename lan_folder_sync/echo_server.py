import socket
from framing import send_msg, recv_msg

HOST = ""
PORT = 8765


def main():
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    server_sock.bind((HOST, PORT))

    server_sock.listen(1)
    print(f"Listening on port {PORT} ... (Ctrl+C to stop)")

    try:
        while True:
            conn, addr = server_sock.accept()
            print(f"Client connected from {addr[0]}:{addr[1]}")

            with conn:
                while True:
                    data = recv_msg(conn)

                    if data is None:
                        print(f"Client {addr[0]} disconnected.")
                        break

                    print(f" received {len(data)} bytes: {data!r}")
                    send_msg(conn, data)
    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        server_sock.close()


if __name__ == "__main__":
    main()
