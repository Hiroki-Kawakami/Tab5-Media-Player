if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/usb_msc.c" "src/msc_bot.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES bsp fatfs vfs esp_mm)
else()
    idf_component_register(
        SRCS "src/usb_msc_sim.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES harness)
endif()
