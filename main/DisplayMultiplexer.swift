fileprivate let Log = Logger(tag: "DisplayMultiplexer")

enum DisplayMultiplexer {

    private static let size = Size(width: 360, height: 640)
    private(set) static var srm: IDF.PPAClient!
    private(set) static var fill: IDF.PPAClient!
    private(set) static var clear: ((Int) -> ())!
    private(set) static var flush: ((Int) -> ())!
    private(set) static var colorSpace: ColorSpace!
    private static var srmColorMode: IDF.PPAClient.SRMColorMode { colorSpace == .rgb888 ? .rgb888 : .rgb565 }
    private static var fillColorMode: IDF.PPAClient.FillColorMode { colorSpace == .rgb888 ? .rgb888 : .rgb565 }
    private(set) static var frameBuffers: [UnsafeMutableRawBufferPointer]!
    private static var getTouchPoint: (() -> Point?)!
    private static var setBrightness: ((Int) -> ())!

    enum Mode {
        case fileManager
        case videoPlayer(view: AnyObject)

        fileprivate struct Config {
            let uiRegions: [(yOffset: Int, height: Int)]
            let canHideControl: Bool
            let autoRefresh: Bool
            let jpegDecoder: Bool
        }
        fileprivate var config: Config {
            switch self {
            case .fileManager:
                Config(
                    uiRegions: [(yOffset: 0, height: size.height)],
                    canHideControl: false,
                    autoRefresh: true,
                    jpegDecoder: false,
                )
            case .videoPlayer:
                Config(
                    uiRegions: [(yOffset: 0, height: 60), (yOffset: size.height - 162, height: 162)],
                    canHideControl: true,
                    autoRefresh: false,
                    jpegDecoder: true,
                )
            }
        }
    }
    static private(set) var mode: Mode = .fileManager
    static func change(mode: Mode, screen: LVGL.Screen) {
        if Self.mode.config.jpegDecoder { stopJpegDecoderTask() }
        Self.mode = mode
        clear(0)
        screen.load()
        if mode.config.jpegDecoder { startJpegDecoderTask() }
        if mode.config.autoRefresh { drawFrameBuffer() }
    }

    static private var screenStack: [(mode: Mode, screen: LVGL.Screen)] = []
    static func push(mode: Mode, screen: LVGL.Screen) {
        screenStack.append((mode: Self.mode, screen: LVGL.Screen.active))
        change(mode: mode, screen: screen)
    }
    static func pop() {
        let last = screenStack.removeLast()
        change(mode: last.mode, screen: last.screen)
    }

    private static var buffer: UnsafeMutableBufferPointer<lv_color_t>!
    private static var lvglDisplay: LVGL.Display!
    static var showControl = true

    static func configure(
        clear: @escaping (Int) -> (),
        flush: @escaping (Int) -> (),
        colorSpace: ColorSpace,
        frameBuffers: [UnsafeMutableRawBufferPointer],
        getTouchPoint: @escaping (() -> Point?),
        setBrightness: @escaping ((Int) -> ()),
    ) throws(IDF.Error) {
        Self.srm = try IDF.PPAClient(operType: .srm)
        Self.fill = try IDF.PPAClient(operType: .fill)
        Self.clear = clear
        Self.flush = flush
        Self.colorSpace = colorSpace
        Self.frameBuffers = frameBuffers
        Self.getTouchPoint = getTouchPoint
        Self.setBrightness = setBrightness
        setBrightness(brightness)
        Self.workFrameBuffer = IDF.JPEG.Decoder.allocateOutputBuffer(size: 1280 * 720 * (colorSpace == .rgb888 ? 3 : 2))

        buffer = Memory.allocate(type: lv_color_t.self, capacity: size.area, capability: .spiram)!
        lvglDisplay = LVGL.Display.createDirectBufferDisplay(buffer: buffer.baseAddress, size: size) { display, pixels in
            if mode.config.autoRefresh {
                drawFrameBuffer()
            }
            display.flushReady()
        }

        let touch = TouchStateMachine()
        touch.onEvent { event in
            guard case .tap(_) = event else { return }
            Self.showControl = !Self.showControl
            print("Control Visible: \(self.showControl)")
        }
        _ = LVGL.Indev.createPollingPointerDevice { indev, data in
            guard let point = getTouchPoint() else {
                data.pointee.state = .released
                touch.onTouch(coordinates: [])
                return
            }

            let x = point.x / 2, y = point.y / 2
            if showControl || !mode.config.canHideControl {
                for region in mode.config.uiRegions {
                    if region.yOffset <= y && y < region.yOffset + region.height {
                        data.pointee.point.x = Int32(x)
                        data.pointee.point.y = Int32(y)
                        data.pointee.state = .pressed
                        return
                    }
                }
            }
            touch.onTouch(coordinates: [point])
        }
    }

