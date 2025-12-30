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
    )
    try LVGL.begin()

    let ppa = try IDF.PPAClient(operType: .srm)
    let guiSize = Size(width: 720 / 2, height: 1280 / 2)
    let guiBuffer = Memory.allocate(type: lv_color_t.self, capacity: guiSize.area, capability: .spiram)!
    let display = LVGL.Display.createDirectBufferDisplay(
        buffer: guiBuffer.baseAddress,
        size: guiSize
    ) { display, buffer in
        let colorMode: IDF.PPAClient.SRMColorMode = PixelFormat.self == RGB565.self ? .rgb565 : .rgb888
        try? ppa.srm(
            input: (buffer: UnsafeRawBufferPointer(start: guiBuffer.baseAddress, count: guiBuffer.count), size: guiSize, block: nil, colorMode: .rgb565),
            output: (buffer: UnsafeMutableRawBufferPointer(tab5.display.frameBuffers[0]), size: Size(width: 720, height: 1280), block: nil, colorMode: colorMode),
        )
        tab5.display.flush(fbNum: 0)
        display.flushReady()
    }
    display.setDefault()
    let _ = LVGL.Indev.createPollingPointerDevice { indev, data in
        if let point = (try? tab5.touch.coordinates)?.first {
            data.pointee.point.x = Int32(point.x / 2)
            data.pointee.point.y = Int32(point.y / 2)
            data.pointee.state = .pressed
        } else {
            data.pointee.state = .released
        }
    }

    LVGL.asyncCall {
        StorageSelectView.create(
            mountSdcard: { path, maxFiles throws(IDF.Error) in try tab5.sdcard.mount(path: path, maxFiles: maxFiles) }
        )
    }

    tab5.display.brightness = 100

    // let tab5 = try M5StackTab5.begin()
    // let frameBuffer = tab5.display.frameBuffer
    // tab5.display.brightness = 100

    // let multiTouch: MultiTouch = MultiTouch()
    // multiTouch.task(xCoreID: 1) {
    //     tab5.touch.waitInterrupt()
    //     return try! tab5.touch.coordinates
    // }

    // let fontPartition = IDF.Partition(type: 0x40, subtype: 0)!
    // PixelWriter.defaultFont = Font(from: fontPartition)!

    // let usbHost = USBHost()
    // let mscDriver = USBHost.MSC()
    // try usbHost.install()
    // try mscDriver.install(taskStackSize: 4096, taskPriority: 5, xCoreID: 0, createBackgroundTask: true)
    // Task.delay(1000)
    // var mountPoint = ""
    // while true {
    //     do throws(IDF.Error) {
    //         try mscDriver.mount(path: "/usb", maxFiles: 25)
    //         mountPoint = "usb"
    //         break
    //     } catch {
    //         Log.error("Failed to mount USB storage: \(error)")
    //     }
    //     do throws(IDF.Error) {
    //         try tab5.sdcard.mount(path: "/sdcard", maxFiles: 25)
    //         mountPoint = "sdcard"
    //         break
    //     } catch {
    //         Log.error("Failed to mount SD card: \(error)")
    //     }

    //     let writer = PixelWriter(buffer: frameBuffer, screenSize: tab5.display.size)
    //     writer.drawText("Storage not found.", at: Point(x: 40, y: 40), fontSize: 54, color: .white)
    //     writer.drawText("Please insert USB or", at: Point(x: 40, y: 114), fontSize: 54, color: .white)
    //     writer.drawText("SD card.", at: Point(x: 40, y: 188), fontSize: 54, color: .white)
    //     tab5.display.drawBitmap(rect: Rect(origin: .zero, size: tab5.display.size), data: frameBuffer.baseAddress!)

    //     Task.delay(1000)
    //     Log.info("Retry mounting storage...")
    // }

    // let fileManagerView = FileManagerView(size: tab5.display.size)
    // fileManagerView.push(path: "", name: mountPoint)

    // let aviPlayer = try AVIPlayer()
    // let aviPlayerSemaphore = Semaphore.createBinary()!
    // var showControls = false
    // let videoBufferTx = Queue<(UnsafeMutableBufferPointer<UInt8>, Int, Size)>(capacity: 4)!
    // aviPlayer.onVideoData { buffer, bufferSize, frameSize in
    //     videoBufferTx.send((buffer, bufferSize, frameSize))
    //     return false
    // }
    // aviPlayer.onAudioData { buffer in
    //     try! tab5.audio.write(buffer)
    // }
    // aviPlayer.onAudioSetClock { sampleRate, bitsPerSample, channels in
    //     Log.info("Audio Clock: \(sampleRate)Hz, \(bitsPerSample)-bit, \(channels) channels")
    //     try! tab5.audio.reconfigOutput(rate: sampleRate, bps: bitsPerSample, ch: channels)
    // }
    // aviPlayer.onPlayEnd {
    //     aviPlayerSemaphore.give()
    // }
    // Task(name: "MJpegDecoder", priority: 15, xCoreID: 1) { _ in
    //     var lastTick: UInt32? = nil
    //     var frameCount = 0
    //     let videoDecoder = try! IDF.JPEG.createDecoderRgb565(rgbElementOrder: .bgr, rgbConversion: .bt709)
    //     let decodeBuffer1 = IDF.JPEG.Decoder<UInt16>.allocateOutputBuffer(capacity: tab5.display.size.width * tab5.display.size.height)!
    //     let decodeBuffer2 = IDF.JPEG.Decoder<UInt16>.allocateOutputBuffer(capacity: tab5.display.size.width * tab5.display.size.height)!
    //     let videoBuffer1 = IDF.JPEG.Decoder<UInt16>.allocateOutputBuffer(capacity: tab5.display.size.width * tab5.display.size.height)!
    //     let videoBuffer2 = IDF.JPEG.Decoder<UInt16>.allocateOutputBuffer(capacity: tab5.display.size.width * tab5.display.size.height)!
    //     let ppa = try! IDF.PPAClient(operType: .srm)
    //     var bufferToggle = false
    //     for (buffer, bufferSize, frameSize) in videoBufferTx {
    //         if frameSize.width * frameSize.height > 720 * 1280 {
    //             Log.error("Received video frame larger than 720x1280: \(frameSize.width)x\(frameSize.height)")
    //             continue
    //         }

    //         let inputBuffer = UnsafeRawBufferPointer(
    //             start: buffer.baseAddress!,
    //             count: bufferSize
    //         )

    //         frameCount += 1
    //         if let _lastTick = lastTick {
    //             let currentTick = Task.tickCount
    //             let elapsed = currentTick - _lastTick
    //             if elapsed >= Task.ticks(1000) {
    //                 Log.info("FPS: \(frameCount)")
    //                 frameCount = 0
    //                 lastTick = currentTick
    //             }
    //         } else {
    //             frameCount = 0
    //             lastTick = Task.tickCount
    //         }

    //         do throws(IDF.Error) {
    //             let decodeBuffer = bufferToggle ? decodeBuffer1 : decodeBuffer2
    //             let videoBuffer = bufferToggle ? videoBuffer1 : videoBuffer2
    //             let _ = try videoDecoder.decode(inputBuffer: inputBuffer, outputBuffer: decodeBuffer)
    //             let draw = { () throws(IDF.Error) in
    //                 let size = showControls ? Size(width: 720, height: 1280 - 300) : tab5.display.size
    //                 if frameSize.width == 720 && frameSize.height == 1280 {
    //                     tab5.display.drawBitmap(rect: Rect(origin: .zero, size: size), data: decodeBuffer.baseAddress!, retry: false)
    //                 } else {
    //                     try ppa.fitScreen(
    //                         inputBuffer: decodeBuffer,
    //                         inputSize: frameSize,
    //                         outputBuffer: videoBuffer,
    //                         outputSize: tab5.display.size
    //                     )
    //                     tab5.display.drawBitmap(rect: Rect(origin: .zero, size: size), data: videoBuffer.baseAddress!)
    //                 }
    //             }
    //             try draw()
    //             while aviPlayer.isPaused {
    //                 Task.delay(100)
    //                 try draw()
    //             }
    //             bufferToggle.toggle()
    //         } catch {
    //             Log.error("Failed to decode video frame: \(error)")
    //         }
    //         aviPlayer.returnVideoBuffer(buffer)
    //     }
    // }

    // let rect = Rect(x: 0, y: 1280 - 300, width: 720, height: 300)
    // let playerControlView = PlayerControlView(size: rect.size)

    // var selectedFile: String? = nil
    // multiTouch.onEvent { event in
    //     guard case .tap(let point) = event else { return }
    //     if aviPlayer.isPlaying {
    //         if !showControls || point.y < 1280 - 300 {
    //             showControls.toggle()
    //             Task.delay(30)
    //         } else {
    //             let point = Point(x: point.x, y: point.y - (1280 - 300))
    //             let controlEvent = playerControlView.onTap(point: point)
    //             switch controlEvent {
    //             case .close:
    //                 try? aviPlayer.stop()
    //                 // aviPlayerSemaphore.give()
    //             case .playPause:
    //                 if aviPlayer.isPaused {
    //                     aviPlayer.resume()
    //                 } else {
    //                     aviPlayer.pause()
    //                 }
    //             case .volume(let diff):
    //                 tab5.audio.volume = max(0, min(100, tab5.audio.volume + diff))
    //             case .brightness(let diff):
    //                 tab5.display.brightness = max(10, min(100, tab5.display.brightness + diff))
    //             default:
    //                 break
    //             }
    //         }
    //         if showControls {
    //             let buffer = playerControlView.draw(
    //                 pause: !aviPlayer.isPaused, volume: tab5.audio.volume, brightness: tab5.display.brightness
    //             )
    //             tab5.display.drawBitmap(rect: rect, data: buffer.baseAddress!)
    //         }
    //     } else {
    //         let (refresh, file) = fileManagerView.onTouch(event: event)
    //         if refresh {
    //             let buffer = fileManagerView.draw()
    //             tab5.display.drawBitmap(rect: Rect(origin: .zero, size: fileManagerView.size), data: buffer.baseAddress!)
    //         }
    //         if let file = file {
    //             selectedFile = file
    //         }
    //     }
    // }

    // tab5.audio.volume = 40
    // while true {
    //     let buffer = fileManagerView.draw()
    //     tab5.display.drawBitmap(rect: Rect(origin: .zero, size: fileManagerView.size), data: buffer.baseAddress!)

    //     var playSucceed = false
    //     while true {
    //         if let file = selectedFile {
    //             Log.info("Selected file: \(file)")
    //             showControls = false
    //             memset(frameBuffer.baseAddress!, 0, tab5.display.size.width * tab5.display.size.height * 2)
    //             tab5.display.drawBitmap(rect: Rect(origin: .zero, size: tab5.display.size), data: frameBuffer.baseAddress!)
    //             do {
    //                 try aviPlayer.play(file: file)
    //                 playSucceed = true
    //             } catch {
    //                 Log.error("Failed to play video: \(error)")
    //             }
    //             selectedFile = nil
    //             break
    //         }
    //         Task.delay(10)
    //     }
    //     if !playSucceed {
    //         continue
    //     }
    //     aviPlayerSemaphore.take()
    // }
}

