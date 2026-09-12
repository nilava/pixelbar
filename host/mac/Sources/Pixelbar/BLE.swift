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
}

final class BLETransport: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var panel: CBPeripheral?
    private var cmdChar: CBCharacteristic?
    private var stateChar: CBCharacteristic?
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
    /// True once an operation has actually been accepted, which is the only
    /// evidence the link is encrypted and authenticated. `ready` means only
    /// that the characteristic was found, and finding one requires no security.
    private(set) var secured = false

    private func log(_ s: String) { onLog?(s) }

    override init() {
        super.init()
        // Its own queue: CoreBluetooth callbacks should not land on the main
        // thread, where the menu is being rebuilt.
        central = CBCentralManager(delegate: self, queue: DispatchQueue(label: "pixelbar.ble"))
    }

    /// Provokes pairing without changing anything.
    ///
    /// A Read Request is acknowledged, so a rejection for insufficient
    /// encryption comes back properly — and unlike the command write, its
    /// success has no side effect on the panel. The state characteristic is
    /// READ_ENC for exactly this.
    func provokePairing() {
        guard let p = panel, let s = stateChar else { return }
        log("reading state to provoke pairing")
        p.readValue(for: s)
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
        panel = nil
        cmdChar = nil
        stateChar = nil
        secured = false
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
        p.discoverCharacteristics([BLEIDs.command, BLEIDs.state], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        for c in service.characteristics ?? [] {
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
            secured = true
            return
        }
        if e.domain == CBATTErrorDomain, let att = CBATTError.Code(rawValue: e.code) {
            switch att {
            case .insufficientAuthentication, .insufficientEncryption:
                // Expected once, on the write that provokes pairing: the system
                // puts its dialog up and the operation is retried after the
                // link comes up secured.
                log("needs pairing — the panel is showing a code")
                onNeedsPairing?()
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
        if let e = error as NSError? {
            if e.domain == CBATTErrorDomain,
               let att = CBATTError.Code(rawValue: e.code),
               att == .insufficientAuthentication || att == .insufficientEncryption {
                log("needs pairing — the panel is showing a code")
                onNeedsPairing?()
                return
            }
            log("read failed: \(e.localizedDescription)")
            onError?(e.localizedDescription)
            return
        }
        secured = true
        guard let d = c.value else { return }
        onValue?(c.uuid, d)
    }

    private func setReady(_ v: Bool) {
        guard ready != v else { return }
        ready = v
        DispatchQueue.main.async { [weak self] in self?.onReady?(v) }
    }
}
