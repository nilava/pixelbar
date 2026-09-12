// A window that says what is happening.
//
// Setup used to be a sequence of modal alerts with nothing in between: you
// typed a passkey and then watched a menu-bar icon for thirty seconds with no
// idea whether the panel was scanning, joining, or had given up. Every step of
// this takes real time — a scan is two seconds, a join can be ten — and a
// process that goes quiet for that long reads as broken even when it is
// working perfectly.
//
// So: one window, live, with every step visible and the current one named. The
// network picker is part of it rather than another alert on top of it, because
// choosing a network is a step of setup and not an interruption to it.
import AppKit
import SwiftUI

@MainActor
final class SetupWindowController: NSObject, NSWindowDelegate {
    private var window: NSWindow?
    private let model: SetupModel

    init(model: SetupModel) {
        self.model = model
        super.init()
    }

    func show() {
        if let w = window {
            w.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            return
        }
        let w = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 380, height: 430),
            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        w.title = "Pixelbar"
        w.center()
        w.isReleasedWhenClosed = false
        w.delegate = self
        w.contentView = NSHostingView(rootView: SetupView(model: model))
        window = w
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func windowWillClose(_ n: Notification) { window = nil }
}

/// What the window shows. Kept apart from Onboarding so the window can also be
/// opened when nothing is being set up, to show what already is.
@MainActor
final class SetupModel: ObservableObject {
    @Published var stage: Onboarding.Step = .idle
    @Published var networks: [Network] = []
    @Published var chosen: String = ""
    @Published var password: String = ""
    @Published var summary: String = ""
    @Published var panelAddress: String = ""
    @Published var paired: [String] = []
    @Published var busy = false

    /// Set while a network is being chosen; resuming it continues setup.
    var resume: ((String, String)?) -> Void = { _ in }

    var onStart: () -> Void = {}
    var onReset: () -> Void = {}
}

private struct StepRow: View {
    let title: String
    let state: State
    enum State { case waiting, active, done, failed }

    var body: some View {
        HStack(spacing: 10) {
            Group {
                switch state {
                case .waiting: Image(systemName: "circle").foregroundStyle(.tertiary)
                case .active:  ProgressView().controlSize(.small)
                case .done:    Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                case .failed:  Image(systemName: "xmark.circle.fill").foregroundStyle(.red)
                }
            }
            .frame(width: 18, height: 18)
            Text(title)
                .foregroundStyle(state == .waiting ? .secondary : .primary)
            Spacer()
        }
    }
}

struct SetupView: View {
    @ObservedObject var model: SetupModel

    private func state(for index: Int) -> StepRow.State {
        let now = order(model.stage)
        if case .failed = model.stage { return index == now ? .failed : (index < now ? .done : .waiting) }
        // A refusal marks the join row red while the picker comes back, rather
        // than leaving it spinning on an attempt that is already over.
        if case .rejected = model.stage { return index == now ? .failed : (index < now ? .done : .waiting) }
        if index < now { return .done }
        if index == now { return .active }
        return .waiting
    }

    /// Where each stage sits in the list, so a step that has been passed shows
    /// as done rather than as still running.
    private func order(_ s: Onboarding.Step) -> Int {
        switch s {
        case .idle: return -1
        case .findingPanel: return 0
        case .pairing: return 1
        case .scanning: return 2
        case .chooseNetwork: return 2
        case .joining: return 3
        // The network refused us. It is the join that failed, so that is the
        // row that should be showing it while the picker comes back.
        case .rejected: return 3
        case .collecting: return 4
        case .done: return 4
        case .failed: return 4
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Image(systemName: "rectangle.fill").font(.title2).foregroundStyle(.orange)
                VStack(alignment: .leading, spacing: 1) {
                    Text("Pixelbar").font(.headline)
                    Text(model.summary.isEmpty ? "Not set up" : model.summary)
                        .font(.caption).foregroundStyle(.secondary)
                }
            }

            Divider()

            VStack(alignment: .leading, spacing: 9) {
                StepRow(title: "Find the panel over Bluetooth", state: state(for: 0))
                StepRow(title: "Pair — type the code shown on the panel",
                        state: state(for: 1))
                StepRow(title: "Ask the panel what networks it can see",
                        state: state(for: 2))
                StepRow(title: "Join the network", state: state(for: 3))
                StepRow(title: "Collect the address and a token", state: state(for: 4))
            }

            // Only while it is the current step: a picker that stays on screen
            // after the choice is made invites changing an answer that has
            // already been acted on.
            if case .chooseNetwork(let nets) = model.stage {
                Divider()
                Text("Pick a network").font(.subheadline).bold()
                Picker("", selection: $model.chosen) {
                    ForEach(nets, id: \.ssid) { n in
                        Text("\(n.ssid)  \(bars(n.rssi))\(n.open ? "" : "  🔒")").tag(n.ssid)
                    }
                }
                .labelsHidden()
                SecureField("Password (leave empty if open)", text: $model.password)
                    .textFieldStyle(.roundedBorder)
                Button("Join") {
                    let ssid = model.chosen.isEmpty ? (nets.first?.ssid ?? "") : model.chosen
                    model.resume((ssid, model.password))
                    model.password = ""
                }
                .keyboardShortcut(.defaultAction)
                .disabled(nets.isEmpty)
                .onAppear { if model.chosen.isEmpty { model.chosen = nets.first?.ssid ?? "" } }
            }

            if case .pairing = model.stage {
                Divider()
                Text("The panel is showing six digits. macOS will ask for them — "
                     + "type what the panel shows.")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if case .done = model.stage {
                Divider()
                LabeledContent("Panel", value: model.panelAddress)
                if !model.paired.isEmpty {
                    LabeledContent("Paired", value: model.paired.joined(separator: ", "))
                }
                Text("One code authorised both — Bluetooth is bonded and this Mac "
                     + "holds its own Wi-Fi token.")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if case .failed(let why) = model.stage {
                Divider()
                Text(why).font(.caption).foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Spacer(minLength: 0)
            Divider()
            HStack {
                Button(model.stage == .idle ? "Set up" : "Start again") { model.onStart() }
                    .disabled(model.busy && model.stage != .idle)
                Spacer()
                Button("Reset everything…") { model.onReset() }
                    .foregroundStyle(.red)
            }
        }
        .padding(18)
        .frame(width: 380, height: 430, alignment: .topLeading)
    }

    private func bars(_ rssi: Int) -> String {
        rssi > -55 ? "●●●" : (rssi > -70 ? "●●○" : "●○○")
    }
}
