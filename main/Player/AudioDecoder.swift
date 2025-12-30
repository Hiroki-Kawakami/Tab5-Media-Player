fileprivate let Log = Logger(tag: "AudioDecoder")

final class AudioDecoder {
    let type: esp_audio_type_t
    var pcmBuffer: UnsafeMutableRawBufferPointer
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
        pcmBuffer = Memory.allocateRaw(size: 32 * 1024, capability: .spiram)!
        decoder = audioDecoder
    }

    func decode(
        buffer: UnsafeMutableBufferPointer<UInt8>,
        frameRecover: esp_audio_dec_recovery_t = .plc,
        callback: (UnsafeMutableRawBufferPointer) -> ()
    ) {
        var rawInput = esp_audio_dec_in_raw_t()
        rawInput.buffer = buffer.baseAddress
        rawInput.len = UInt32(buffer.count)
        rawInput.frame_recover = frameRecover
        var frameOutput = esp_audio_dec_out_frame_t()
        frameOutput.buffer = pcmBuffer.assumingMemoryBound(to: UInt8.self).baseAddress!
        frameOutput.len = UInt32(pcmBuffer.count)

        let err = esp_audio_dec_process(decoder, &rawInput, &frameOutput)
        if err != ESP_AUDIO_ERR_OK {
            Log.error("Audio decode error: \(err)")
            return
        }

        let decodedBuffer = UnsafeMutableRawBufferPointer(
            start: pcmBuffer.baseAddress!,
            count: Int(frameOutput.decoded_size)
        )
        callback(decodedBuffer)
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
