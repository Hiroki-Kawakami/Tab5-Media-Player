fileprivate let Log = Logger(tag: "USBHost")

class USBHost {

    func install() throws(IDF.Error) {
        var config = usb_host_config_t()
        config.intr_flags = ESP_INTR_FLAG_LEVEL1
        try IDF.Error.check(usb_host_install(&config))
        Task(name: "USBHost", priority: 10) { _ in self.task() }
    }

    private func task() {
        while true {
            var eventFlags: UInt32 = 0
            usb_host_lib_handle_events(portMAX_DELAY, &eventFlags)
            if (eventFlags & UInt32(USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) != 0 {
                usb_host_device_free_all()
            }
            if (eventFlags & UInt32(USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) != 0 {
                Log.info("All devices freed")
            }
        }
    }

    var deviceAddrList: [UInt8] {
        var addrList = [UInt8](repeating: 0, count: 16)
        var numDevices: Int32 = 0
        usb_host_device_addr_list_fill(Int32(addrList.count), &addrList, &numDevices)
        if numDevices != addrList.count {
            addrList.removeLast(addrList.count - Int(numDevices))
        }
        return addrList
    }

    // MSC Driver
    class MSC {
        func install(
            taskStackSize: Int,
            taskPriority: UInt32,
            xCoreID: Int32 = tskNO_AFFINITY,
            createBackgroundTask: Bool = true,
        ) throws(IDF.Error) {
            var config = msc_host_driver_config_t(
                create_backround_task: createBackgroundTask,
                task_priority: Int(taskPriority),
                stack_size: taskStackSize,
                core_id: xCoreID,
                callback: { (event, arg) in
                    let msc = Unmanaged<MSC>.fromOpaque(arg!).takeUnretainedValue()
                    msc.callback(event: event!)
                },
                callback_arg: Unmanaged.passRetained(self).toOpaque()
            )
            try IDF.Error.check(msc_host_install(&config))
            Log.info("MSC class driver installed")
        }

        func callback(event: UnsafePointer<msc_host_event_t>) {
            switch event.pointee.event {
            case MSC_DEVICE_CONNECTED:
                Log.info("MSC device connected, addr: \(event.pointee.device.address)")
                self.addr = event.pointee.device.address
            case MSC_DEVICE_DISCONNECTED:
                Log.info("MSC device disconnected")
                self.addr = nil
            default:
                abort()
            }
        }

        var addr: UInt8?
        var device: msc_host_device_handle_t?
        var vfsHandle: msc_host_vfs_handle_t?

        func mount(path: String, maxFiles: Int32) throws(IDF.Error) {
            guard let addr = addr else {
                throw IDF.Error(ESP_ERR_NOT_FOUND)
            }
            var mountConfig = esp_vfs_fat_mount_config_t(
                format_if_mount_failed: false,
                max_files: maxFiles,
                allocation_unit_size: 1024,
                disk_status_check_enable: false,
                use_one_fat: false
            )

            try IDF.Error.check(msc_host_install_device(addr, &device))
            try IDF.Error.check(path.utf8CString.withUnsafeBufferPointer {
                msc_host_vfs_register(device, $0.baseAddress!, &mountConfig, &vfsHandle)
            })
        }
    }
}
