fileprivate let Log = Logger(tag: "main")

@_cdecl("app_main")
func app_main() {
    do {
        try main(pixelFormat: RGB888.self)
    } catch {
        Log.error("Main Function Exit with Error: \(error)")
    }
}
func main<PixelFormat: Pixel>(pixelFormat: PixelFormat.Type) throws(IDF.Error) {
    let tab5 = try M5StackTab5.begin(
        pixelFormat: PixelFormat.self,
        frameBufferNum: 3,
        usbHost: true,
    )
    try LVGL.begin()
    let displayMux = try DisplayMultiplexer<PixelFormat>(tab5: tab5)

    let usbHost = USBHost()
    let mscDriver = USBHost.MSC()
    try usbHost.install()
    try mscDriver.install(taskStackSize: 4096, taskPriority: 5, xCoreID: 0, createBackgroundTask: true)
    Task.delay(1000)

    VideoPlayerView.config = (
        setPlayerDisplay: { displayMux.mode = $0 ? .player : .fileManager },
        outputFormat: PixelFormat.self == RGB888.self ?
            .rgb888(elementOrder: .bgr, conversion: .bt601) : .rgb565(elementOrder: .bgr, conversion: .bt601),
        frameBuffers: tab5.display.frameBuffers.map {
            UnsafeMutableRawBufferPointer(start: $0.baseAddress!, count: $0.count * MemoryLayout<PixelFormat>.size)
        },
        flush: { tab5.display.flush(fbNum: $0) },
        audioSetClock: {
            Log.info("Audio Clock: \($0)Hz, \($1)-bit, \($2) channels")
            try? tab5.audio.reconfigOutput(rate: $0, bps: $1, ch: $2)
        },
        audioWrite: { try? tab5.audio.write($0) },
    )
    LVGL.asyncCall {
        StorageSelectView.create(
            mountSdcard: { path, maxFiles throws(IDF.Error) in try tab5.sdcard.mount(path: path, maxFiles: maxFiles) },
            mountUsbDrive: { path, maxFiles throws(IDF.Error) in try mscDriver.mount(path: path, maxFiles: maxFiles) }
        )
    }

    tab5.display.brightness = 100
    tab5.audio.volume = 20
}

fileprivate final class DisplayMultiplexer<PixelFormat: Pixel> {

    let tab5: M5StackTab5<PixelFormat>
    let ppa: IDF.PPAClient

    enum Mode {
        case fileManager
        case player
    }
    var mode: Mode = .fileManager {
        didSet { modeChanged(from: oldValue, to: mode) }
    }

    struct Display {
        let size: Size
        let buffer: UnsafeMutableBufferPointer<lv_color_t>
        var lvglDisplay: LVGL.Display!
        var showControl = false

        init(size: Size) {
            self.size = size
            self.buffer = Memory.allocate(type: lv_color_t.self, capacity: size.area, capability: .spiram)!
        }
    }
    var fileManager: Display!
    var player: Display!

    init(tab5: M5StackTab5<PixelFormat>) throws(IDF.Error) {
        self.tab5 = tab5
        self.ppa = try IDF.PPAClient(operType: .srm)

        // Create File Manager Display
        var fileManager = Display(size: Size(width: 720 / 2, height: 1280 / 2))
        fileManager.lvglDisplay = LVGL.Display.createDirectBufferDisplay(
            buffer: fileManager.buffer.baseAddress,
            size: fileManager.size
        ) { display, pixels in
            if self.mode == .fileManager {
                self.drawFileManagerDisplay()
            }
            display.flushReady()
        }
        fileManager.lvglDisplay.setDefault()
        self.fileManager = fileManager

        // Create Player Display
        var player = Display(size: Size(width: 720 / 2, height: 640 / 2))
        player.lvglDisplay = LVGL.Display.createDirectBufferDisplay(
            buffer: player.buffer.baseAddress,
            size: player.size
        ) { display, pixels in
            display.flushReady()
        }
        self.player = player

        // Input Device
        let _ = LVGL.Indev.createPollingPointerDevice { indev, data in
            if let point = (try? tab5.touch.coordinates)?.first {
                data.pointee.point.x = Int32(point.x / 2)
                data.pointee.point.y = Int32(point.y / 2)
                data.pointee.state = .pressed
            } else {
                data.pointee.state = .released
            }
        }
    }

    func drawFileManagerDisplay() {
        let colorMode: IDF.PPAClient.SRMColorMode = PixelFormat.self == RGB565.self ? .rgb565 : .rgb888
        try? self.ppa.srm(
            input: (buffer: UnsafeRawBufferPointer(start: fileManager.buffer.baseAddress, count: fileManager.buffer.count * MemoryLayout<lv_color_t>.size), size: fileManager.size, block: nil, colorMode: .rgb565),
            output: (buffer: UnsafeMutableRawBufferPointer(self.tab5.display.frameBuffers[0]), size: Size(width: 720, height: 1280), block: nil, colorMode: colorMode),
        )
        self.tab5.display.flush(fbNum: 0)
    }

    private func modeChanged(from: Mode, to: Mode) {
        if to == .player {
            tab5.display.frameBuffers[0].initialize(repeating: .black)
            tab5.display.flush(fbNum: 0)
        } else {
            drawFileManagerDisplay()
        }
    }
}
