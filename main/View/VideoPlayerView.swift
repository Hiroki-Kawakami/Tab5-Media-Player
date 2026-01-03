fileprivate let Log = Logger(tag: "VideoPlayer")

class VideoPlayerView {

    static var config: (
        setPlayerDisplay: ((Bool) -> ()),
        outputFormat: IDF.JPEG.Decoder.OutputFormat,
        frameBuffers: [UnsafeMutableRawBufferPointer],
        flush: (Int) -> (),
        audioSetClock: (UInt32, UInt8, UInt8) -> Void,
        audioWrite: (UnsafeMutableRawBufferPointer) -> Void
    )!
    var queue: Queue<UnsafeRawBufferPointer?>?
    let gptimer: IDF.GeneralPurposeTimer
    var esptimer: IDF.ESPTimer?
    let player: AVIPlayer
    let jpegBuffer = [UnsafeMutableBufferPointer<UInt8>]((0..<8).map({ _ in
        Memory.allocate(type: UInt8.self, capacity: 512 * 1024, capability: .spiram)!
    }))
    let audioBuffer = Memory.allocate(type: UInt8.self, capacity: 128 * 1024, capability: .spiram)!

    init() throws(IDF.Error) {
        gptimer = try IDF.GeneralPurposeTimer(resolutionHz: 1000 * 1000)
        player = try AVIPlayer()
    }

    deinit {
        for buffer in jpegBuffer {
            Memory.free(buffer)
        }
        Log.info("VideoPlayerView deinit")
    }

    struct Events: OptionSet {
        let rawValue: UInt32
        static let frameTimeout = Events(rawValue: 1 << 0)
    }

    func play(path: String) {
        guard let info = player.open(file: path) else {
            Log.error("Failed to start AVI Player")
            dispose()
            return
        }

        VideoPlayerView.config.setPlayerDisplay(true)
        queue = Queue<UnsafeRawBufferPointer?>(capacity: 1)!
        Task(name: "Decoder", priority: 10, xCoreID: 0) { _ in
            let decoder = try! IDF.JPEG.Decoder(outputFormat: VideoPlayerView.config.outputFormat)
            self.jpegDecoderTask(decoder: decoder)
            self.queue = nil
            Log.info("Task END")
        }

        VideoPlayerView.config.audioSetClock(
            info.audio.sampling_rate,
            16,
            info.audio.channels
        )

        let eventGroup = EventGroup(type: Events.self)
        esptimer = try! IDF.ESPTimer(name: "Player") {
            eventGroup.set(bits: .frameTimeout)
        }
        Task(name: "Player", priority: 8, xCoreID: 0) { _ in
            var jpegBufferIndex = 0
            var frameCount = 0
            while true {
                // if frameCount > 3 { eventGroup.wait(bits: .frameTimeout) }
                self.player.videoBuffer = self.jpegBuffer[jpegBufferIndex]
                self.player.audioBuffer = self.audioBuffer
                guard let frame = self.player.readFrame() else { break }
                if frame.type == AVI_DMUX_FRAME_TYPE_VIDEO {
                    eventGroup.wait(bits: .frameTimeout)
                    if frame.size > 0 {
                        self.queue?.send(UnsafeRawBufferPointer(self.jpegBuffer[jpegBufferIndex]))
                        jpegBufferIndex = (jpegBufferIndex + 1) % self.jpegBuffer.count
                    }
                    frameCount += 1
                }
                if frame.type == AVI_DMUX_FRAME_TYPE_AUDIO && frame.size > 0 {
                    if info.audio.codec == AVI_DMUX_AUDIO_CODEC_MP3 {
                        self.player.decoder.decode(buffer: self.audioBuffer) {
                            VideoPlayerView.config.audioWrite($0)
                        }
                    } else {
                        VideoPlayerView.config.audioWrite(
                            UnsafeMutableRawBufferPointer(start: self.audioBuffer.baseAddress, count: Int(frame.size))
                        )
                    }
                }
            }
        }
        // let fpsTime = 1000 * 1000 / info.video.frame_rate;
        // esptimer?.startPeriodic(period: UInt64(fpsTime))
        esptimer?.startPeriodic(period: UInt64(info.video.frame_rate))

    }

    func stop() {

    }

    private func dispose() {
        queue?.send(nil)
        while queue != nil { Task.delay(10) }
        VideoPlayerView.config.setPlayerDisplay(false)
    }

    private func jpegDecoderTask(decoder: IDF.JPEG.Decoder) {
        let frameBuffers = VideoPlayerView.config.frameBuffers
        var frameBufferIndex = 0
        var frameCount = 0
        var start = gptimer.count
        var decodeDurationMax: UInt64 = 0
        for jpegData in queue! {
            guard let jpegData = jpegData else { break }
            let nextFrameBufferIndex = (frameBufferIndex + 1) % frameBuffers.count
            let decodeStart = gptimer.count
            guard let _ = try? decoder.decode(inputBuffer: jpegData, outputBuffer: frameBuffers[nextFrameBufferIndex]) else {
                continue
            }
            let decodeDuration = gptimer.duration(from: decodeStart)
            if decodeDuration > decodeDurationMax { decodeDurationMax = decodeDuration }

            VideoPlayerView.config.flush(nextFrameBufferIndex)
            frameBufferIndex = nextFrameBufferIndex

            frameCount += 1
            let now = gptimer.count
            if (now - start) >= 1000000 {
                Log.info("\(frameCount)fps, decode: \(decodeDurationMax)")
                frameCount = 0
                start = now
                decodeDurationMax = 0
            }
        }
    }
}
