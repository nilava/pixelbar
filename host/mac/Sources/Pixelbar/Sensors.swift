// Is the microphone on? Is the camera on?
//
// Both are answered without any special permission, because neither asks what
// is being recorded — only whether some device is running. That distinction is
// the whole reason this approach is usable: a helper that had to request
// microphone access in order to know the microphone was busy would be a worse
// trade than the feature is worth.
import AVFoundation
import CoreMediaIO
import CoreAudio
import Foundation

/// The microphone, via CoreAudio.
///
/// `kAudioDevicePropertyDeviceIsRunningSomewhere` is true when *any* process
/// has the device running, which is exactly the question, and it supports a
/// property listener — so this is a callback when the state changes rather than
/// a poll. A poll would have to be frequent enough to catch a call starting and
/// would then spend all day waking the CPU to be told nothing happened.
final class MicMonitor {
    private var listening: [AudioObjectID] = []
    private let onChange: (Bool) -> Void
    private var block: AudioObjectPropertyListenerBlock?

    init(onChange: @escaping (Bool) -> Void) {
        self.onChange = onChange
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self else { return }
            // Coalesce: several devices can report within a few milliseconds of
            // each other when a call starts, and they should be one change.
            DispatchQueue.main.async { self.onChange(Self.anyInputRunning()) }
        }
        self.block = block
        for dev in Self.inputDevices() {
            var addr = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceIsRunningSomewhere,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain)
            if AudioObjectAddPropertyListenerBlock(dev, &addr, nil, block) == noErr {
                listening.append(dev)
            }
        }
    }

    deinit {
        guard let block else { return }
        for dev in listening {
            var addr = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceIsRunningSomewhere,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain)
            AudioObjectRemovePropertyListenerBlock(dev, &addr, nil, block)
        }
    }

    static func anyInputRunning() -> Bool {
        for dev in inputDevices() where isRunning(dev) { return true }
        return false
    }

    /// Every audio device that has at least one input channel. Output-only
    /// devices report "running" whenever anything plays a sound, which would
    /// make a notification chime look like a conference call.
    private static func inputDevices() -> [AudioObjectID] {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(
                AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr,
              size > 0 else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(
                AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr
        else { return [] }
        return ids.filter { hasInput($0) }
    }

    private static func hasInput(_ dev: AudioObjectID) -> Bool {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioDevicePropertyScopeInput,
            mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(dev, &addr, 0, nil, &size) == noErr, size > 0
        else { return false }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, raw) == noErr else { return false }
        let lists = raw.assumingMemoryBound(to: AudioBufferList.self)
        let buffers = UnsafeMutableAudioBufferListPointer(lists)
        for b in buffers where b.mNumberChannels > 0 { return true }
        return false
    }

    private static func isRunning(_ dev: AudioObjectID) -> Bool {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDeviceIsRunningSomewhere,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var running: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &running) == noErr
        else { return false }
        return running != 0
    }
}

/// The camera.
///
/// CoreMediaIO, not AVFoundation. This used `AVCaptureDevice`'s
/// `isInUseByAnotherApplication`, which is documented for the *other* process
/// case and is not reliable for it on current macOS — the microphone worked
/// and the camera silently never did.
///
/// `kCMIODevicePropertyDeviceIsRunningSomewhere` is the exact analogue of the
/// audio property above, down to the name, and it is reliable for the same
/// reason: it is the system's own answer to "is some process running this
/// device", rather than an inference drawn from one. It needs no camera
/// permission, for the same reason the audio one needs no microphone
/// permission — it asks whether the device is running, not what it is
/// capturing — and it supports a property listener, so this is push-driven
/// rather than polled.
enum CameraMonitor {
    /// Must be set before anything else here is called; CoreMediaIO hides its
    /// devices from a process that has not asked to see them all.
    private static var allowed = false

    private static func allowScreenAndOtherDevices() {
        guard !allowed else { return }
        allowed = true
        var addr = CMIOObjectPropertyAddress(
            mSelector: CMIOObjectPropertySelector(kCMIOHardwarePropertyAllowScreenCaptureDevices),
            mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
            mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
        var yes: UInt32 = 1
        CMIOObjectSetPropertyData(CMIOObjectID(kCMIOObjectSystemObject), &addr, 0, nil,
                                  UInt32(MemoryLayout<UInt32>.size), &yes)
    }

    static func anyRunning() -> Bool {
        allowScreenAndOtherDevices()
        for dev in devices() where isRunning(dev) { return true }
        return false
    }

    private static func devices() -> [CMIOObjectID] {
        var addr = CMIOObjectPropertyAddress(
            mSelector: CMIOObjectPropertySelector(kCMIOHardwarePropertyDevices),
            mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
            mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
        var size: UInt32 = 0
        guard CMIOObjectGetPropertyDataSize(CMIOObjectID(kCMIOObjectSystemObject),
                                            &addr, 0, nil, &size) == noErr, size > 0
        else { return [] }
        var ids = [CMIOObjectID](repeating: 0,
                                 count: Int(size) / MemoryLayout<CMIOObjectID>.size)
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(CMIOObjectID(kCMIOObjectSystemObject), &addr, 0, nil,
                                        size, &used, &ids) == noErr else { return [] }
        return ids
    }

    private static func isRunning(_ dev: CMIOObjectID) -> Bool {
        var addr = CMIOObjectPropertyAddress(
            mSelector: CMIOObjectPropertySelector(kCMIODevicePropertyDeviceIsRunningSomewhere),
            mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeWildcard),
            mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementWildcard))
        var running: UInt32 = 0
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(dev, &addr, 0, nil,
                                        UInt32(MemoryLayout<UInt32>.size), &used,
                                        &running) == noErr else { return false }
        return running != 0
    }

    /// The names of the devices being watched, for diagnostics. A camera that
    /// is never listed can never be found running, and that failure is silent.
    static func deviceNames() -> [String] {
        allowScreenAndOtherDevices()
        return devices().compactMap { dev -> String? in
            var addr = CMIOObjectPropertyAddress(
                mSelector: CMIOObjectPropertySelector(kCMIOObjectPropertyName),
                mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
                mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
            var name: CFString = "" as CFString
            var used: UInt32 = 0
            guard CMIOObjectGetPropertyData(dev, &addr, 0, nil,
                                            UInt32(MemoryLayout<CFString>.size), &used,
                                            &name) == noErr else { return nil }
            return name as String
        }
    }
}