    private static func drawFrameBuffer(fbNum: Int = 0, flush: Bool = true) {
        for region in mode.config.uiRegions {
            let inputRect = Rect(x: 0, y: region.yOffset, width: size.width, height: region.height)
            let outputRect = Rect(x: 0, y: inputRect.origin.y * 2, width: inputRect.width * 2, height: inputRect.height * 2)
            try? self.srm.srm(
                input: (buffer: UnsafeRawBufferPointer(buffer), size: size, block: inputRect, colorMode: .rgb565),
                output: (buffer: frameBuffers[fbNum], size: Size(width: 720, height: 1280), block: outputRect, colorMode: srmColorMode),
            )
        }
        if flush {
            Self.flush(fbNum)
        }
    }

    private static var jpegDecoder: (
        queue: Queue<UnsafeRawBufferPointer>,
        shouldStop: Bool,
    )?

    enum JpegDecoderMode {
        case direct
        case aspectFitRotate(size: Size)
    }
    static var workFrameBuffer: UnsafeMutableRawBufferPointer!
    static var transformImage: ((Int) -> ())?
    static var clearPadding: ((Int) -> ())?
    static var jpegDecoderMode: JpegDecoderMode = .direct {
        didSet {
            switch jpegDecoderMode {
            case .aspectFitRotate(let size):
                let offset: Point
                if size.width < size.height { // scale only
                    let scale = min(720 / Float(size.width), 1280 / Float(size.height))
                    offset = Point(x: Int(720 - Float(size.width) * scale) / 2, y: Int(1280 - Float(size.height) * scale) / 2)
                    transformImage = {
                        try? self.srm.srm(
                            input: (buffer: UnsafeRawBufferPointer(workFrameBuffer), size: size, block: nil, colorMode: srmColorMode),
                            output: (buffer: frameBuffers[$0], size: Self.size, offset: offset, scale: scale, colorMode: srmColorMode)
                        )
                    }
                } else { // scale & rotate
                    let scale = min(720 / Float(size.height), 1280 / Float(size.width))
                    offset = Point(x: Int(720 - Float(size.height) * scale) / 2, y: Int(1280 - Float(size.width) * scale) / 2)
                    transformImage = {
                        try? self.srm.srm(
                            input: (buffer: UnsafeRawBufferPointer(workFrameBuffer), size: size, block: nil, colorMode: srmColorMode),
                            output: (buffer: frameBuffers[$0], size: Size(width: 720, height: 1280), offset: offset, scale: scale, colorMode: srmColorMode),
                            rotate: 90
                        )
                    }
                }
                if offset.x > 0 {
                    clearPadding = {
                        try? self.fill.fill(
                            output: (buffer: frameBuffers[$0], size: Size(width: 720, height: 1280), colorMode: fillColorMode),
                            rect: Rect(x: 0, y: 0, width: offset.x, height: 1280), color: .black
                        )
                        try? self.fill.fill(
                            output: (buffer: frameBuffers[$0], size: Size(width: 720, height: 1280), colorMode: fillColorMode),
                            rect: Rect(x: 720 - offset.x, y: 0, width: offset.x, height: 1280), color: .black
                        )
                    }
                } else {
                    clearPadding = {
                        try? self.fill.fill(
                            output: (buffer: frameBuffers[$0], size: Size(width: 720, height: 1280), colorMode: fillColorMode),
                            rect: Rect(x: 0, y: 0, width: 720, height: offset.y), color: .black
                        )
                        try? self.fill.fill(
                            output: (buffer: frameBuffers[$0], size: Size(width: 720, height: 1280), colorMode: fillColorMode),
                            rect: Rect(x: 0, y: 1280 - offset.y, width: 720, height: offset.y), color: .black
                        )
                    }
                }
            default:
                transformImage = nil
                clearPadding = nil
            }
        }
    }

