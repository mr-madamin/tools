import socket
import sys
from framing import send_msg, recv_msg

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 8765


def main():
    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    print(f"Connecting to {HOST}:{PORT} ...")
    client_sock.connect((HOST, PORT))
    print("Connected! Type a message (empty line or 'quit' to exit).")

    try:
        while True:
            line = input("> ")
            if line == "" or line == "quit":
                break

            send_msg(client_sock, line.encode("utf-8"))

            reply = recv_msg(client_sock)
            if reply is None:
                print("Server closed the connection.")
                break
            print(f" echo: {reply.decode('utf-8')}")

    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        client_sock.close()
        print("\nDisconnected.")


if __name__ == "__main__":
    main()
