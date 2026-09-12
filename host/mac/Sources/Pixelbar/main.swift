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

        device = Device(host: Prefs.host)
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

        let state: String
        if searching {
            state = "Looking for the panel…"
        } else if controller.paused {
            state = "Paused"
        } else if let p = controller.pushed {
            state = "Showing \(p.label)"
        } else if controller.anyLink {
            state = "Watching"
        } else {
            state = "Panel unreachable"
        }
        menu.addItem(withTitle: state, action: nil, keyEquivalent: "")

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
        menu.addItem(.separator())

        // Setting a status by hand is the thing you want when the automatic
        // rules are not the whole story — stepping out, or a lunch nobody's
        // microphone knows about.
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

        let pairItem = NSMenuItem(title: controller.bluetoothReady
                                    ? "Pair over Bluetooth…" : "Bluetooth: no panel in range",
                                  action: #selector(pairBluetooth), keyEquivalent: "")
        pairItem.target = self
        pairItem.isEnabled = controller.bluetoothReady
        menu.addItem(pairItem)

        let findItem = NSMenuItem(title: "Find panel", action: #selector(findPanel), keyEquivalent: "")
        findItem.target = self
        menu.addItem(findItem)

        let hostTitle = Prefs.host.isEmpty ? "Panel address…" : "Panel address: \(Prefs.host)"
        let hostItem = NSMenuItem(title: hostTitle, action: #selector(showHostPrompt), keyEquivalent: "")
        hostItem.target = self
        menu.addItem(hostItem)

        let quit = NSMenuItem(title: "Quit", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        menu.addItem(quit)

        item.menu = menu
        item.button?.image = NSImage(
            systemSymbolName: controller.paused ? "rectangle" : "rectangle.fill",
            accessibilityDescription: "Pixelbar")
    }

    @objc private func togglePause() { controller.paused.toggle(); rebuildMenu() }
    @objc private func toggleMic() { Prefs.micEnabled.toggle(); rebuildMenu() }
    @objc private func toggleCam() { Prefs.camEnabled.toggle(); rebuildMenu() }

    @objc private func pick(_ sender: NSMenuItem) {
        guard let s = Status(rawValue: sender.tag) else { return }
        Task { await controller.send(s) }
    }

    @objc private func findPanel() { Task { await autoFind(announce: true) } }

    @objc private func pairBluetooth() {
        Task {
            await controller.pair()
            note("The panel is showing a six-digit code. macOS will ask for it "
                 + "in a moment — type what the panel shows.\n\nThe code exists "
                 + "nowhere else, so a host that can produce it is a host in the "
                 + "room.")
        }
    }

    /// Broadcast, then sweep, then ask. `announce` is false at startup so a
    /// first launch on a network with no panel on it is quiet rather than
    /// greeting you with a failure.
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
