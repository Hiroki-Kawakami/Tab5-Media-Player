fileprivate let Log = Logger(tag: "DisplayMultiplexer")

enum DisplayMultiplexer {

    private static let size = Size(width: 360, height: 640)
    private(set) static var ppa: IDF.PPAClient!
    private(set) static var clear: ((Int) -> ())!
    private(set) static var flush: ((Int) -> ())!
    private(set) static var colorSpace: ColorSpace!
    private(set) static var frameBuffers: [UnsafeMutableRawBufferPointer]!
    private static var getTouchPoint: (() -> Point?)!

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
                    uiRegions: [(yOffset: 0, height: 60), (yOffset: size.height - 150, height: 150)],
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
    ) throws(IDF.Error) {
        Self.ppa = try IDF.PPAClient(operType: .srm)
        Self.clear = clear
        Self.flush = flush
        Self.colorSpace = colorSpace
        Self.frameBuffers = frameBuffers
        Self.getTouchPoint = getTouchPoint

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
        let colorMode: IDF.PPAClient.SRMColorMode = colorSpace == .rgb888 ? .rgb888 : .rgb565
        for region in mode.config.uiRegions {
            let inputRect = Rect(x: 0, y: region.yOffset, width: size.width, height: region.height)
            let outputRect = Rect(x: 0, y: inputRect.origin.y * 2, width: inputRect.width * 2, height: inputRect.height * 2)
            try? self.ppa.srm(
                input: (buffer: UnsafeRawBufferPointer(buffer), size: size, block: inputRect, colorMode: .rgb565),
                output: (buffer: frameBuffers[fbNum], size: Size(width: 720, height: 1280), block: outputRect, colorMode: colorMode),
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
                if let recv = queue.receive(timeout: 10) {
                    jpegData = recv
                } else if prevControlVisible == true {
                    prevControlVisible = false
                    if let last = lastJpegBuffer {
                        jpegData = last
                    } else {
                        clear(frameBufferIndex)
                        flush(frameBufferIndex)
                        continue
                    }
                } else {
                    continue
                }
            }

            let nextFrameBufferIndex = (frameBufferIndex + 1) % frameBuffers.count
            let decodeStart = timer.count
            do {
                try decoder.decode(inputBuffer: jpegData, outputBuffer: frameBuffers[nextFrameBufferIndex])
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
}
