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
    static let calKey = "calEnabled"
    static let tokenKey = "apiToken"

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
    static var calEnabled: Bool {
        // Off by default: reading somebody's calendar is a thing to opt into,
        // not a thing to discover has been happening.
        get { UserDefaults.standard.bool(forKey: calKey) }
        set { UserDefaults.standard.set(newValue, forKey: calKey) }
    }
    static var token: String {
        get { UserDefaults.standard.string(forKey: tokenKey) ?? "" }
        set { UserDefaults.standard.set(newValue, forKey: tokenKey) }
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
    /// How long to leave an unreachable panel alone between WiFi probes.
    private let kUnreachableProbeSeconds = 30
    @Published private(set) var pushed: Status?     // what we last set, if anything
    @Published var paused: Bool = Prefs.paused { didSet { Prefs.paused = paused; Task { await settle() } } }

    /// How far through setup this Mac is.
    ///
    /// Explicit rather than inferred at each use site, because "has an
    /// address", "has a token" and "can actually reach it" are three different
    /// failures with three different remedies, and a menu that greys
    /// everything out without saying which is just broken.
    enum Setup: Equatable {
        case needsPanel          // no address yet
        case needsPairing        // address, but the panel refuses us
        case unreachable         // paired once, nothing answering now
        case ready

        var summary: String {
            switch self {
            case .needsPanel: return "Not set up — find your panel"
            case .needsPairing: return "Not paired — pair with the panel"
            case .unreachable: return "Panel unreachable"
            case .ready: return "Ready"
            }
        }
    }

    @Published private(set) var setup: Setup = .needsPanel

    /// Which pipe the last command actually went down.
    enum Link: String { case wifi = "Wi-Fi", bluetooth = "Bluetooth", none = "no link" }
    @Published private(set) var link: Link = .none

    @Published private(set) var meeting: Meeting?
    @Published private(set) var calendarAuthorised = false

    private let device: Device
    private let ble = BLETransport()
    private let calendars = Calendars()
    private(set) lazy var onboarding = Onboarding(ble: ble)
    private var mic: MicMonitor?
    /// What was last put on the panel, so it is only re-sent when it changes.
    /// A draw request every five seconds would restart the scroll each time.
    private var shownMeetingStart: Date?
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
        // The diagnostic channel was declared and never connected, so every
        // Bluetooth failure went nowhere. It is the reason the bug arrived as
        // "doesn't work" rather than as a message.
        ble.onLog = { [weak self] m in
            Task { @MainActor in self?.bleLog = m }
        }
        ble.onNeedsPairing = { [weak self] in
            Task { @MainActor in self?.blePairingWanted = true }
        }
        ble.onError = { [weak self] m in
            Task { @MainActor in self?.bleLog = m }
        }
        ble.onSecured = { [weak self] in
            Task { @MainActor in
                self?.blePairingWanted = false
                self?.objectWillChange.send()
            }
        }
        Task { await poll() }
    }

    /// The camera has no change notification, the link state has to be
    /// discovered somehow, and the calendar has to be re-read: one slow loop
    /// covers all three.
    private func poll() async {
        var sinceCalendar = 999
        var sinceProbe = 999
        while !Task.isCancelled {
            let cam = CameraMonitor.anyRunning()
            if cam != camOn {
                camOn = cam
                await evaluate()
            }
            // Probing WiFi costs a full timeout when it is not working, so a
            // panel that cannot be reached is not asked every five seconds.
            // It is still asked — often enough to notice going home, seldom
            // enough not to spend a third of the loop on a question whose
            // answer has not changed in an hour.
            if reachable || sinceProbe >= kUnreachableProbeSeconds {
                sinceProbe = 0
                reachable = await device.state() != nil
            } else {
                sinceProbe += 5
            }
            await refreshSetup()

            // Once a minute, not every five seconds. A calendar changes on the
            // scale that meetings are moved, and the countdown on the panel is
            // worked out there from a deadline rather than sent as a number —
            // so re-reading faster would buy nothing.
            sinceCalendar += 5
            if Prefs.calEnabled && sinceCalendar >= 60 {
                sinceCalendar = 0
                await refreshCalendar()
            }
            try? await Task.sleep(for: .seconds(5))
        }
    }

    func enableCalendar() async -> Bool {
        let ok = await calendars.requestAccess()
        calendarAuthorised = ok
        if ok {
            Prefs.calEnabled = true
            await refreshCalendar()
        }
        return ok
    }

    func disableCalendar() async {
        Prefs.calEnabled = false
        meeting = nil
        shownMeetingStart = nil
        await device.clearDraw()
        await evaluate()
    }

    private func refreshCalendar() async {
        let next = await calendars.upcoming().first
        meeting = next
        await evaluate()

        guard let m = next else {
            if shownMeetingStart != nil {
                shownMeetingStart = nil
                await device.clearDraw()
            }
            return
        }

        // Announced only in the last ten minutes. Earlier than that it is not
        // news, and a panel that shows the same meeting for an hour is a panel
        // nobody looks at.
        let soon = m.startsIn > 0 && m.startsIn <= 600
        guard soon else {
            if shownMeetingStart != nil {
                shownMeetingStart = nil
                await device.clearDraw()
            }
            return
        }
        // Only when it changes, or the scroll restarts every minute.
        guard shownMeetingStart != m.start else { return }
        shownMeetingStart = m.start

        _ = await device.draw([
            "source": "calendar",
            "text": m.title.uppercased(),
            "until": Int(m.start.timeIntervalSince1970),
            "icon": m.isVideo ? "busy" : "timer",
            // Urgent, but below a firmware update and below pairing — both of
            // which are states the device is in rather than things a host
            // asked for, and outrank every request by construction.
            "priority": 90,
            "ttl": 30,
            "color": m.isVideo ? "#00AFB9" : "#FF8A1F",
        ])
    }

    /// The status the evidence currently justifies, or nil for "nothing to say".
    ///
    /// Ordered by how directly each thing is observed. A live camera is
    /// happening now; a calendar entry is somebody's earlier intention, and
    /// people leave meetings early, join late, and decline by walking away.
    /// So the sensors win, and the calendar fills the gap where there are no
    /// sensors to go on.
    private var wanted: Status? {
        if paused || setup != .ready { return nil }
        if camOn && Prefs.camEnabled { return .call }
        if micOn && Prefs.micEnabled { return .busy }
        if Prefs.calEnabled, let m = meeting, m.isNow { return .meet }
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

    func refreshSetup() async {
        // Each await on its own line: `a || await b` puts the await inside an
        // autoclosure, which cannot cross into an actor.
        let host = await device.currentHost()
        let refused = await device.needsPairing
        if host.isEmpty && !ble.ready {
            setup = .needsPanel
        } else if Prefs.token.isEmpty || refused {
            setup = .needsPairing
        } else if !reachable && !ble.ready {
            setup = .unreachable
        } else {
            setup = .ready
        }
    }

    var isReady: Bool { setup == .ready }

    func host() async -> String { await device.currentHost() }
    /// Whichever pipe is known to work, tried first.
    ///
    /// This used to try WiFi on every command and fall back to Bluetooth when
    /// it failed. That is correct and it is slow in the one case where the
    /// fallback exists to help: on a network with client isolation — an office
    /// guest network, most hotel WiFi — the panel joins happily, gets an
    /// address, syncs its clock, and is nonetheless unreachable from this Mac
    /// forever. Every status change then waited out the full HTTP timeout
    /// before Bluetooth got a turn, and the poll loop spent two of every five
    /// seconds doing the same. That is the two radios "fighting": not
    /// contention on the air, but the helper re-learning the same lesson
    /// several times a minute.
    ///
    /// So the answer is remembered. `reachable` is measured by the poll loop
    /// and is the ordering, not a preference: when WiFi works it goes first,
    /// because it is the pipe that also carries firmware and a web page; when
    /// it is known not to, the attempt is skipped entirely rather than
    /// repeated. A failure on a link we believed in marks it down immediately,
    /// so the *next* command is already fast.
    func send(_ s: Status) async {
        // Recorded before anything is attempted, so the last resort below
        // cannot repeat an attempt that has already just timed out. Paying the
        // HTTP timeout twice in one command is worse than the behaviour this
        // whole change exists to remove.
        let triedWifi = reachable

        if reachable {
            if await device.setStatus(s) {
                link = .wifi
                return
            }
            // Believed reachable and was not. Say so now rather than finding
            // out again on the next command.
            reachable = false
        }

        if ble.ready {
            ble.send(["status": s.rawValue])
            link = .bluetooth
            return
        }

        // Neither pipe is known to work and Bluetooth is not there either.
        // One timeout is cheaper than telling somebody their panel is gone
        // when it has merely come back on a different address.
        if !triedWifi, await device.setStatus(s) {
            reachable = true
            link = .wifi
            return
        }
        link = .none
    }

    /// True if either pipe can reach the panel.
    var anyLink: Bool { reachable || ble.ready }
    /// Found and connected. Says nothing about whether the link is usable.
    var bluetoothReady: Bool { ble.ready }
    /// Found, connected, *and* an operation has been accepted — which is the
    /// only proof the link is paired.
    var bluetoothUsable: Bool { ble.secured }
    @Published private(set) var bleLog: String = ""
    @Published var blePairingWanted = false

    /// Whether the panel has refused this Mac for want of a token.
    func needsWifiPairing() async -> Bool { await device.needsPairing }

    /// The same ceremony as Bluetooth, over WiFi: ask, read the code off the
    /// panel, type it back, keep what comes out. One gesture whichever
    /// transport is carrying it, rather than two unrelated ones.
    func beginWifiPairing() async -> Bool { await device.beginPairing() }

    /// Everything setup produced, in one place: the panel's address and this
    /// Mac's own token. Stored together because they are learned together.
    func adopt(token: String, ip: String) async {
        Prefs.token = token
        Prefs.host = ip
        await device.setHost(ip)
        await device.setToken(token)
        await refreshSetup()
        await evaluate()
    }

    /// Forget everything, on both sides.
    ///
    /// The device first, because it is the half that needs a working link to
    /// reach: clearing this Mac's token before telling the panel would leave
    /// the panel still trusting a host that can no longer talk to it, and
    /// nothing on the panel to clear it from except its own menu.
    func factoryReset(includeDevice: Bool) async {
        if includeDevice { _ = await device.post("/api/factory", [:]) }
        Prefs.token = ""
        Prefs.host = ""
        Prefs.paused = false
        Prefs.calEnabled = false
        await device.setToken("")
        await device.setHost("")
        meeting = nil
        pushed = nil
        link = .none
        await refreshSetup()
    }

    func redeemWifiCode(_ code: Int) async -> Bool {
        guard let t = await device.redeem(code) else { return false }
        Prefs.token = t
        await refreshSetup()
        // Whatever the sensors already say is true now becomes worth sending.
        await evaluate()
        return true
    }

    /// Deliberately provoke Bluetooth pairing.
    ///
    /// The command characteristic needs an authenticated link, so the *first*
    /// write triggers pairing — and without this that would happen at whatever
    /// random moment a microphone first opened, which is a poor time to be
    /// asked to read six digits off a panel and type them into a dialog. This
    /// makes it a thing you sit down and do.
    func pair() async {
        guard ble.ready else { return }
        // A read rather than a write. It is acknowledged either way, so a
        // rejection comes back — but a read that *succeeds* changes nothing,
        // whereas the write this used to do would set a status as a side
        // effect of asking to pair. It also no longer goes via HTTP to find
        // out what status to echo, which was an odd dependency in the path
        // that exists for when there is no network.
        ble.provokePairing()
    }
}
