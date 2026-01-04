fileprivate let Log = Logger(tag: "VideoPlayer")

class VideoPlayerView {

    static func open(file: String) {
        let view = VideoPlayerView(file: file)
        DisplayMultiplexer.push(mode: .videoPlayer(view: view), screen: view.screen)
        Task(name: "PlayerView", priority: 2) { _ in view.start() }
    }

    let file: String
    let screen = LVGL.Screen()
    let player = AVIPlayer()

    var playButtonLabel: LVGL.Image!
    var slider: LVGL.Slider!
    var sliderLeftIcon: LVGL.Image!
    var sliderRightIcon: LVGL.Image!
    var sliderModeIcon: LVGL.Image!

    private enum SliderMode {
        case volume
        case brightness

        var value: Int {
            get {
                switch self {
                case .volume: AudioController.volume
                case .brightness: DisplayMultiplexer.brightness
                }
            }
            set {
                switch self {
                case .volume: AudioController.volume = newValue
                case .brightness: DisplayMultiplexer.brightness = newValue
                }
            }
        }
    }
    private static var sliderMode = SliderMode.volume

    init(file: String) {
        self.file = file

        screen.setStyleBgColor(.black)
        createNavigationBar()
        createControlView()

        player.stateChangedCallback = { self.stateChanged(state: $0) }
    }

    func createNavigationBar() {
        let navigationBar = LVGL.Object(parent: screen)
        navigationBar.setSize(width: LVGL.percent(100), height: 60)
        navigationBar.align(.topMid)
        navigationBar.setStyleBgColor(.black)
        navigationBar.setStyleBorderWidth(1)
        navigationBar.setStyleBorderColor(.white)
        navigationBar.setStyleBorderSide(LV_BORDER_SIDE_BOTTOM)
        navigationBar.setStyleRadius(0)
        navigationBar.removeFlag(.scrollable)

        let titleLabel = LVGL.Label(parent: navigationBar)
        titleLabel.setText(String(file.split(separator: "/").last ?? "Video Player"))
        titleLabel.center()
        titleLabel.setStyleTextColor(.white)

        let backButton = LVGL.Button(parent: navigationBar)
        backButton.setHeight(50)
        backButton.align(.leftMid)
        backButton.addEventCallback(filter: .pressed, callback: backButtonAction)
        let backButtonLabel = LVGL.Label(parent: backButton)
        backButtonLabel.setText("Back")
        backButtonLabel.center()
        backButtonLabel.setStyleTextColor(.white)
    }
    func createControlView() {
        let controlView = LVGL.Object(parent: screen)
        controlView.removeStyleAll()
        controlView.setSize(width: LVGL.percent(100), height: 162)
        controlView.align(.bottomMid)
        controlView.setStyleBgColor(.black)
        controlView.setStyleBorderWidth(1)
        controlView.setStyleBorderColor(.white)
        controlView.setStyleBorderSide(LV_BORDER_SIDE_TOP)

        let buttonsView = LVGL.Object(parent: controlView)
        buttonsView.removeStyleAll()
        buttonsView.setSize(width: 360, height: 80)
        buttonsView.setFlexFlow(.row)
        buttonsView.setFlexAlign(mainPlace: .center, crossPlace: .center, trackCrossPlace: .center)
        buttonsView.setStylePadColumn(5)
        buttonsView.align(.topMid, yOffset: 4)
        // buttonsView.setStyleBgColor(LVGL.Color(hex: 0x440000))
        // buttonsView.setStyleBgOpa(.cover)
        let addButton = { (size: Int32, icon: UnsafeRawPointer?, callback: FFI.Wrapper<() -> ()>) -> LVGL.Image in
            let button = LVGL.Button(parent: buttonsView)
            button.removeStyleAll()
            button.setSize(width: size, height: size)
            button.addEventCallback(filter: .clicked, callback: callback)
            let image = LVGL.Image(parent: button)
            image.setStyleImageRecolorOpa(.percent(30), selector: .pressed)
            image.center()
            if let icon = icon { image.setSrc(icon) }
            return image
        }
        playButtonLabel = addButton(80, nil, playButtonPressed)
        stateChanged(state: player.state)

        let sliderView = LVGL.Object(parent: controlView)
        sliderView.removeStyleAll()
        sliderView.setSize(width: 340, height: 30)
        sliderView.alignTo(base: buttonsView, align: .outBottomMid, yOffset: 4)
        // sliderView.setStyleBgColor(LVGL.Color(hex: 0x004400))
        // sliderView.setStyleBgOpa(.cover)
        sliderLeftIcon = LVGL.Image(parent: sliderView)
        sliderLeftIcon.align(.leftMid)
        sliderLeftIcon.setSize(width: 30, height: 30)
        sliderRightIcon = LVGL.Image(parent: sliderView)
        sliderRightIcon.align(.rightMid)
        sliderRightIcon.setSize(width: 30, height: 30)
        slider = LVGL.Slider(parent: sliderView)
        slider.setWidth(240)
        slider.align(.center)
        slider.setRange(min: 1, max: 100)
        slider.addEventCallback(filter: .valueChanged, callback: sliderValueChanged)

        let smallButtonsView = LVGL.Object(parent: controlView)
        smallButtonsView.removeStyleAll()
        smallButtonsView.setSize(width: LVGL.percent(100), height: 30)
        smallButtonsView.setFlexFlow(.row)
        smallButtonsView.setFlexAlign(mainPlace: .center, crossPlace: .center, trackCrossPlace: .center)
        smallButtonsView.setStylePadColumn(25)
        smallButtonsView.alignTo(base: sliderView, align: .outBottomMid, yOffset: 8)
        // smallButtonsView.setStyleBgColor(LVGL.Color(hex: 0x000044))
        // smallButtonsView.setStyleBgOpa(.cover)
        let addSmallButton = { (icon: UnsafeRawPointer?, callback: FFI.Wrapper<() -> ()>) -> LVGL.Image in
            let button = LVGL.Button(parent: smallButtonsView)
            button.removeStyleAll()
            button.setSize(width: 30, height: 30)
            button.addEventCallback(filter: .clicked, callback: callback)
            let image = LVGL.Image(parent: button)
            image.setStyleImageRecolorOpa(.percent(30), selector: .pressed)
            image.center()
            if let icon = icon { image.setSrc(icon) }
            return image
        }
        sliderModeIcon = addSmallButton(nil, sliderModeButtonPressed)
        sliderModeChanged()
    }

