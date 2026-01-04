fileprivate let Log = Logger(tag: "AudioController")

final class AudioDecoder {
    let type: esp_audio_type_t
    private var decoder: esp_audio_dec_handle_t!

    init(type: esp_audio_type_t) throws(IDF.Error) {
        self.type = type
        switch type {
        case .mp3:
            esp_mp3_dec_register()
        default:
            Log.error("Unsupported Audio Codec: \(type)")
            fatalError()
        }

        var decoderConfig = esp_audio_dec_cfg_t()
        decoderConfig.type = type
        var audioDecoder: esp_audio_dec_handle_t?
        if esp_audio_dec_open(&decoderConfig, &audioDecoder) != ESP_AUDIO_ERR_OK {
            throw IDF.Error(ESP_FAIL)
        }
        decoder = audioDecoder
    }

    func close() {
        esp_audio_dec_close(decoder)
    }

    func decode(
        buffer: UnsafeMutableRawBufferPointer,
        output: UnsafeMutableBufferPointer<UInt8>,
        frameRecover: esp_audio_dec_recovery_t = .plc,
    ) -> Int {
        var rawInput = esp_audio_dec_in_raw_t()
        rawInput.buffer = buffer.assumingMemoryBound(to: UInt8.self).baseAddress
        rawInput.len = UInt32(buffer.count)
        rawInput.frame_recover = frameRecover
        var frameOutput = esp_audio_dec_out_frame_t()
        frameOutput.buffer = output.baseAddress
        frameOutput.len = UInt32(output.count)

        let err = esp_audio_dec_process(decoder, &rawInput, &frameOutput)
        if err != ESP_AUDIO_ERR_OK {
            Log.error("Audio decode error: \(err.rawValue)")
            return 0
        }

        return Int(frameOutput.decoded_size)
    }
}

extension esp_audio_type_t {
    static let unsupport = ESP_AUDIO_TYPE_UNSUPPORT
    static let amrnb = ESP_AUDIO_TYPE_AMRNB
    static let amrwb = ESP_AUDIO_TYPE_AMRWB
    static let aac = ESP_AUDIO_TYPE_AAC
    static let g771a = ESP_AUDIO_TYPE_G711A
    static let g771u = ESP_AUDIO_TYPE_G711U
    static let opus = ESP_AUDIO_TYPE_OPUS
    static let adpcm = ESP_AUDIO_TYPE_ADPCM
    static let pcm = ESP_AUDIO_TYPE_PCM
    static let flac = ESP_AUDIO_TYPE_FLAC
    static let vorbis = ESP_AUDIO_TYPE_VORBIS
    static let mp3 = ESP_AUDIO_TYPE_MP3
    static let alac = ESP_AUDIO_TYPE_ALAC
    static let sbc = ESP_AUDIO_TYPE_SBC
    static let lc3 = ESP_AUDIO_TYPE_LC3
    static let cutomized = ESP_AUDIO_TYPE_CUSTOMIZED
    static let cutomizedMax = ESP_AUDIO_TYPE_CUSTOMIZED_MAX
}

extension esp_audio_dec_recovery_t {
    static let none = ESP_AUDIO_DEC_RECOVERY_NONE
    static let plc = ESP_AUDIO_DEC_RECOVERY_PLC
}

enum AudioController {

    private static var open: ((UInt32, UInt8, UInt8) -> ())!
    private static var close: (() -> ())!
    private static var write: ((UnsafeMutableRawBufferPointer) -> ())!
    private static var audioBuffer: UnsafeMutableBufferPointer<UInt8>!
    private static var setVolume: ((Int) -> ())!

    static func configure(
        open: @escaping ((UInt32, UInt8, UInt8) -> ()),
        close: @escaping () -> (),
        write: @escaping (UnsafeMutableRawBufferPointer) -> (),
        setVolume: @escaping (Int) -> (),
    ) throws(IDF.Error) {
        Self.open = open
        Self.close = close
        Self.write = write
        Self.setVolume = setVolume
        audioBuffer = Memory.allocate(type: UInt8.self, capacity: 64 * 1024, capability: .spiram)
        setVolume(volume)
    }

    enum Codec {
        case pcm(rate: UInt32, bps: UInt8, ch: UInt8)
        case mp3(rate: UInt32, ch: UInt8)

        var rate: UInt32 {
            switch self {
            case .pcm(let rate, _, _): return rate
            case .mp3(let rate, _): return rate
            }
        }
        var bps: UInt8 {
            switch self {
            case .pcm(_, let bps, _): return bps
            default: return 16
            }
        }
        var ch: UInt8 {
            switch self {
            case .pcm(_, _, let ch): return ch
            case .mp3(_, let ch): return ch
            }
        }
        var audioType: esp_audio_type_t {
            switch self {
            case .pcm: .pcm
            case .mp3: .mp3
            }
        }
    }

    private static var decoder: AudioDecoder?
    static var codec: Codec? {
        didSet {
            close()
            decoder?.close()
            decoder = nil
            if let c = codec {
                if c.audioType != .pcm {
                    decoder = try? AudioDecoder(type: c.audioType)
                }
                open(c.rate, c.bps, c.ch)
            } else {
                open(48000, 16, 2) // default config
            }
        }
    }

    static func write(data: UnsafeMutableRawBufferPointer) {
        if let decoder = decoder {
            let size = decoder.decode(buffer: data, output: audioBuffer)
            write(UnsafeMutableRawBufferPointer(start: audioBuffer.baseAddress, count: size))
        } else {
            write(data)
        }
    }

    static var volume: Int = 50 {
        didSet {
            setVolume(volume)
        }
    }
}
