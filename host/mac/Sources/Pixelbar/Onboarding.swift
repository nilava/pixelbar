// Setting the panel up, once, over Bluetooth.
//
// One ceremony, not two. The passkey that secures the Bluetooth link is the
// only code anybody types: over that authenticated link the panel hands back an
// API token, so Wi-Fi is authorised by the same gesture that authorised
// Bluetooth. Asking for a second code on the second transport would be asking
// the same question twice and calling it security.
//
// It also means the panel can simply *tell* the helper its address. Discovery —
// a broadcast probe, then a sweep of the subnet — exists for the case where
// nothing is set up yet; once Bluetooth is talking, the address is a field in a
// JSON document rather than something to go looking for.
import CoreBluetooth
import Foundation

struct Network: Hashable {
    let ssid: String
    let rssi: Int
    let open: Bool
}

@MainActor
final class Onboarding: ObservableObject {
    enum Step: Equatable {
        case idle
        case findingPanel
        case pairing                    // the panel is showing a code
        case scanning
        case chooseNetwork([Network])
        case joining(String)
        case done(ip: String)
        case failed(String)

        var summary: String {
            switch self {
            case .idle: return ""
            case .findingPanel: return "Looking for the panel over Bluetooth…"
            case .pairing: return "Type the code shown on the panel"
            case .scanning: return "Asking the panel what it can see…"
            case .chooseNetwork: return "Choose a network"
            case .joining(let s): return "Joining \(s)…"
            case .done(let ip): return "Set up — the panel is at \(ip)"
            case .failed(let why): return "Setup failed: \(why)"
            }
        }
    }

    @Published private(set) var step: Step = .idle

    private let ble: BLETransport
    /// One waiter per characteristic. A read is answered once; a second answer
    /// for the same characteristic belongs to the next read, not this one.
    private var waiters: [CBUUID: (Data?) -> Void] = [:]

    init(ble: BLETransport) {
        self.ble = ble
        ble.onValue = { [weak self] id, data in
            Task { @MainActor in self?.deliver(id, data) }
        }
    }

    private func deliver(_ id: CBUUID, _ data: Data) {
        guard let w = waiters.removeValue(forKey: id) else { return }
        w(data)
    }

    /// Reads one characteristic and parses the answer, or gives up.
    ///
    /// A read that needs pairing is answered with an ATT error rather than a
    /// value, so nil here is the ordinary state while somebody is reading six
    /// digits off a panel — not a failure.
    private func read(_ id: CBUUID, timeout: Double = 8) async -> [String: Any]? {
        let data: Data? = await withCheckedContinuation { cont in
            var settled = false
            waiters[id] = { d in
                guard !settled else { return }
                settled = true
                cont.resume(returning: d)
            }
            ble.read(id)
            DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { [weak self] in
                guard !settled else { return }
                settled = true
                self?.waiters[id] = nil
                cont.resume(returning: nil)
            }
        }
        guard let d = data else { return nil }
        return try? JSONSerialization.jsonObject(with: d) as? [String: Any]
    }

    /// Waits for the link to come up secured, which is the only signal that the
    /// passkey was accepted — CoreBluetooth has no callback for it.
    private func awaitPairing(seconds: Double = 90) async -> Bool {
        step = .pairing
        let deadline = Date().addingTimeInterval(seconds)
        while Date() < deadline {
            if ble.secured { return true }
            // Poking the link is what discovers that it is now usable.
            _ = await read(BLEIDs.link, timeout: 2)
            if ble.secured { return true }
        }
        return false
    }

    /// The whole thing: find, pair, scan, join, collect the token and address.
    /// `pick` chooses among the networks found; returning nil abandons setup.
    func run(name: String,
             pick: @escaping ([Network]) async -> (ssid: String, pass: String)?)
        async -> (token: String, ip: String)? {

        step = .findingPanel
        for _ in 0..<40 {
            if ble.ready { break }
            try? await Task.sleep(for: .milliseconds(500))
        }
        guard ble.ready else {
            step = .failed("no panel in range")
            return nil
        }

        if !ble.secured, !(await awaitPairing()) {
            step = .failed("not paired")
            return nil
        }

        // Introduce ourselves before asking for anything, so the token the
        // panel mints is filed under a name rather than under "host".
        ble.write(BLEIDs.provision, ["name": name])

        step = .scanning
        var networks: [Network] = []
        // The first read starts a scan and returns the previous one, which on a
        // fresh device is empty. Ask again once it has had time to finish.
        for attempt in 0..<6 {
            if let j = await read(BLEIDs.scan),
               let arr = j["networks"] as? [[String: Any]], !arr.isEmpty {
                networks = arr.compactMap {
                    guard let s = $0["ssid"] as? String else { return nil }
                    return Network(ssid: s, rssi: $0["rssi"] as? Int ?? -99,
                                   open: $0["open"] as? Bool ?? false)
                }
                if !networks.isEmpty && attempt > 0 { break }
            }
            try? await Task.sleep(for: .seconds(2))
        }

        guard !networks.isEmpty else {
            step = .failed("the panel could not see any networks")
            return nil
        }
        step = .chooseNetwork(networks.sorted { $0.rssi > $1.rssi })
        guard let choice = await pick(networks.sorted { $0.rssi > $1.rssi }) else {
            step = .idle
            return nil
        }

        step = .joining(choice.ssid)
        ble.write(BLEIDs.provision,
                  ["ssid": choice.ssid, "pass": choice.pass, "name": name])

        // Wait for an address. The panel reports its own progress, so there is
        // nothing to discover and nothing to poll over a network that may not
        // have accepted us yet.
        var ip = ""
        for _ in 0..<30 {
            try? await Task.sleep(for: .seconds(1))
            guard let j = await read(BLEIDs.link, timeout: 3) else { continue }
            if let e = j["error"] as? String, !e.isEmpty {
                step = .failed(e)
                return nil
            }
            if let got = j["ip"] as? String, !got.isEmpty, got != "0.0.0.0" {
                ip = got
                break
            }
        }
        guard !ip.isEmpty else {
            step = .failed("joined nothing in time")
            return nil
        }

        guard let j = await read(BLEIDs.token), let token = j["token"] as? String,
              token.count == 32 else {
            step = .failed("the panel would not issue a token")
            return nil
        }

        step = .done(ip: ip)
        return (token, ip)
    }
}
