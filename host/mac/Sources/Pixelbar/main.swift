// The menu-bar app.
//
// Deliberately an agent with no Dock icon and no window: it has one job, it
// should be visible enough to switch off and invisible the rest of the time.
import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var item: NSStatusItem!
    private var controller: Controller!
    private var device: Device!
    private var observers: [NSKeyValueObservation] = []
    private var searching = false
    private lazy var setupModel = SetupModel()
    private lazy var setupWindow = SetupWindowController(model: setupModel)

    func applicationDidFinishLaunching(_ note: Notification) {
        // One at a time. `make install` leaves a copy in ~/Applications under a
        // LaunchAgent, and running the built one from the source tree as well
        // gives two helpers racing to set the same status on the same panel.
        let me = ProcessInfo.processInfo.processIdentifier
        let others = NSRunningApplication.runningApplications(
            withBundleIdentifier: "com.pixelbar.helper")
            .filter { $0.processIdentifier != me }
        if !others.isEmpty {
            // The one already running wins: it is probably the installed copy,
            // started at login, and the newcomer is probably a test.
            NSApp.terminate(nil)
            return
        }

        device = Device(host: Prefs.host, token: Prefs.token)
        controller = Controller(device: device)

        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.image = NSImage(systemSymbolName: "rectangle.fill",
                                     accessibilityDescription: "Pixelbar")
        rebuildMenu()

        // The menu is rebuilt when it opens rather than kept in sync, because
        // it is only ever read at that moment and a timer that redrew a hidden
        // menu would be work nobody sees.
        Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.rebuildMenu() }
        }

        // Look for it rather than asking. The prompt is the fallback, not the
        // greeting: a device that announced its own address for twelve seconds
        // when it joined the network should not then make you read it off a
        // scrolling 24-pixel panel and type it back in.
        if Prefs.host.isEmpty { Task { await autoFind(announce: false) } }
    }

    private func rebuildMenu() {
        let menu = NSMenu()
        let ready = controller.isReady

        // The first line always says what is true, and when something is
        // missing it says which thing. A menu that greys everything out
        // without explaining is indistinguishable from one that is broken.
        let headline: String
        if searching { headline = "Looking for the panel…" }
        else if !ready { headline = controller.setup.summary }
        else if controller.paused { headline = "Paused" }
        else if let p = controller.pushed { headline = "Showing \(p.label)" }
        else { headline = "Watching" }
        let head = NSMenuItem(title: headline, action: nil, keyEquivalent: "")
        head.isEnabled = false
        menu.addItem(head)

        if !ready {
            // One obvious next step, rather than a menu of things that will
            // all fail. Setup runs find-then-pair in order and stops at
            // whichever part is missing.
            menu.addItem(.separator())
            let go = NSMenuItem(title: "Set up Pixelbar…", action: #selector(runSetup),
                                keyEquivalent: "")
            go.target = self
            menu.addItem(go)
            menu.addItem(.separator())
            let quit = NSMenuItem(title: "Quit",
                                  action: #selector(NSApplication.terminate(_:)),
                                  keyEquivalent: "q")
            menu.addItem(quit)
            item.menu = menu
            item.button?.image = NSImage(systemSymbolName: "rectangle.dashed",
                                         accessibilityDescription: "Pixelbar — not set up")
            return
        }

        let detail = "Mic \(controller.micOn ? "on" : "off") · Camera "
            + "\(controller.camOn ? "on" : "off") · \(controller.link.rawValue)"
        let d = NSMenuItem(title: detail, action: nil, keyEquivalent: "")
        d.isEnabled = false
        menu.addItem(d)
        menu.addItem(.separator())

        let pause = NSMenuItem(title: "Pause", action: #selector(togglePause), keyEquivalent: "")
        pause.target = self
        pause.state = controller.paused ? .on : .off
        menu.addItem(pause)

        let mic = NSMenuItem(title: "Microphone sets BUSY", action: #selector(toggleMic), keyEquivalent: "")
        mic.target = self
        mic.state = Prefs.micEnabled ? .on : .off
        menu.addItem(mic)

        let cam = NSMenuItem(title: "Camera sets CALL", action: #selector(toggleCam), keyEquivalent: "")
        cam.target = self
        cam.state = Prefs.camEnabled ? .on : .off
        menu.addItem(cam)

        let cal = NSMenuItem(title: "Calendar sets MEET", action: #selector(toggleCal), keyEquivalent: "")
        cal.target = self
        cal.state = Prefs.calEnabled ? .on : .off
        menu.addItem(cal)

        if Prefs.calEnabled, let m = controller.meeting {
            let mins = Int(m.startsIn / 60)
            let line = m.isNow
                ? "Now: \(m.title)"
                : (mins <= 60 ? "In \(max(mins, 0)) min: \(m.title)" : "Next: \(m.title)")
            let mi = NSMenuItem(title: line, action: nil, keyEquivalent: "")
            mi.isEnabled = false
            menu.addItem(mi)
        }
        menu.addItem(.separator())

        let sub = NSMenu()
        for s in Status.allCases {
            let mi = NSMenuItem(title: s.label, action: #selector(pick(_:)), keyEquivalent: "")
            mi.target = self
            mi.tag = s.rawValue
            sub.addItem(mi)
        }
        let setItem = NSMenuItem(title: "Set status", action: nil, keyEquivalent: "")
        menu.setSubmenu(sub, for: setItem)
        menu.addItem(setItem)
        menu.addItem(.separator())

        // No separate "pair over Bluetooth" and "pair over Wi-Fi" entries.
        // They are one act — see runSetup — and offering them separately
        // invited doing half of it and wondering why the other half was not
        // working.
        let linkLine = NSMenuItem(
            title: "Link: "
                + (controller.bluetoothUsable ? "Bluetooth paired" : "Bluetooth not paired")
                + " · " + (Prefs.token.isEmpty ? "no token" : "token held"),
            action: nil, keyEquivalent: "")
        linkLine.isEnabled = false
        menu.addItem(linkLine)


        let status = NSMenuItem(title: "Panel status…", action: #selector(runSetup),
                                keyEquivalent: "")
        status.target = self
        menu.addItem(status)

        let hostItem = NSMenuItem(title: "Panel: \(Prefs.host.isEmpty ? "—" : Prefs.host)",
                                  action: nil, keyEquivalent: "")
        hostItem.isEnabled = false
        menu.addItem(hostItem)

        let quit = NSMenuItem(title: "Quit", action: #selector(NSApplication.terminate(_:)),
                              keyEquivalent: "q")
        menu.addItem(quit)

        item.menu = menu
        item.button?.image = NSImage(
            systemSymbolName: controller.paused ? "rectangle" : "rectangle.fill",
            accessibilityDescription: "Pixelbar")
    }

    /// Find the panel, then pair with it. Stops at whichever part is missing
    /// and says why, rather than reporting a generic failure for two quite
    /// different problems.
    @objc private func runSetup() {
        setupModel.summary = controller.setup.summary
        setupModel.panelAddress = Prefs.host
        setupModel.onStart = { [weak self] in Task { await self?.runBluetoothSetup() } }
        setupModel.onReset = { [weak self] in self?.confirmReset() }
        setupWindow.show()
        // Started for you rather than waiting on a button: opening this window
        // while nothing is configured is itself the request.
        if controller.setup != .ready && setupModel.stage == .idle {
            Task { await runBluetoothSetup() }
        }
    }

    private func confirmReset() {
        let a = NSAlert()
        a.messageText = "Reset everything?"
        a.informativeText =
            "The panel forgets every paired client, every Bluetooth bond and its "
            + "Wi-Fi network, and restarts into setup. This Mac forgets its token "
            + "and the panel's address.\n\nYou will also need to forget the panel "
            + "in System Settings → Bluetooth: macOS keeps its half of the pairing "
            + "keys and will not offer the code again until it does."
        a.alertStyle = .warning
        a.addButton(withTitle: "Reset")
        a.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn else { return }
        Task {
            setupModel.stage = .idle
            setupModel.summary = "Resetting…"
            await controller.factoryReset(includeDevice: true)
            setupModel.summary = "Reset. The panel is restarting into setup."
            setupModel.panelAddress = ""
            setupModel.paired = []
            rebuildMenu()
        }
    }

    private func runBluetoothSetup() async {
        setupModel.busy = true
        defer { setupModel.busy = false }

        // Mirror every stage into the window, so the half-minute this can take
        // is legible instead of silent.
        let mirror = Task { @MainActor in
            while !Task.isCancelled {
                setupModel.stage = controller.onboarding.step
                setupModel.summary = controller.onboarding.step.summary
                try? await Task.sleep(for: .milliseconds(200))
            }
        }
        defer { mirror.cancel() }

        let who = Host.current().localizedName ?? "Mac"
        let result = await controller.onboarding.run(name: who) { nets in
            await self.chooseInWindow(nets)
        }

        if let r = result {
            await controller.adopt(token: r.token, ip: r.ip)
            setupModel.panelAddress = r.ip
            setupModel.paired = ["Bluetooth", "Wi-Fi token"]
            setupModel.stage = .done(ip: r.ip)
            setupModel.summary = "Set up"
            rebuildMenu()
            return
        }

        if case .failed(let why) = controller.onboarding.step {
            setupModel.summary = "Bluetooth setup did not finish"
            // Offered rather than done silently: falling through to another
            // transport without saying so is how somebody ends up not knowing
            // which one they are on.
            let a = NSAlert()
            a.messageText = "Bluetooth setup did not finish"
            // Wi-Fi is only offered when it could possibly work.
            //
            // A panel that has never been provisioned is sitting on its own
            // access point and is not on this network at all, so discovery
            // cannot find it however long it looks — offering the fallback
            // there produces "can't find the panel over Wi-Fi", which is true
            // and tells nobody anything. It is a real fallback only once the
            // panel has a network of its own to be found on.
            let wifiPossible = !Prefs.host.isEmpty
            a.informativeText = wifiPossible
                ? "\(why)\n\nTry over Wi-Fi instead?"
                : "\(why)\n\nWi-Fi is not an alternative yet: until the panel "
                  + "has joined a network it is on its own setup access point "
                  + "and nothing on this network can reach it."
            if wifiPossible { a.addButton(withTitle: "Try Wi-Fi") }
            a.addButton(withTitle: "Cancel")
            NSApp.activate(ignoringOtherApps: true)
            if a.runModal() == .alertFirstButtonReturn && wifiPossible {
                await runWifiSetup()
            }
        }
        rebuildMenu()
    }

    /// Hands the choice to the window and waits for its button.
    @MainActor
    private func chooseInWindow(_ nets: [Network]) async -> (ssid: String, pass: String)? {
        setupModel.networks = nets
        return await withCheckedContinuation { cont in
            var settled = false
            setupModel.resume = { choice in
                guard !settled else { return }
                settled = true
                cont.resume(returning: choice)
            }
        }
    }

    private func runWifiSetup() async {
        if Prefs.host.isEmpty {
            await autoFind(announce: true)
            if Prefs.host.isEmpty { return }
        }
        guard await controller.beginWifiPairing() else {
            note("Found the panel at \(Prefs.host) but could not ask it to pair.")
            return
        }
        let a = NSAlert()
        a.messageText = "Pair with the panel"
        a.informativeText = "It is showing six digits. Type them here."
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 160, height: 24))
        f.placeholderString = "000000"
        a.accessoryView = f
        a.addButton(withTitle: "Pair")
        a.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn,
              let code = Int(f.stringValue.trimmingCharacters(in: .whitespaces))
        else { return }
        if await controller.redeemWifiCode(code) { note("Paired over Wi-Fi.") }
        else { note("That code was wrong or had expired.") }
    }

    @objc private func togglePause() { controller.paused.toggle(); rebuildMenu() }
    @objc private func toggleMic() { Prefs.micEnabled.toggle(); rebuildMenu() }
    @objc private func toggleCam() { Prefs.camEnabled.toggle(); rebuildMenu() }

    @objc private func toggleCal() {
        Task {
            if Prefs.calEnabled {
                await controller.disableCalendar()
            } else if !(await controller.enableCalendar()) {
                // A refusal is remembered by the system, so retrying in a loop
                // achieves nothing; say where to change it instead.
                note("macOS did not grant calendar access.\n\nSystem Settings → "
                     + "Privacy & Security → Calendars, then switch this on again.")
            }
            rebuildMenu()
        }
    }

    @objc private func pick(_ sender: NSMenuItem) {
        guard let s = Status(rawValue: sender.tag) else { return }
        Task { await controller.send(s) }
    }

    @objc private func findPanel() { Task { await autoFind(announce: true) } }

    private func autoFind(announce: Bool) async {
        searching = true
        rebuildMenu()
        let found = await Discovery.find()
        searching = false

        if found.count == 1 {
            await controller.setHost(found[0].ip)
            rebuildMenu()
            if announce { note("Found the panel at \(found[0].ip).") }
            return
        }
        if found.count > 1 {
            // More than one is a real situation once there are two of these on
            // a desk, and picking silently would be picking wrongly half the
            // time.
            let a = NSAlert()
            a.messageText = "More than one panel"
            a.informativeText = "Choose which one this Mac should drive."
            let pop = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 240, height: 25))
            for f in found { pop.addItem(withTitle: "\(f.name) — \(f.ip)") }
            a.accessoryView = pop
            a.addButton(withTitle: "Use this one")
            a.addButton(withTitle: "Cancel")
            NSApp.activate(ignoringOtherApps: true)
            if a.runModal() == .alertFirstButtonReturn {
                await controller.setHost(found[pop.indexOfSelectedItem].ip)
                rebuildMenu()
            }
            return
        }
        if announce {
            note("No panel answered. Check it is powered and on this network, "
                 + "or enter its address by hand.")
        }
        if Prefs.host.isEmpty && announce { showHostPrompt() }
    }

    private func note(_ text: String) {
        let a = NSAlert()
        a.messageText = "Pixelbar"
        a.informativeText = text
        NSApp.activate(ignoringOtherApps: true)
        a.runModal()
    }
    @objc private func showHostPrompt() {
        let a = NSAlert()
        a.messageText = "Panel address"
        a.informativeText = "The address the panel showed when it joined your network, e.g. 192.168.1.42"
        a.addButton(withTitle: "Save")
        a.addButton(withTitle: "Cancel")
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        f.stringValue = Prefs.host
        f.placeholderString = "192.168.1.42"
        a.accessoryView = f
        NSApp.activate(ignoringOtherApps: true)
        if a.runModal() == .alertFirstButtonReturn {
            let h = f.stringValue.trimmingCharacters(in: .whitespaces)
            Task { await controller.setHost(h); rebuildMenu() }
        }
    }
}

// Top-level code in main.swift runs on the main thread but is not, to the
// Swift 6 concurrency checker, main-actor *isolated* — so constructing a
// @MainActor delegate here has to say out loud that the thread is already the
// right one. assumeIsolated asserts it rather than assuming it: if this were
// ever run from somewhere else it would trap instead of racing.
MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    // Accessory: no Dock icon, no menu bar of its own.
    app.setActivationPolicy(.accessory)
    // Held for the process lifetime; without this the delegate is released as
    // soon as this closure returns and the menu bar item goes with it.
    objc_setAssociatedObject(app, "pixelbar.delegate", delegate, .OBJC_ASSOCIATION_RETAIN)
    app.run()
}
