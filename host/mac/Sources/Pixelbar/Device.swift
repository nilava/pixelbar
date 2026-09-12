// Talking to the panel.
//
// One rule: never block the main thread, and never let a device that has gone
// away turn into a spinning menu bar. Every request has a short timeout and
// every failure is just a state change, because a desk gadget being
// unreachable is an ordinary condition and not an error worth a dialog.
import Foundation

/// The statuses the panel knows, in the order `panel::Status` declares them.
/// The wire format is that integer — the firmware validates the range and
/// ignores anything outside it.
enum Status: Int, CaseIterable {
    case free = 0, busy, call, dnd, away, focus, lunch, meet

    var label: String {
        switch self {
        case .free: return "Free"
        case .busy: return "Busy"
        case .call: return "On a call"
        case .dnd: return "Do not disturb"
        case .away: return "Away"
        case .focus: return "Focus"
        case .lunch: return "Lunch"
        case .meet: return "In a meeting"
        }
    }

    var symbol: String {
        switch self {
        case .free: return "circle"
        case .busy: return "circle.fill"
        case .call: return "phone.fill"
        case .dnd: return "minus.circle.fill"
        case .away: return "moon.fill"
        case .focus: return "target"
        case .lunch: return "fork.knife"
        case .meet: return "person.2.fill"
        }
    }
}

/// What the panel last told us about itself.
struct DeviceState {
    var screen: String = ""
    var status: String = ""
    var brightness: Int = 0
    var clock: String = ""
}

actor Device {
    private var host: String
    private let session: URLSession

    init(host: String) {
        self.host = host
        let cfg = URLSessionConfiguration.ephemeral
        // Short, and deliberately so. This runs on a LAN; anything that takes
        // longer than two seconds is a device that is off, not a slow one, and
        // the answer in both cases is the same.
        cfg.timeoutIntervalForRequest = 2
        cfg.waitsForConnectivity = false
        self.session = URLSession(configuration: cfg)
    }

    func setHost(_ h: String) { host = h }
    func currentHost() -> String { host }

    private func url(_ path: String) -> URL? {
        var h = host.trimmingCharacters(in: .whitespaces)
        if h.isEmpty { return nil }
        if !h.hasPrefix("http://") && !h.hasPrefix("https://") { h = "http://" + h }
        return URL(string: h + path)
    }

    @discardableResult
    func post(_ path: String, _ body: [String: Any]) async -> Bool {
        guard let u = url(path),
              let data = try? JSONSerialization.data(withJSONObject: body) else { return false }
        var req = URLRequest(url: u)
        req.httpMethod = "POST"
        req.httpBody = data
        do {
            let (_, resp) = try await session.data(for: req)
            return (resp as? HTTPURLResponse).map { (200..<300).contains($0.statusCode) } ?? false
        } catch {
            return false
        }
    }

    func setStatus(_ s: Status) async -> Bool {
        await post("/api/input", ["status": s.rawValue])
    }

    func draw(_ body: [String: Any]) async -> Bool {
        await post("/api/display/draw", body)
    }

    @discardableResult
    func clearDraw() async -> Bool {
        guard let u = url("/api/display/draw") else { return false }
        var req = URLRequest(url: u)
        req.httpMethod = "DELETE"
        do {
            let (_, resp) = try await session.data(for: req)
            return (resp as? HTTPURLResponse).map { (200..<300).contains($0.statusCode) } ?? false
        } catch {
            return false
        }
    }

    func state() async -> DeviceState? {
        guard let u = url("/api/state") else { return nil }
        do {
            let (data, _) = try await session.data(from: u)
            guard let j = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
            return DeviceState(
                screen: j["screen"] as? String ?? "",
                status: j["status"] as? String ?? "",
                brightness: j["brightness"] as? Int ?? 0,
                clock: j["clock"] as? String ?? "")
        } catch {
            return nil
        }
    }
}
