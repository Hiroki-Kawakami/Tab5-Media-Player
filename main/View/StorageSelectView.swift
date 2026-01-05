class StorageSelectView {

    private static var shared: StorageSelectView!
    let screen: LVGL.Screen
    let noStorage: LVGL.Object
    var list: LVGL.Object?
    var items: [ListItem] = []

    var needUpdate = false
    lazy var updateCallback = FFI.Wrapper { self.update() }
    let mountSdcard: (String, Int32) throws(IDF.Error) -> ()
    let mountUsbDrive: (String, Int32) throws(IDF.Error) -> ()
    var sdcardAvailable = false
    var usbDriveAvailable = false

    static func create(
        mountSdcard: @escaping (String, Int32) throws(IDF.Error) -> (),
        mountUsbDrive: @escaping (String, Int32) throws(IDF.Error) -> (),
    ) {
        shared = StorageSelectView(
            screen: .active,
            mountSdcard: mountSdcard,
            mountUsbDrive: mountUsbDrive
        )
        shared.update()
    }
    static func loadScreen() {
        shared?.screen.load()
    }

    private init(
        screen: LVGL.Screen,
        mountSdcard: @escaping (String, Int32) throws(IDF.Error) -> (),
        mountUsbDrive: @escaping (String, Int32) throws(IDF.Error) -> ()
    ) {
        self.screen = screen
        self.mountSdcard = mountSdcard
        self.mountUsbDrive = mountUsbDrive

        // Create Navigation Bar
        let navigationBar = LVGL.Object(parent: screen)
        navigationBar.setSize(width: LVGL.percent(100), height: 60)
        navigationBar.align(.topMid)
        navigationBar.setStyleBgColor(LVGL.Color(hex: 0xf8f8f8))
        navigationBar.setStyleBorderWidth(0)
        navigationBar.setStyleRadius(0)
        navigationBar.removeFlag(.scrollable)

        let titleLabel = LVGL.Label(parent: navigationBar)
        titleLabel.setText("Storage Select")
        titleLabel.center()
        titleLabel.setStyleTextColor(.black)

        noStorage = LVGL.Object(parent: screen)
        noStorage.removeStyleAll()
        noStorage.setSize(width: LVGL.percent(100), height: 80)
        noStorage.align(.topMid, yOffset: 120)

        let noStorageLabel = LVGL.Label(parent: noStorage)
        noStorageLabel.setText("No Storage Connected\nInsert SD Card or USB Drive")
        noStorageLabel.setStyleTextAlign(.center)
        noStorageLabel.setStyleTextColor(LVGL.Color(hex: 0x444444))
        noStorageLabel.align(.topMid)

        let reloadButton = LVGL.Button(parent: noStorage)
        reloadButton.align(.bottomMid)
        reloadButton.addEventCb({
            let event = LVGL.Event(e: $0!)
            FFI.Wrapper<() -> ()>.unretained(event.getUserData())()
        }, filter: .pressed, userData: updateCallback.passRetained())
        let reloadButtonLabel = LVGL.Label(parent: reloadButton)
        reloadButtonLabel.setText("Reload")

        let benchButton = LVGL.Button(parent: navigationBar)
        benchButton.setHeight(50)
        benchButton.align(.rightMid)
        benchButton.addEventCb({
            let event = LVGL.Event(e: $0!)
            FFI.Wrapper<() -> ()>.unretained(event.getUserData())()
        }, filter: .pressed, userData: openBench.passUnretained())
        let benchButtonLabel = LVGL.Label(parent: benchButton)
        benchButtonLabel.setText(LV_SYMBOL_SD_CARD)
        benchButtonLabel.center()
        benchButtonLabel.setStyleTextColor(.white)
    }

    func update() {
        checkSdcard()
        checkUsbDrive()
        if needUpdate { updateList() }
    }

    class ListItem {
        let path: String
        let callback: (String) -> ()
        init(path: String, callback: @escaping (String) -> ()) {
            self.path = path
            self.callback = callback
        }
    }

    private func updateList() {
        list?.deleteAsync()

        let list = LVGL.Object(parent: screen)
        self.list = list
        list.setStyleBorderWidth(0, selector: .main)
        list.setStyleBorderWidth(0, selector: .items)
        list.setSize(width: LVGL.percent(100), height: 580)
        list.align(.bottomMid)
        list.setStyleBgColor(LVGL.Color(hex: 0xeeeeee))
        list.setStylePadRow(16)
        list.setFlexFlow(.column)

        items = []
        if sdcardAvailable {
            items.append(ListItem(path: "/sdcard") { self.onSelect(path: $0) })
        }
        if usbDriveAvailable {
            items.append(ListItem(path: "/usb") { self.onSelect(path: $0) })
        }

        for item in items {
            let button = LVGL.Button(parent: list)
            button.setSize(width: LVGL.percent(100), height: 80)
            button.setStyleRadius(8)
            button.setStyleBorderWidth(1)
            button.setStyleBorderColor(LVGL.Color(hex: 0x444444))
            button.setStyleBgColor(.white)
            button.addEventCb({
                let item = Unmanaged<ListItem>.fromOpaque(LVGL.Event(e: $0!).getUserData()).takeUnretainedValue()
                item.callback(item.path)
            }, filter: .clicked, userData: Unmanaged.passUnretained(item).toOpaque())

            let label = LVGL.Label(parent: button)
            label.setText("\(item.path)")
            label.align(.leftMid)
            label.setStyleTextColor(.black)
        }

        noStorage.setFlag(.hidden, !items.isEmpty)
        needUpdate = false
    }

    func checkSdcard() {
        if !sdcardAvailable {
            do {
                try mountSdcard("/sdcard", 25)
                sdcardAvailable = true
                needUpdate = true
            } catch {}
        }
    }
    func checkUsbDrive() {
        if !usbDriveAvailable {
            do {
                try mountUsbDrive("/usb", 25)
                usbDriveAvailable = true
                needUpdate = true
            } catch {}
        }
    }

    private func onSelect(path: String) {
        FileManagerView.open(path: path)
        FileManagerView.loadScreen()
    }

    private let openBench = FFI.Wrapper {
        let benchView = StorageBenchView()
        benchView.screen.load()
    }
}
