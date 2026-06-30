import socket

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
                    data = conn.recv(8)

                    if not data:
                        print(f"Client {addr[0]} disconnected.")
                        break

                    print(f" received {len(data)} bytes: {data!r}")
                    conn.sendall(data)
    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        server_sock.close()


if __name__ == "__main__":
    main()
