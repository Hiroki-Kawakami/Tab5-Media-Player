class StorageBenchView {

    let screen: LVGL.Screen
    var fileList: LVGL.Dropdown!
    var bsList: LVGL.Dropdown!
    var resultView: LVGL.Object!

    init() {
        self.screen = LVGL.Screen()

        // Create Navigation Bar
        let navigationBar = LVGL.Object(parent: screen)
        navigationBar.setSize(width: LVGL.percent(100), height: 60)
        navigationBar.align(.topMid)
        navigationBar.setStyleBgColor(LVGL.Color(hex: 0xf8f8f8))
        navigationBar.setStyleBorderWidth(0)
        navigationBar.setStyleRadius(0)
        navigationBar.removeFlag(.scrollable)

        let titleLabel = LVGL.Label(parent: navigationBar)
        titleLabel.setText("Storage Benchmark")
        titleLabel.center()
        titleLabel.setStyleTextColor(.black)

        makeOptionView(screen: screen)
        makeResultView(screen: screen)
    }

    func makeOptionView(screen: LVGL.Screen) {
        let optionView = LVGL.Object(parent: screen)
        optionView.removeStyleAll()
        optionView.setSize(width: 320, height: 300)
        optionView.setFlexFlow(.column)
        optionView.align(.topMid, yOffset: 70)
        optionView.setStylePadRow(8)

        let files =
            (FileManager.default.contentsOfDirectory(atPath: "/sdcard") ?? []).map { "/sdcard/\($0)" } +
            (FileManager.default.contentsOfDirectory(atPath: "/usb") ?? []).map { "/usb/\($0)" }
        fileList = LVGL.Dropdown(parent: optionView)
        fileList.setOptions(files.joined(separator: "\n"))
        fileList.setWidth(320)

        let blockSizes = [512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072]
        bsList = LVGL.Dropdown(parent: optionView)
        bsList.setOptions("All\n" + blockSizes.map({ "\($0)" }).joined(separator: "\n"))
        bsList.setWidth(320)

        let button = LVGL.Button(parent: optionView)
        button.setWidth(320)
        button.addEventCb({
            let event = LVGL.Event(e: $0!)
            FFI.Wrapper<() -> ()>.unretained(event.getUserData())()
        }, filter: .pressed, userData: startBench.passUnretained())

        let buttonLabel = LVGL.Label(parent: button)
        buttonLabel.setText("Start")
        buttonLabel.center()
        buttonLabel.setStyleTextColor(.white)
    }

    func makeResultView(screen: LVGL.Screen) {
        resultView = LVGL.Object(parent: screen)
        resultView.removeStyleAll()
        resultView.setSize(width: 320, height: 420)
        resultView.setAlign(.bottomMid)
        resultView.setFlexFlow(.column)
    }

    private func println(_ str: String) {
        let label = LVGL.Label(parent: resultView)
        label.setText(str)
    }

    private lazy var startBench = FFI.Wrapper {
        self.resultView.clean()
        let file = self.fileList.getSelectedStr()
        let bs = Int(self.bsList.getSelectedStr()) ?? 0
        if file == "" {
            self.println("Invalid Params.")
            return
        }
        self.bench(file: file, blockSize: bs != 0 ? [bs] : [512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072])
    }

    private lazy var benchmarkOutput = FFI.Wrapper { (str: UnsafePointer<CChar>) in
        let sstr = String(cString: str)
        LVGL.asyncCall { self.println(sstr) }
        print(sstr)
    }

    private func bench(file: String, blockSize: [Int]) {
        Task(name: "Benchmark", priority: 15) { _ in
            file.withCString {
                for bs in blockSize {
                    storage_benchmark($0, Int32(bs), {
                        FFI.Wrapper<(UnsafePointer<CChar>) -> ()>.unretained($1)($0!)
                    }, self.benchmarkOutput.passUnretained())
                    Task.delay(1000)
                }
            }
        }
    }

}
