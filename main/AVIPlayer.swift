fileprivate let Log = Logger(tag: "AVI")

fileprivate struct AVIDemuxer {
    private var dmux: OpaquePointer?

    mutating func open(file: String) -> avi_dmux_info_t? {
        dmux = file.utf8CString.withUnsafeBufferPointer {
            avi_dmux_create($0.baseAddress!)
        }
        if let d = dmux, let info = avi_dmux_parse_info(d) {
            return info.pointee
        }
        close()
        return nil
    }
    mutating func close() {
        if let d = dmux {
            avi_dmux_delete(d)
            dmux = nil
        }
    }

    func readFrame(
        videoBuffer: UnsafeMutableBufferPointer<UInt8>,
        audioBuffer: UnsafeMutableBufferPointer<UInt8>,
    ) -> avi_dmux_frame_t? {
        var frame = avi_dmux_frame_t()
        let result = avi_dmux_read_frame(
            dmux, &frame,
            videoBuffer.baseAddress, UInt32(videoBuffer.count),
            audioBuffer.baseAddress, UInt32(audioBuffer.count)
        )
        return result ? frame : nil
    }
}

final class AVIPlayer {

    private var dmux = AVIDemuxer()
    var jpegBufferIndex = 0
    let jpegBuffer = [UnsafeMutableBufferPointer<UInt8>]((0..<8).map({ _ in
        Memory.allocate(type: UInt8.self, capacity: 512 * 1024, capability: .spiram)!
    }))
    let audioBuffer = Memory.allocate(type: UInt8.self, capacity: 64 * 1024, capability: .spiram)!
    var frameCount = 0
    var info: avi_dmux_info_t?
    var stateChangedCallback: ((State) -> ())?

    enum State {
        case play
        case pause
        case stop
        case dispose
    }
    private(set) var state: State = .stop {
        didSet { stateChangedCallback?(state) }
    }

    func open(file: String) -> Bool {
        guard let info = dmux.open(file: file) else { return false }
        self.info = info

        // setup audio codec
        switch info.audio.codec {
        case AVI_DMUX_AUDIO_CODEC_PCM:
            AudioController.codec = .pcm(rate: info.audio.sampling_rate, bps: info.audio.bits_per_sample, ch: info.audio.channels)
        case AVI_DMUX_AUDIO_CODEC_MP3:
            AudioController.codec = .mp3(rate: info.audio.sampling_rate, ch: info.audio.channels)
        default:
            AudioController.codec = nil // no audio channel
        }

        startTask()
        return true
    }
    func close() {
        state = .dispose
        while task != nil { Task.delay(10) } // wait task end
        stopTimer()
        for b in jpegBuffer { Memory.free(b) }
        Memory.free(audioBuffer)
        dmux.close()
    }

    func play() {
        if let info = info {
            state = .play
            startTimer(frameRate: UInt64(info.video.frame_rate))
        }
    }
    func pause() {
        if state == .play {
            state = .pause
        }
    }
    func resume() {
        if state == .pause {
            state = .play
        }
    }
    func stop() {
        state = .stop
        stopTimer()
    }

    private struct Events: OptionSet {
        let rawValue: UInt32
        static let frameTimeout = Events(rawValue: 1 << 0)
    }
    private let eventGroup = EventGroup(type: Events.self)

    private var task: Task?
    private func startTask() {
        task = Task(name: "AVI", priority: 8) { _ in
            Log.info("AVI Task Start")
            self.taskRoutine()
            self.task = nil
            Log.info("AVI Task End")
        }
    }
    private func taskRoutine() {
        while true {
            switch state {
            case .play: taskPlay()
            case .dispose: return
            default: Task.delay(20);
            }
        }
    }
    private func taskPlay() {
        let videoBuffer = jpegBuffer[jpegBufferIndex]
        let audioBuffer = self.audioBuffer
        guard let frame = self.dmux.readFrame(videoBuffer: videoBuffer, audioBuffer: audioBuffer) else {
            DisplayMultiplexer.showControl = true
            stop()
            return
        }
        if frame.type == AVI_DMUX_FRAME_TYPE_VIDEO {
            while true {
                let event = eventGroup.wait(bits: .frameTimeout, ticksToWait: Task.ticks(20))
                if event.contains(.frameTimeout) { break }
                if state != .play { return }
            }
            if frame.size > 0 {
                DisplayMultiplexer.drawJpeg(data: UnsafeRawBufferPointer(videoBuffer))
                jpegBufferIndex = (jpegBufferIndex + 1) % self.jpegBuffer.count
            }
            frameCount += 1
        }
        if frame.type == AVI_DMUX_FRAME_TYPE_AUDIO && frame.size > 0 {
            AudioController.write(
                data: UnsafeMutableRawBufferPointer(start: audioBuffer.baseAddress, count: Int(frame.size))
            )
        }
    }

    private var timer: IDF.ESPTimer?
    private func startTimer(frameRate: UInt64) {
        stopTimer()
        timer = try! IDF.ESPTimer(name: "Player") {
            self.eventGroup.set(bits: .frameTimeout)
        }
        timer?.startPeriodic(period: frameRate)
    }
    private func stopTimer() {
        timer?.stop()
        timer = nil
    }
}