    static func drawJpeg(data: UnsafeRawBufferPointer) {
        jpegDecoder?.queue.overwrite(data)
    }
    private static func startJpegDecoderTask() {
        let queue = Queue<UnsafeRawBufferPointer>(capacity: 1)!
        jpegDecoder = (queue: queue, shouldStop: false)
        Task(name: "JPEG", priority: 15) { _ in
            Log.info("JPEG Task Start")
            try! jpegDecoderTask(queue: queue)
            self.jpegDecoder = nil
            Log.info("JPEG Task End")
        }
    }
    private static func stopJpegDecoderTask() {
        jpegDecoder?.shouldStop = true
        while jpegDecoder != nil { Task.delay(1) }
    }
    private static func jpegDecoderTask(queue: Queue<UnsafeRawBufferPointer>) throws(IDF.Error) {
        let decoder = try IDF.JPEG.Decoder(outputFormat: colorSpace == .rgb888 ? .rgb888(elementOrder: .bgr, conversion: .bt601) : .rgb565(elementOrder: .bgr, conversion: .bt601))
        let timer = try IDF.GeneralPurposeTimer(resolutionHz: 1000 * 1000)
        var frameBufferIndex = 0
        var frameCount = 0
        var start = timer.count
        var decodeDurationMax: UInt64 = 0
        var lastJpegBuffer: UnsafeRawBufferPointer?
        var prevControlVisible = false
        while true {
            if jpegDecoder?.shouldStop == true { return }
            let jpegData: UnsafeRawBufferPointer
            if showControl {
                prevControlVisible = true
                if let recv = queue.receive(timeout: 4) {
                    jpegData = recv
                } else {
                    LVGL.withLock { drawFrameBuffer(fbNum: frameBufferIndex) }
                    continue
                }
            } else {
                if prevControlVisible {
                    prevControlVisible = false
                    for i in 0..<frameBuffers.count { clearPadding?(i) }
                    if let recv = queue.receive(timeout: 10) {
                        jpegData = recv
                    } else if let last = lastJpegBuffer {
                        jpegData = last
                    } else {
                        clear(frameBufferIndex)
                        flush(frameBufferIndex)
                        continue
                    }
                } else if let recv = queue.receive(timeout: 10) {
                    jpegData = recv
                } else {
                    continue
                }
            }

            let nextFrameBufferIndex = (frameBufferIndex + 1) % frameBuffers.count
            let decodeStart = timer.count
            do {
                try decoder.decode(inputBuffer: jpegData, outputBuffer: transformImage != nil ? workFrameBuffer : frameBuffers[nextFrameBufferIndex])
                transformImage?(nextFrameBufferIndex)
            } catch {
                continue
            }
            let decodeDuration = timer.duration(from: decodeStart)
            if decodeDuration > decodeDurationMax { decodeDurationMax = decodeDuration }

            if showControl {
                LVGL.withLock { drawFrameBuffer(fbNum: nextFrameBufferIndex) }
            } else {
                flush(nextFrameBufferIndex)
            }
            frameBufferIndex = nextFrameBufferIndex
            lastJpegBuffer = jpegData

            frameCount += 1
            let now = timer.count
            if (now - start) >= 1000000 {
                Log.info("\(frameCount)fps, decode(worst): \(decodeDurationMax)us")
                frameCount = 0
                start = now
                decodeDurationMax = 0
            }
        }
    }

    static var brightness: Int = 50 {
        didSet {
            setBrightness(brightness)
        }
    }
}
