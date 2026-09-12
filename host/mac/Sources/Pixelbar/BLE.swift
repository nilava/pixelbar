// The same device, over Bluetooth.
//
// A fallback rather than a replacement: WiFi carries firmware images and a web
// page and reaches the panel from anywhere in the house, and this carries two
// short characteristics. But it needs no credentials, no router, no address and
// no discovery — the panel is simply there or it is not — so it is what keeps
// the helper working on a network that has not been set up, has gone down, or
// is the kind that blocks everything between two clients.
import CoreBluetooth
import Foundation

enum BLEIDs {
    static let service = CBUUID(string: "00006F9A-0001-5E4B-A311-4E7D0C216F9A")
    static let command = CBUUID(string: "00006F9A-0002-5E4B-A311-4E7D0C216F9A")
    static let state   = CBUUID(string: "00006F9A-0003-5E4B-A311-4E7D0C216F9A")
    // Onboarding, all four behind the passkey.
    static let scan    = CBUUID(string: "00006F9A-0004-5E4B-A311-4E7D0C216F9A")
    static let provision = CBUUID(string: "00006F9A-0005-5E4B-A311-4E7D0C216F9A")
    static let link    = CBUUID(string: "00006F9A-0006-5E4B-A311-4E7D0C216F9A")
    static let token   = CBUUID(string: "00006F9A-0007-5E4B-A311-4E7D0C216F9A")
}

