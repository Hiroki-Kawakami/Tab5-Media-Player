if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/usb_msc.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES bsp fatfs vfs)
else()
    idf_component_register(
        SRCS "src/usb_msc_sim.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES harness)
endif()