// class PlayerControlView {
//     let writer: PixelWriter
//     var buffer: UnsafeMutableBufferPointer<UInt16> {
//         return writer.buffer
//     }
//     var size: Size {
//         return writer.screenSize
//     }

//     struct Icon {
//         let offset: Point
//         let icon: (size: Size, bitmap: [UInt32])

//         func rect(margin: Int = 0) -> Rect {
//             return Rect(
//                 origin: Point(x: offset.x - margin, y: offset.y - margin),
//                 size: Size(width: icon.size.width + margin * 2, height: icon.size.height + margin * 2)
//             )
//         }

//         init(center: Point, icon: (size: Size, bitmap: [UInt32])) {
//             self.offset = Point(x: center.x - icon.size.width / 2, y: center.y - icon.size.height / 2)
//             self.icon = icon
//         }
//     }

//     enum Event {
//         case close
//         case playPause
//         case volume(diff: Int)
//         case brightness(diff: Int)
//     }

//     private let closeButton = Icon(center: Point(x: 90, y: 150), icon: Icons.close)
//     private let playButton = Icon(center: Point(x: 215, y: 150), icon: Icons.play)
//     private let pauseButton = Icon(center: Point(x: 211, y: 150), icon: Icons.pause)
//     private let volMinusButton = Icon(center: Point(x: 355, y: 85), icon: Icons.minus)
//     private let volPlusButton = Icon(center: Point(x: 655, y: 85), icon: Icons.plus)
//     private let volIcon = Icon(center: Point(x: 430, y: 85), icon: Icons.speaker)
//     private let briMinusButton = Icon(center: Point(x: 355, y: 215), icon: Icons.minus)
//     private let briPlusButton = Icon(center: Point(x: 655, y: 215), icon: Icons.plus)
//     private let briIcon = Icon(center: Point(x: 433, y: 215), icon: Icons.light)

