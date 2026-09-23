// The first experiment: push one file at file_receiver. No HELLO, no manifest.
import SyncCore

@main
struct FileSender {
    static func main() {
        setUpProcess()
        let args = CommandLine.arguments
        let host = args.count > 1 ? args[1] : "127.0.0.1"
        let path = args.count > 2 ? args[2] : "bigfile.bin"

        do {
            let sock = try Socket.connect(host: host, port: 8765, timeout: 8)
            if try sendFile(sock, root: ".", relPath: path) {
                print("  ! \(path) changed while being sent; the copy is padded or clipped")
            }
            print("Sent \(path)")
            sock.close()
        } catch {
            die("send failed: \(error)")
        }
    }
}