final class BLETransport: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var panel: CBPeripheral?
    private var cmdChar: CBCharacteristic?
    private var stateChar: CBCharacteristic?
    private var chars: [CBUUID: CBCharacteristic] = [:]
    private var poweredOn = false

    /// Set when the panel is connected and its command characteristic is known,
    /// which is the only state in which this transport can do anything.
    private(set) var ready = false
    var onReady: ((Bool) -> Void)?
    /// Where this transport says what it is doing. Bluetooth fails quietly and
    /// in several different ways — powered off, permission withheld, nothing
    /// advertising — and they are indistinguishable from "not ready" without
    /// somewhere to say which.
    var onLog: ((String) -> Void)?
    /// Raised when the panel has refused an operation until the link is paired.
    var onNeedsPairing: (() -> Void)?
    var onError: ((String) -> Void)?
    /// A characteristic answered with a value.
    var onValue: ((CBUUID, Data) -> Void)?
    /// A read we asked for came back as an error rather than a value.
    ///
    /// Without this a failed read was indistinguishable from a slow one:
    /// nothing resolved the waiter, so every refusal cost the full timeout and
    /// then reported that the panel had not answered — when it had answered
    /// immediately, and said why.
    var onReadError: ((CBUUID, String) -> Void)?
    /// The link came up secured — which is the only signal that pairing
    /// actually completed, because CoreBluetooth has no such callback.
    var onSecured: (() -> Void)?
    /// True once an operation *we asked for* on an authenticated characteristic
    /// has been accepted. Nothing else counts as evidence.
    ///
    /// This used to be set by any successful `didUpdateValueFor`, and that
    /// callback carries notifications as well as read responses. The panel
    /// notifies its state four times a second, so the first of those — arriving
    /// down an unencrypted link, unasked for, before anybody had typed
    /// anything — set this true and the whole ceremony was skipped. Setup then
    /// walked into a provisioning write that could only be refused, reported
    /// that the panel could see no networks, and offered Wi-Fi instead, which
    /// cannot possibly work: an unprovisioned panel is sitting on its own
    /// access point and is not on this network at all.
    ///
    /// Two conditions, both necessary. *We asked*, because an unsolicited value
    /// says nothing about what we are allowed to do; and *authenticated*,
    /// because the state characteristic is only READ_ENC, which Just Works
    /// satisfies without anyone reading a passkey off the panel.
    private(set) var secured = false

    /// Characteristics that need the passkey, not merely encryption. Only a
    /// successful operation on one of these proves the ceremony happened.
    private static let authenticated: Set<CBUUID> = [
        BLEIDs.command, BLEIDs.scan, BLEIDs.provision, BLEIDs.link, BLEIDs.token,
    ]
    /// Reads this transport has issued and not yet seen answered. A value for a
    /// characteristic that is not in here arrived on its own.
    private var pendingReads: Set<CBUUID> = []

    /// macOS is holding keys for a panel that no longer has its half — the
    /// state a factory reset leaves behind. It will not offer the code again
    /// until the panel is forgotten in System Settings, and there is nothing
    /// this process can do about it.
    var onStalePairing: (() -> Void)?

    private func noteSecured(_ id: CBUUID) {
        guard BLETransport.authenticated.contains(id) else { return }
        guard !secured else { return }
        secured = true
        endPairingRetries()
        onSecured?()
    }

    private func log(_ s: String) { onLog?(s) }

    override init() {
        super.init()
        // Its own queue: CoreBluetooth callbacks should not land on the main
        // thread, where the menu is being rebuilt.
        central = CBCentralManager(delegate: self, queue: DispatchQueue(label: "pixelbar.ble"))
    }

    /// Keep asking until the link comes up, or give up after a minute.
    ///
    /// CoreBluetooth exposes no "pairing finished" callback and no link-security
    /// property: the only way to learn that the passkey was accepted is to try
    /// the operation again and see it succeed. Without this the helper sat at
    /// "not paired" after a perfectly good pairing, until something else
    /// happened to touch the link — which is exactly the "stuck in some state,
    /// then eventually paired" that was reported.
    private var retryTimer: DispatchSourceTimer?

    private func beginPairingRetries() {
        guard retryTimer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "pixelbar.ble.retry"))
        // Every two seconds: slower than a person can type six digits, faster
        // than they will wonder whether it worked.
        t.schedule(deadline: .now() + 2, repeating: 2)
        var left = 30
        t.setEventHandler { [weak self] in
            guard let self else { return }
            if self.secured || left <= 0 {
                self.endPairingRetries()
                if self.secured { self.log("paired") }
                else { self.log("pairing timed out") }
                return
            }
            left -= 1
            self.provokePairing()
        }
        retryTimer = t
        t.resume()
    }

    private func endPairingRetries() {
        retryTimer?.cancel()
        retryTimer = nil
    }

    /// Reads one characteristic; the answer arrives on `onValue`.
    func read(_ id: CBUUID) {
        guard let p = panel, let c = chars[id] else {
            log("read \(id.uuidString): no such characteristic")
            return
        }
        pendingReads.insert(id)
        p.readValue(for: c)
    }

    /// Writes JSON to one characteristic, acknowledged.
    func write(_ id: CBUUID, _ body: [String: Any]) {
        guard let p = panel, let c = chars[id],
              let d = try? JSONSerialization.data(withJSONObject: body) else { return }
        p.writeValue(d, for: c, type: .withResponse)
    }

    /// Provokes pairing without changing anything.
    ///
    /// A Read Request is acknowledged, so a rejection for insufficient
    /// authentication comes back properly — and unlike the command write, its
    /// success has no side effect on the panel.
    ///
    /// The link characteristic, not the state one. State is READ_ENC and
    /// notified: reading it succeeds on a Just Works link that authenticated
    /// nobody, and its unsolicited notifications look exactly like a successful
    /// read. Link is READ_AUTHEN and is never pushed, so a success on it means
    /// the passkey was typed and nothing else does.
    func provokePairing() {
        guard chars[BLEIDs.link] != nil else { return }
        log("reading the link to provoke pairing")
        read(BLEIDs.link)
    }

    func send(_ body: [String: Any]) {
        guard ready, let p = panel, let c = cmdChar,
              let data = try? JSONSerialization.data(withJSONObject: body) else { return }
        // Always with response.
        //
        // This preferred the unacknowledged form when the characteristic
        // offered it, which cost the entire pairing flow: an ATT Write Command
        // has no response PDU, so a rejection for insufficient authentication
        // cannot come back, CoreBluetooth never learns pairing is needed, and
        // never starts it. The device no longer offers the fast form at all;
        // asking for it explicitly here too means this file says why.
        p.writeValue(data, for: c, type: .withResponse)
    }

    // MARK: central

    func centralManagerDidUpdateState(_ m: CBCentralManager) {
        let name: String
        switch m.state {
        case .poweredOn: name = "on"
        case .poweredOff: name = "Bluetooth is switched off"
        case .unauthorized: name = "permission withheld"
        case .unsupported: name = "no Bluetooth on this Mac"
        case .resetting: name = "resetting"
        default: name = "unknown"
        }
        log("central: \(name)")
        poweredOn = m.state == .poweredOn
        if poweredOn { scan() } else { setReady(false) }
    }

    private func scan() {
        guard poweredOn, panel == nil else { return }
        // Filtered by service UUID rather than by name: a name is what someone
        // types into a settings screen and can be changed, and the service is
        // what actually says this is a panel.
        log("scanning for \(BLEIDs.service.uuidString)")
        central.scanForPeripherals(withServices: [BLEIDs.service])
    }

    func centralManager(_ m: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        log("found \(p.name ?? "?") rssi \(rssi)")
        m.stopScan()
        panel = p
        p.delegate = self
        m.connect(p)
    }

    func centralManager(_ m: CBCentralManager, didConnect p: CBPeripheral) {
        log("connected, discovering services")
        p.discoverServices([BLEIDs.service])
    }

    func centralManager(_ m: CBCentralManager, didDisconnectPeripheral p: CBPeripheral,
                        error: Error?) {
        // A panel that was reset while this Mac kept its keys drops the link
        // here rather than answering anything, so this is one of the two places
        // the stale-bond case is visible at all.
        if let e = error as NSError?, isStalePairing(e) {
            log("this Mac is holding old keys for the panel")
            onStalePairing?()
        }
        panel = nil
        pendingReads.removeAll()
        cmdChar = nil
        stateChar = nil
        secured = false
        endPairingRetries()
        setReady(false)
        // Straight back to looking. A panel that was unplugged and plugged in
        // again should be picked up without anyone being told about it.
        scan()
    }

    func centralManager(_ m: CBCentralManager, didFailToConnect p: CBPeripheral,
                        error: Error?) {
        panel = nil
        scan()
    }

    // MARK: peripheral

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == BLEIDs.service }) else { return }
        // All of them: onboarding needs four more, and asking for the whole
        // service is one round trip rather than five.
        p.discoverCharacteristics(nil, for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        for c in service.characteristics ?? [] {
            chars[c.uuid] = c
            if c.uuid == BLEIDs.command { cmdChar = c }
            if c.uuid == BLEIDs.state { stateChar = c }
        }
        log("characteristics: \(service.characteristics?.count ?? 0), command \(cmdChar == nil ? "missing" : "found")")
        setReady(cmdChar != nil)
    }

    // MARK: results

    /// The only callback that carries an ATT error back from a write, and the
    /// only place insufficient-authentication becomes visible. Its absence is
    /// why the bug report was "pairing does not work" rather than an error:
    /// every failure below was happening silently.
    func peripheral(_ p: CBPeripheral, didWriteValueFor c: CBCharacteristic,
                    error: Error?) {
        guard let e = error as NSError? else {
            log("write accepted")
            noteSecured(c.uuid)
            return
        }
        if e.domain == CBATTErrorDomain, let att = CBATTError.Code(rawValue: e.code) {
            switch att {
            case .insufficientAuthentication, .insufficientEncryption:
                log("needs pairing — the panel is showing a code")
                onNeedsPairing?()
                beginPairingRetries()
                return
            default:
                break
            }
        }
        log("write failed: \(e.localizedDescription)")
        onError?(e.localizedDescription)
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic,
                    error: Error?) {
        // Whether this is an answer to something we asked for, or the panel
        // talking on its own. The distinction is the whole fix: only the former
        // is evidence of anything.
        let solicited = pendingReads.remove(c.uuid) != nil
        if let e = error as NSError? {
            if e.domain == CBATTErrorDomain,
               let att = CBATTError.Code(rawValue: e.code),
               att == .insufficientAuthentication || att == .insufficientEncryption {
                log("needs pairing — the panel is showing a code")
                onNeedsPairing?()
                beginPairingRetries()
                // Deliberately not resolved: this one *is* worth waiting out,
                // because somebody is reading six digits off a panel and the
                // retry will answer it properly.
                return
            }
            if isStalePairing(e) {
                log("this Mac is holding old keys for the panel")
                onStalePairing?()
                if solicited { onReadError?(c.uuid, "this Mac holds old pairing keys") }
                return
            }
            log("read failed: \(e.localizedDescription)")
            if solicited { onReadError?(c.uuid, e.localizedDescription) }
            onError?(e.localizedDescription)
            return
        }
        if solicited { noteSecured(c.uuid) }
        guard let d = c.value else { return }
        onValue?(c.uuid, d)
    }

    /// macOS kept its half of a bond the panel has since forgotten.
    ///
    /// Worth telling apart from every other failure, because it is the one the
    /// user has to fix somewhere else: macOS will not offer the passkey dialog
    /// again while it believes it already has keys, so retrying — which is what
    /// this transport does with everything else — can only fail forever.
    /// Deliberately narrow: an ATT insufficient-authentication is the ordinary
    /// "please pair now" and is handled by provoking, not by sending anybody to
    /// System Settings.
    private func isStalePairing(_ e: NSError) -> Bool {
        e.domain == CBErrorDomain
            && e.code == CBError.peerRemovedPairingInformation.rawValue
    }

    private func setReady(_ v: Bool) {
        guard ready != v else { return }
        ready = v
        DispatchQueue.main.async { [weak self] in self?.onReady?(v) }
    }
}
