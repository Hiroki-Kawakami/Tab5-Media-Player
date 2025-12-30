fileprivate let Log = Logger(tag: "AVI")

class AVIPlayer {

    var decoder: AudioDecoder

    private var videoDataCallback: ((UnsafeMutableBufferPointer<UInt8>, Size) -> Void)? = nil
    private var audioDataCallback: ((UnsafeMutableRawBufferPointer) -> Void)? = nil
    private var audioSetClockCallback: ((_ sampleRate: UInt32, _ bitsPerSample: UInt8, _ channels: UInt8) -> Void)? = nil
    private var aviPlayEndCallback: (() -> Void)? = nil

    private(set) var isPlaying = false
    private(set) var isPaused = false

    init() throws(IDF.Error) {
        decoder = try AudioDecoder(type: .mp3)

        var config = avi_player_config_t()
        config.buffer_size = 4192 * 1024
        config.video_cb = { (data, arg) in
            Unmanaged<AVIPlayer>.fromOpaque(arg!).takeUnretainedValue().videoCallback(data: data!)
        }
        config.audio_cb = { (data, arg) in
            Unmanaged<AVIPlayer>.fromOpaque(arg!).takeUnretainedValue().audioCallback(data: data!)
        }
        config.audio_set_clock_cb = { (rate, bits, ch, arg) in
            Unmanaged<AVIPlayer>.fromOpaque(arg!).takeUnretainedValue().audioSetClockCallback?(rate, UInt8(bits), UInt8(ch))
        }
        config.avi_play_end_cb = { arg in
            let player = Unmanaged<AVIPlayer>.fromOpaque(arg!).takeUnretainedValue()
            player.isPlaying = false
            player.aviPlayEndCallback?()
        }
        config.user_data = Unmanaged.passRetained(self).toOpaque()
        config.priority = 15
        try IDF.Error.check(avi_player_init(config))
    }

    private func videoCallback(data: UnsafeMutablePointer<frame_data_t>) {
        while isPaused { Task.delay(100) }
        guard let callback = videoDataCallback else { return }
        if data.pointee.video_info.frame_format != FORMAT_MJEPG {
            Log.error("Unsupported video format")
            return
        }
        let buffer = UnsafeMutableBufferPointer<UInt8>(start: data.pointee.data, count: data.pointee.data_bytes)
        callback(buffer, Size(width: Int(data.pointee.video_info.width), height: Int(data.pointee.video_info.height)))
    }

    private func audioCallback(data: UnsafeMutablePointer<frame_data_t>) {
        guard let callback = audioDataCallback else { return }
        let buffer = UnsafeMutableBufferPointer<UInt8>(start: data.pointee.data, count: data.pointee.data_bytes)
        decoder.decode(buffer: buffer, callback: callback)
    }

    func onVideoData(_ callback: ((UnsafeMutableBufferPointer<UInt8>, Size) -> Void)?) {
        self.videoDataCallback = callback
    }
    func onAudioData(_ callback: ((UnsafeMutableRawBufferPointer) -> Void)?) {
        self.audioDataCallback = callback
    }
    func onAudioSetClock(_ callback: ((_ sampleRate: UInt32, _ bitsPerSample: UInt8, _ channels: UInt8) -> Void)?) {
        self.audioSetClockCallback = callback
    }
    func onPlayEnd(_ callback: (() -> Void)?) {
        self.aviPlayEndCallback = callback
    }

    func play(file: String) throws(IDF.Error) {
        let err = file.utf8CString.withUnsafeBufferPointer {
            avi_player_play_from_file($0.baseAddress!)
        }
        try IDF.Error.check(err)
        isPaused = false
        isPlaying = true
    }

    func stop() throws(IDF.Error) {
        guard isPlaying else { return }
        isPlaying = false
        isPaused = false
        try IDF.Error.check(avi_player_play_stop())
    }

    func pause() {
        isPaused = true
    }

    func resume() {
        isPaused = false
    }
}
