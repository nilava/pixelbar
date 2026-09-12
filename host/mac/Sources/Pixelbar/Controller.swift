// What the sensors mean, and what the panel is told.
//
// Kept apart from both the sensing and the UI, because this is the only part
// with an opinion, and opinions are the part worth being able to read.
import Foundation

struct Prefs {
    static let hostKey = "deviceHost"
    static let micKey = "micEnabled"
    static let camKey = "camEnabled"
    static let pausedKey = "paused"

    static var host: String {
        get { UserDefaults.standard.string(forKey: hostKey) ?? "" }
        set { UserDefaults.standard.set(newValue, forKey: hostKey) }
    }
    static var micEnabled: Bool {
        get { UserDefaults.standard.object(forKey: micKey) as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: micKey) }
    }
    static var camEnabled: Bool {
        get { UserDefaults.standard.object(forKey: camKey) as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: camKey) }
    }
    static var paused: Bool {
        get { UserDefaults.standard.bool(forKey: pausedKey) }
        set { UserDefaults.standard.set(newValue, forKey: pausedKey) }
    }
}

@MainActor
final class Controller: ObservableObject {
    @Published private(set) var micOn = false
    @Published private(set) var camOn = false
    @Published private(set) var reachable = false
    @Published private(set) var pushed: Status?     // what we last set, if anything
    @Published var paused: Bool = Prefs.paused { didSet { Prefs.paused = paused; Task { await settle() } } }

    /// Which pipe the last command actually went down.
    enum Link: String { case wifi = "Wi-Fi", bluetooth = "Bluetooth", none = "no link" }
    @Published private(set) var link: Link = .none

    private let device: Device
    private let ble = BLETransport()
    private var mic: MicMonitor?
    private var releaseTask: Task<Void, Never>?

    /// What the panel was showing before we first overrode it.
    ///
    /// Restoring to FREE would be wrong and quietly annoying: someone who set
    /// LUNCH by hand should still be at LUNCH after a notification sound opens
    /// and closes the microphone for two seconds. We put back what we found.
    private var displaced: Status?

    /// How long the sensors must stay quiet before the panel is handed back.
    ///
    /// A microphone closes between sentences on some conferencing apps, and a
    /// panel that flickered between BUSY and FREE would be worse than one that
    /// lagged. Two seconds is longer than any gap observed and shorter than
    /// anyone waits at the end of a call.
    private let settleSeconds: UInt64 = 2

    init(device: Device) {
        self.device = device
        mic = MicMonitor { [weak self] on in
            guard let self else { return }
            Task { @MainActor in
                self.micOn = on
                await self.evaluate()
            }
        }
        micOn = MicMonitor.anyInputRunning()
        camOn = CameraMonitor.anyRunning()
        ble.onReady = { [weak self] _ in Task { @MainActor in self?.objectWillChange.send() } }
        Task { await poll() }
    }

    /// The camera has no change notification, and the link state has to be
    /// discovered somehow, so one slow loop covers both.
    private func poll() async {
        while !Task.isCancelled {
            let cam = CameraMonitor.anyRunning()
            if cam != camOn {
                camOn = cam
                await evaluate()
            }
            reachable = await device.state() != nil
            try? await Task.sleep(for: .seconds(5))
        }
    }

    /// The status the sensors currently justify, or nil for "nothing to say".
    private var wanted: Status? {
        if paused { return nil }
        // Camera beats microphone: a call with video is a call, and CALL is
        // the more informative of the two to whoever is looking at the panel.
        if camOn && Prefs.camEnabled { return .call }
        if micOn && Prefs.micEnabled { return .busy }
        return nil
    }

    private func evaluate() async {
        releaseTask?.cancel()
        releaseTask = nil

        if let want = wanted {
            if displaced == nil { displaced = await currentStatus() }
            if pushed != want {
                pushed = want
                await send(want)
            }
            return
        }

        guard pushed != nil else { return }
        // Nothing wants the panel any more. Wait, in case this is a gap rather
        // than an ending.
        releaseTask = Task { [weak self] in
            guard let self else { return }
            try? await Task.sleep(for: .seconds(self.settleSeconds))
            guard !Task.isCancelled else { return }
            await self.release()
        }
    }

    private func release() async {
        guard wanted == nil, pushed != nil else { return }
        let back = displaced ?? .free
        displaced = nil
        pushed = nil
        await send(back)
    }

    /// Called when the pause switch moves, so pausing hands the panel straight
    /// back rather than leaving it stuck on whatever we last set.
    private func settle() async {
        if paused { await release() } else { await evaluate() }
    }

    private func currentStatus() async -> Status? {
        guard let s = await device.state() else { return nil }
        // The panel reports its status as the word it is showing.
        switch s.status.uppercased() {
        case "FREE": return .free
        case "BUSY": return .busy
        case "CALL": return .call
        case "DND": return .dnd
        case "AWAY": return .away
        case "FOCUS": return .focus
        case "LUNCH": return .lunch
        case "MEET": return .meet
        default: return nil
        }
    }

    func setHost(_ h: String) async {
        Prefs.host = h
        await device.setHost(h)
        reachable = await device.state() != nil
    }

    func host() async -> String { await device.currentHost() }
    /// WiFi first, Bluetooth second.
    ///
    /// Not a preference so much as an ordering by capability: WiFi is the pipe
    /// that also carries firmware and a web page, so when it is there it is the
    /// one to use. Bluetooth needs no credentials, no router and no address,
    /// which is exactly when it is the only one left.
    func send(_ s: Status) async {
        if await device.setStatus(s) {
            link = .wifi
            return
        }
        if ble.ready {
            ble.send(["status": s.rawValue])
            link = .bluetooth
            return
        }
        link = .none
    }

    /// True if either pipe can reach the panel.
    var anyLink: Bool { reachable || ble.ready }
}
