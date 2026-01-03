fileprivate let Log = Logger(tag: "AVI")

class AVIPlayer {

    var decoder: AudioDecoder
    private var dmux: OpaquePointer?
    var videoBuffer: UnsafeMutableBufferPointer<UInt8>?
    var audioBuffer: UnsafeMutableBufferPointer<UInt8>?

    init() throws(IDF.Error) {
        decoder = try AudioDecoder(type: .mp3)

    }

    func open(file: String) -> avi_dmux_info_t? {
        dmux = file.utf8CString.withUnsafeBufferPointer {
            avi_dmux_create($0.baseAddress!)
        }
        if let d = dmux, let info = avi_dmux_parse_info(d) {
            return info.pointee
        }
        close()
        return nil
    }
    func close() {
        if let d = dmux {
            avi_dmux_delete(d)
        }
    }

    func readFrame() -> avi_dmux_frame_t? {
        var frame = avi_dmux_frame_t()
        let result = avi_dmux_read_frame(
            dmux, &frame,
            videoBuffer?.baseAddress, UInt32(videoBuffer?.count ?? 0),
            audioBuffer?.baseAddress, UInt32(audioBuffer?.count ?? 0)
        )
        return result ? frame : nil
    }
}
