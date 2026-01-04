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
    try DisplayMultiplexer.configure(
        clear: { tab5.display.frameBuffers[$0].initialize(repeating: .black) },
        flush: { tab5.display.flush(fbNum: $0) },
        colorSpace: PixelFormat.self == RGB888.self ? .rgb888 : .rgb565,
        frameBuffers: tab5.display.frameBuffers.map { UnsafeMutableRawBufferPointer($0) },
        getTouchPoint: { (try? tab5.touch.coordinates)?.first }
    )
    try AudioController.configure(
        open: { try? tab5.audio.open(rate: $0, bps: $1, ch: $2) },
        close: { try? tab5.audio.close() },
        write: { try? tab5.audio.write($0) },
        setVolume: { tab5.audio.volume = $0 }
    )

    let usbHost = USBHost()
    let mscDriver = USBHost.MSC()
    try usbHost.install()
    try mscDriver.install(taskStackSize: 4096, taskPriority: 5, xCoreID: 0, createBackgroundTask: true)
    Task.delay(1000)

    LVGL.asyncCall {
        StorageSelectView.create(
            mountSdcard: { path, maxFiles throws(IDF.Error) in try tab5.sdcard.mount(path: path, maxFiles: maxFiles) },
            mountUsbDrive: { path, maxFiles throws(IDF.Error) in try mscDriver.mount(path: path, maxFiles: maxFiles) }
        )
    }

    tab5.display.brightness = 100
}