//     init(size: Size) {
//         let buffer = UnsafeMutableBufferPointer<UInt16>.allocate(capacity: size.width * size.height)
//         self.writer = PixelWriter(buffer: buffer, screenSize: size)
//     }

//     func draw(pause: Bool, volume: Int, brightness: Int) -> UnsafeMutableBufferPointer<UInt16> {
//         writer.clear(color: .black)
//         writer.drawLine(from: .zero, to: Point(x: size.width - 1, y: 0), color: .white)

//         // Draw icons
//         writer.drawBitmap(closeButton.icon, at: closeButton.offset, color: .white)
//         if pause {
//             writer.drawBitmap(pauseButton.icon, at: pauseButton.offset, color: .white)
//         } else {
//             writer.drawBitmap(playButton.icon, at: playButton.offset, color: .white)
//         }
//         writer.drawBitmap(volMinusButton.icon, at: volMinusButton.offset, color: .white)
//         writer.drawBitmap(volPlusButton.icon, at: volPlusButton.offset, color: .white)
//         writer.drawBitmap(volIcon.icon, at: volIcon.offset, color: .white)
//         writer.drawBitmap(briMinusButton.icon, at: briMinusButton.offset, color: .white)
//         writer.drawBitmap(briPlusButton.icon, at: briPlusButton.offset, color: .white)
//         writer.drawBitmap(briIcon.icon, at: briIcon.offset, color: .white)