    func start() {
        if player.open(file: file) {
            player.play()
        }
    }
    func close() {
        player.close()
        DisplayMultiplexer.pop()
    }

    private func stateChanged(state: AVIPlayer.State) {
        switch state {
        case .play : playButtonLabel.setSrc(R.icon.pause_circle)
        case .pause: playButtonLabel.setSrc(R.icon.play_circle)
        case .stop : playButtonLabel.setSrc(R.icon.play_circle)
        default: break
        }
    }
    private func sliderModeChanged() {
        switch VideoPlayerView.sliderMode {
        case .volume :
            sliderLeftIcon.setSrc(R.icon.volume_mute)
            sliderRightIcon.setSrc(R.icon.volume_up)
            sliderModeIcon.setSrc(R.icon.brightness_mid)
        case .brightness:
            sliderLeftIcon.setSrc(R.icon.brightness_low)
            sliderRightIcon.setSrc(R.icon.brightness_high)
            sliderModeIcon.setSrc(R.icon.volume_down)
        }
        slider.setValue(Int32(VideoPlayerView.sliderMode.value), anim: false)
    }

    // LVGL Events
    private lazy var backButtonAction = FFI.Wrapper {
        self.close()
    }
    private lazy var playButtonPressed = FFI.Wrapper {
        if self.player.state == .play {
            self.player.pause()
        } else if self.player.state == .pause {
            self.player.resume()
        } else {
            self.player.play()
        }
    }
    private lazy var sliderModeButtonPressed = FFI.Wrapper {
        switch VideoPlayerView.sliderMode {
        case .volume: VideoPlayerView.sliderMode = .brightness
        case .brightness: VideoPlayerView.sliderMode = .volume
        }
        self.sliderModeChanged()
    }
    private lazy var sliderValueChanged = FFI.Wrapper {
        VideoPlayerView.sliderMode.value = Int(self.slider.getValue())
    }
}

fileprivate extension LVGL.ObjectProtocol {
    func addEventCallback(filter: lv_event_code_t, callback: FFI.Wrapper<() -> ()>) {
        addEventCb({
            let event = LVGL.Event(e: $0!)
            FFI.Wrapper<() -> ()>.unretained(event.getUserData())()
        }, filter: filter, userData: callback.passUnretained())
    }
}
