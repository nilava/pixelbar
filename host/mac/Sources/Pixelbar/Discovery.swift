// Finding the panel, so nobody has to type an address.
//
// Two ways, tried in that order, because neither is reliable everywhere:
//
//  1. A UDP broadcast probe. The device answers on a fixed port. Fast — one
//     packet out, one back, usually inside 200 ms — and it finds the panel
//     wherever it happens to be on the subnet.
//
//  2. A sweep of the local /24. Some networks drop broadcast between clients
//     ("AP isolation", common on guest and mesh setups), and on those the
//     probe goes nowhere. Asking all 254 addresses for /api/state is crude,
//     but it is 254 requests with a short timeout run concurrently, and it
//     works where broadcast does not.
//
// Manual entry stays, because both of these can fail on a network nobody has
// thought about yet and a text field always works.
import Foundation

struct Found: Sendable, Hashable {
    let ip: String
    let name: String
}

enum Discovery {
    private static let port: UInt16 = 51737
    private static let probe = "PIXELBAR?"

    /// Broadcasts once and collects whatever answers within `seconds`.
    static func broadcast(seconds: Double = 1.0) async -> [Found] {
        await withCheckedContinuation { cont in
            DispatchQueue.global().async {
                cont.resume(returning: probeSync(seconds: seconds))
            }
        }
    }

    private static func probeSync(seconds: Double) -> [Found] {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { return [] }
        defer { close(fd) }

        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, socklen_t(MemoryLayout<Int32>.size))
        // A read timeout rather than select(): this runs on its own queue and
        // the only thing it waits for is replies.
        var tv = timeval(tv_sec: 0, tv_usec: 200_000)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var dst = sockaddr_in()
        dst.sin_family = sa_family_t(AF_INET)
        dst.sin_port = port.bigEndian
        dst.sin_addr.s_addr = INADDR_BROADCAST

        let sent = probe.withCString { p -> Int in
            withUnsafePointer(to: &dst) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    sendto(fd, p, strlen(p), 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
        guard sent > 0 else { return [] }

        var found = Set<Found>()
        let deadline = Date().addingTimeInterval(seconds)
        var buf = [UInt8](repeating: 0, count: 256)
        while Date() < deadline {
            var from = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &from) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    recvfrom(fd, &buf, buf.count, 0, sa, &len)
                }
            }
            guard n > 0 else { continue }
            let data = Data(buf[0..<n])
            guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  (j["device"] as? String) == "pixelbar" else { continue }
            let ip = (j["ip"] as? String) ?? ipString(from)
            guard !ip.isEmpty else { continue }
            found.insert(Found(ip: ip, name: (j["name"] as? String) ?? "pixelbar"))
        }
        return Array(found)
    }

    private static func ipString(_ a: sockaddr_in) -> String {
        var addr = a.sin_addr
        var s = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &addr, &s, socklen_t(INET_ADDRSTRLEN))
        return String(cString: s)
    }

    /// Asks every address on this machine's /24 for `/api/state`.
    ///
    /// The fallback, for networks that drop broadcast between clients. Bounded
    /// concurrency: 254 sockets at once is enough to have the OS refuse some
    /// of them, and a scan that silently skipped a third of the subnet would be
    /// worse than a slow one.
    static func scan(seconds: Double = 3.0) async -> [Found] {
        guard let prefix = localPrefix() else { return [] }
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = seconds
        cfg.httpMaximumConnectionsPerHost = 1
        let session = URLSession(configuration: cfg)

        return await withTaskGroup(of: Found?.self) { group in
            var found: [Found] = []
            var launched = 0
            for host in 1...254 {
                if launched >= 48 {
                    if let r = await group.next(), let f = r { found.append(f) }
                    launched -= 1
                }
                let ip = "\(prefix).\(host)"
                group.addTask {
                    guard let u = URL(string: "http://\(ip)/api/state") else { return nil }
                    guard let (data, _) = try? await session.data(from: u),
                          let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                          j["screen"] != nil, j["brightness"] != nil else { return nil }
                    return Found(ip: ip, name: "pixelbar")
                }
                launched += 1
            }
            for await r in group { if let f = r { found.append(f) } }
            return found
        }
    }

    /// The first three octets of this machine's IPv4 address on a real
    /// interface. Loopback and link-local are skipped; so is anything without
    /// an address, which is most of the list on a Mac.
    private static func localPrefix() -> String? {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return nil }
        defer { freeifaddrs(head) }
        var best: String?
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let sa = p.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            let flags = Int32(p.pointee.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_LOOPBACK == 0 else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            guard getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(NI_MAXHOST),
                              nil, 0, NI_NUMERICHOST) == 0 else { continue }
            let ip = String(cString: host)
            if ip.hasPrefix("169.254.") { continue }   // link-local: nothing there
            let parts = ip.split(separator: ".")
            guard parts.count == 4 else { continue }
            best = parts.prefix(3).joined(separator: ".")
            // en0 first if we can tell; otherwise the first real one will do.
            if String(cString: p.pointee.ifa_name) == "en0" { break }
        }
        return best
    }

    /// Broadcast, then sweep if that found nothing.
    static func find() async -> [Found] {
        let quick = await broadcast()
        if !quick.isEmpty { return quick }
        return await scan()
    }
}