//         let fontSize = 60
//         let volumeText = "\(volume)"
//         let volumeWidth = PixelWriter.defaultFont!.width(of: volumeText, fontSize: fontSize)
//         writer.drawText("\(volume)",
//             at: Point(x: 545 - volumeWidth / 2, y: 85 - fontSize / 2),
//             fontSize: fontSize,
//             color: .white
//         )
//         let brightnessText = "\(brightness)"
//         let brightnessWidth = PixelWriter.defaultFont!.width(of: brightnessText, fontSize: fontSize)
//         writer.drawText("\(brightness)",
//             at: Point(x: 545 - brightnessWidth / 2, y: 215 - fontSize / 2),
//             fontSize: fontSize,
//             color: .white
//         )
//         return buffer
//     }

//     func onTap(point: Point) -> Event? {
//         let margin = 20
//         if closeButton.rect(margin: margin).contains(point) {
//             return .close
//         } else if playButton.rect(margin: margin).contains(point) {
//             return .playPause
//         } else if pauseButton.rect(margin: margin).contains(point) {
//             return .playPause
//         } else if volMinusButton.rect(margin: margin).contains(point) {
//             return .volume(diff: -10)
//         } else if volPlusButton.rect(margin: margin).contains(point) {
//             return .volume(diff: 10)
//         } else if briMinusButton.rect(margin: margin).contains(point) {
//             return .brightness(diff: -10)
//         } else if briPlusButton.rect(margin: margin).contains(point) {
//             return .brightness(diff: 10)
//         }
//         return nil
//     }
// }
