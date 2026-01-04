class FileManagerView {

    private static var shared: FileManagerView!
    var pathList: [String] = []
    let screen: LVGL.Screen
    var backButton: LVGL.Button!
    var titleLabel: LVGL.Label!
    var list: LVGL.List?
    var items: [ListItem] = []

    static func open(path: String) {
        if shared == nil { shared = FileManagerView() }
        shared.pathList.append(path)
        shared.update()
    }
    static func loadScreen() {
        shared?.screen.load()
    }

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

        titleLabel = LVGL.Label(parent: navigationBar)
        titleLabel.center()
        titleLabel.setStyleTextColor(.black)

        let backButtonAction = FFI.Wrapper {
            self.backButtonAction()
        }
        backButton = LVGL.Button(parent: navigationBar)
        backButton.setHeight(50)
        backButton.align(.leftMid)
        backButton.addEventCb({
            let event = LVGL.Event(e: $0!)
            FFI.Wrapper<() -> ()>.unretained(event.getUserData())()
        }, filter: .pressed, userData: backButtonAction.passRetained())
        let backButtonLabel = LVGL.Label(parent: backButton)
        backButtonLabel.setText("Back")
        backButtonLabel.center()
        backButtonLabel.setStyleTextColor(.white)
    }

    private func backButtonAction() {
        pathList.removeLast()
        if pathList.count > 0 {
            update()
        } else {
            StorageSelectView.loadScreen()
        }
    }

    func update() {
        titleLabel.setText(pathList.last!)
        updateList()
    }

    class ListItem {
        let name: String
        let path: String
        let isDirectory: Bool
        let callback: (ListItem) -> ()
        init(directory: String, name: String, callback: @escaping (ListItem) -> ()) {
            self.name = name
            self.path = "\(directory)/\(name)"
            self.isDirectory = FileManager.default.isDirectory(atPath: self.path)
            self.callback = callback
        }

        init(name: String, callback: @escaping (ListItem) -> ()) {
            self.name = name
            self.path = name
            self.isDirectory = false
            self.callback = callback
        }
    }

    private func updateList() {
        list?.deleteAsync()

        let list = LVGL.List(parent: screen)
        self.list = list
        list.setSize(width: LVGL.percent(100), height: 580)
        list.align(.bottomMid)
        list.setStyleBgColor(.white)

        let path = pathList.last!
        items = (FileManager.default.contentsOfDirectory(atPath: path) ?? [])
            .sorted(by: naturalSort)
            .map { ListItem(directory: path, name: $0) { self.onSelect(item: $0) } }

        for item in items {
            if !item.isDirectory && !item.name.lowercased().hasSuffix(".avi") { continue }
            let icon = item.isDirectory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_VIDEO
            let button = list.addButton(icon: icon, text: item.name)
            button.addEventCb({
                let item = Unmanaged<ListItem>.fromOpaque(LVGL.Event(e: $0!).getUserData()).takeUnretainedValue()
                item.callback(item)
            }, filter: .clicked, userData: Unmanaged.passUnretained(item).toOpaque())
        }
    }

    private func onSelect(item: ListItem) {
        if item.isDirectory {
            FileManagerView.open(path: item.path)
        } else {
            VideoPlayerView.open(file: item.path)
        }
    }
}
