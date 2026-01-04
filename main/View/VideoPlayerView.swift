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

    var playButtonLabel: LVGL.Label!
    var volumeSlider: LVGL.Slider!

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
        controlView.setSize(width: LVGL.percent(100), height: 150)
        controlView.align(.bottomMid)
        controlView.setStyleBgColor(.black)
        controlView.setStyleBorderWidth(1)
        controlView.setStyleBorderColor(.white)
        controlView.setStyleBorderSide(LV_BORDER_SIDE_TOP)
        controlView.setStyleRadius(0)
        controlView.removeFlag(.scrollable)

        let playButton = LVGL.Button(parent: controlView)
        playButton.setSize(width: 70, height: 70)
        playButton.align(.center)
        playButton.addEventCallback(filter: .pressed, callback: playButtonPressed)
        playButtonLabel = LVGL.Label(parent: playButton)
        playButtonLabel.center()
        playButtonLabel.setStyleTextColor(.white)
        stateChanged(state: player.state)

        let volumeRow = LVGL.Object(parent: controlView)
        volumeRow.removeStyleAll()
        volumeRow.setSize(width: LVGL.percent(100), height: 30)
        volumeRow.align(.bottomMid, yOffset: 10)
        volumeSlider = LVGL.Slider(parent: volumeRow)
        volumeSlider.setWidth(270)
        volumeSlider.align(.center)
        volumeSlider.setRange(min: 1, max: 100)
        volumeSlider.setValue(Int32(AudioController.volume), anim: false)
        volumeSlider.addEventCallback(filter: .valueChanged, callback: volumeSliderValueChanged)
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
        case .play : playButtonLabel.setText("Pause")
        case .pause: playButtonLabel.setText("Play")
        case .stop : playButtonLabel.setText("Play")
        default: break
        }
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
    private lazy var volumeSliderValueChanged = FFI.Wrapper {
        AudioController.volume = Int(self.volumeSlider.getValue())
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
