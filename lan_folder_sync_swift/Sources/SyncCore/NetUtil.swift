// Host/network facts the CLI programs use to explain themselves (macOS).
//
// Nothing here is part of the wire protocol — it exists so a failed push can
// say *why* it failed instead of hanging silently. Python shells out to
// `ipconfig` and `ifconfig`; getifaddrs() is the same answer without a
// subprocess, as in the C port.
import Darwin

private func forEachIPv4Interface(_ body: (_ name: String, _ flags: UInt32, _ addr: in_addr) -> Void) {
    var list: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&list) == 0 else { return }
    defer { freeifaddrs(list) }

    var cursor = list
    while let ifa = cursor {
        defer { cursor = ifa.pointee.ifa_next }
        guard let sa = ifa.pointee.ifa_addr, sa.pointee.sa_family == sa_family_t(AF_INET) else { continue }
        let addr = sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr }
        body(String(cString: ifa.pointee.ifa_name), ifa.pointee.ifa_flags, addr)
    }
}

/// This Mac's en0 IPv4 (Wi-Fi, usually), or nil if offline / not on en0.
public func lanIP() -> String? {
    var found: String?
    forEachIPv4Interface { name, flags, addr in
        if found == nil, name == "en0", flags & UInt32(IFF_UP) != 0, flags & UInt32(IFF_LOOPBACK) == 0 {
            found = ipv4String(addr)
        }
    }
    return found
}

private func isIPv4(_ address: String) -> Bool {
    let parts = address.split(separator: ".", omittingEmptySubsequences: false)
    return parts.count == 4 && parts.allSatisfy { part in
        !part.isEmpty && part.allSatisfy(\.isASCII) && part.allSatisfy(\.isNumber) && (Int(part) ?? 256) < 256
    }
}

/// True if two addresses share a /24. Rough, but right for home LANs.
///
/// Returns true when either side isn't a literal IPv4 address — we can't tell,
/// so we don't cry wolf.
public func sameSubnet(_ a: String, _ b: String) -> Bool {
    guard isIPv4(a), isIPv4(b) else { return true }
    return a.split(separator: ".").prefix(3) == b.split(separator: ".").prefix(3)
}

/// Up tunnel interfaces carrying IPv4 — i.e. a VPN is probably running.
///
/// A VPN that claims the default route can swallow LAN traffic, which looks
/// exactly like a firewall drop from the push side.
public func tunnelInterfaces() -> [String] {
    var found = Set<String>()
    forEachIPv4Interface { name, flags, _ in
        let isTunnel = ["utun", "ppp", "ipsec"].contains { name.hasPrefix($0) }
        if isTunnel && flags & UInt32(IFF_UP) != 0 { found.insert(name) }
    }
    return found.sorted()
}
