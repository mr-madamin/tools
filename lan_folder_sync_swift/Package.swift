// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "LanFolderSync",
    platforms: [.macOS(.v13)],
    targets: [
        // Everything the four programs share: framing, paths, manifests, config.
        .target(name: "SyncCore"),
        .executableTarget(name: "sync_server", dependencies: ["SyncCore"]),
        .executableTarget(name: "sync_push", dependencies: ["SyncCore"]),
        .executableTarget(name: "file_sender", dependencies: ["SyncCore"]),
        .executableTarget(name: "file_receiver", dependencies: ["SyncCore"]),
        .testTarget(name: "SyncCoreTests", dependencies: ["SyncCore"]),
    ]
)
