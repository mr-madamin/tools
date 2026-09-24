// The first experiment: accept one connection, receive one PUT into received/.
import Foundation
import SyncCore

@main
struct FileReceiver {
    static func main() {
        setUpProcess()
        let dest = "received"
        do {
            try FileManager.default.createDirectory(atPath: dest, withIntermediateDirectories: true)
            let listener = try Listener(host: "", port: 8765)
            print("Listening on \(listener.port) ... waiting for a file")

            let (conn, host, port) = try listener.accept()
            print("Connected from \(host):\(port)")
            if let path = try recvFile(conn, destDir: dest) {
                print("Received -> \(path)")
            }
            conn.close()
        } catch {
            die("receive failed: \(error)")
        }
    }
}
