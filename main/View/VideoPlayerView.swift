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
    let timer: IDF.Timer
    let player: AVIPlayer
    let jpegBuffer = [UnsafeMutableBufferPointer<UInt8>]((0..<8).map({ _ in
        Memory.allocate(type: UInt8.self, capacity: 512 * 1024, capability: .spiram)!
    }))

    init() throws(IDF.Error) {
        timer = try IDF.Timer()
        player = try AVIPlayer()
    }

    deinit {
        for buffer in jpegBuffer {
            Memory.free(buffer)
        }
        Log.info("VideoPlayerView deinit")
    }

    func play(path: String) {
        VideoPlayerView.config.setPlayerDisplay(true)
        queue = Queue<UnsafeRawBufferPointer?>(capacity: 1)!
        Task(name: "Decoder", priority: 10, xCoreID: 0) { _ in
            let decoder = try! IDF.JPEG.Decoder(outputFormat: VideoPlayerView.config.outputFormat)
            self.jpegDecoderTask(decoder: decoder)
            self.queue = nil
            Log.info("Task END")
        }

        var jpegBufferIndex = 0
        player.onVideoData { jpegData, _ in
            _ = self.jpegBuffer[jpegBufferIndex].initialize(from: jpegData)
            self.queue?.send(UnsafeRawBufferPointer(start: self.jpegBuffer[jpegBufferIndex].baseAddress, count: jpegData.count))
            jpegBufferIndex = (jpegBufferIndex + 1) % self.jpegBuffer.count
        }
        player.onAudioData {
            VideoPlayerView.config.audioWrite($0)
        }
        player.onAudioSetClock {
            VideoPlayerView.config.audioSetClock($0, $1, $2)
        }
        player.onPlayEnd {
            // TODO
        }

        do {
            try player.play(file: path)
        } catch {
            Log.error("Failed to start AVI Player")
            dispose()
        }
    }

    func stop() {

    }

    private func dispose() {
        queue?.send(nil)
        while queue != nil { Task.delay(10) }
        player.onVideoData(nil)
        player.onAudioData(nil)
        player.onAudioSetClock(nil)
        player.onPlayEnd(nil)
        VideoPlayerView.config.setPlayerDisplay(false)
    }

    private func jpegDecoderTask(decoder: IDF.JPEG.Decoder) {
        let frameBuffers = VideoPlayerView.config.frameBuffers
        var frameBufferIndex = 0
        var frameCount = 0
        var start = timer.count
        var decodeDurationMax: UInt64 = 0
        for jpegData in queue! {
            guard let jpegData = jpegData else { break }
            let nextFrameBufferIndex = (frameBufferIndex + 1) % frameBuffers.count
            let decodeStart = timer.count
            guard let _ = try? decoder.decode(inputBuffer: jpegData, outputBuffer: frameBuffers[nextFrameBufferIndex]) else {
                continue
            }
            let decodeDuration = timer.duration(from: decodeStart)
            if decodeDuration > decodeDurationMax { decodeDurationMax = decodeDuration }

            VideoPlayerView.config.flush(nextFrameBufferIndex)
            frameBufferIndex = nextFrameBufferIndex

            frameCount += 1
            let now = timer.count
            if (now - start) >= 1000000 {
                Log.info("\(frameCount)fps, decode: \(decodeDurationMax)")
                frameCount = 0
                start = now
                decodeDurationMax = 0
            }
        }
    }
}
