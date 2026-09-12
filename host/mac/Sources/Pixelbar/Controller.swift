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

    @Published private(set) var meeting: Meeting?
    @Published private(set) var calendarAuthorised = false

    private let device: Device
    private let ble = BLETransport()
    private let calendars = Calendars()
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
        Task { await poll() }
    }

    /// The camera has no change notification, the link state has to be
    /// discovered somehow, and the calendar has to be re-read: one slow loop
    /// covers all three.
    private func poll() async {
        var sinceCalendar = 999
        while !Task.isCancelled {
            let cam = CameraMonitor.anyRunning()
            if cam != camOn {
                camOn = cam
                await evaluate()
            }
            reachable = await device.state() != nil

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
        if paused { return nil }
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
    var bluetoothReady: Bool { ble.ready }

    /// Deliberately provoke Bluetooth pairing.
    ///
    /// The command characteristic needs an authenticated link, so the *first*
    /// write triggers pairing — and without this that would happen at whatever
    /// random moment a microphone first opened, which is a poor time to be
    /// asked to read six digits off a panel and type them into a dialog. This
    /// makes it a thing you sit down and do.
    func pair() async {
        guard ble.ready else { return }
        // A write whose effect is nothing: the status it already has. What
        // matters is that it is a write, and therefore needs the link secured.
        let current = await currentStatus() ?? .free
        ble.send(["status": current.rawValue])
    }
}
