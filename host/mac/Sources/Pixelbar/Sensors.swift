// Is the microphone on? Is the camera on?
//
// Both are answered without any special permission, because neither asks what
// is being recorded — only whether some device is running. That distinction is
// the whole reason this approach is usable: a helper that had to request
// microphone access in order to know the microphone was busy would be a worse
// trade than the feature is worth.
import AVFoundation
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
/// There is no public notification for "some other process is using the
/// camera", so this is polled — but cheaply, and only while it matters. A
/// camera that is on is nearly always on *because* the microphone is, so the
/// poll runs at a relaxed interval and the mic listener does the fast work.
enum CameraMonitor {
    static func anyRunning() -> Bool {
        let types: [AVCaptureDevice.DeviceType] = [.builtInWideAngleCamera, .external, .continuityCamera]
        let session = AVCaptureDevice.DiscoverySession(
            deviceTypes: types, mediaType: .video, position: .unspecified)
        for d in session.devices where d.isInUseByAnotherApplication { return true }
        return false
    }
}
