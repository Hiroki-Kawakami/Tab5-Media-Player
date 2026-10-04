set(AIRPLAY_SRCS
    "src/airport_key.cpp"
    "src/crypto.cpp"
    "src/decoder.cpp"
    "src/receiver.cpp"
    "src/remote.cpp"
    "src/stream.cpp"
    "src/task.cpp")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${AIRPLAY_SRCS} "src/discovery_mdns.cpp"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES esp_timer lwip mbedtls espressif__mdns espressif__esp_audio_codec)
else()
    idf_component_register(
        SRCS ${AIRPLAY_SRCS} "src/discovery_dnssd.cpp"
        INCLUDE_DIRS "include")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TFPSACRYPTO REQUIRED IMPORTED_TARGET tfpsacrypto)
    target_link_libraries(${COMPONENT_LIB} PkgConfig::TFPSACRYPTO)
    if(NOT APPLE)
        pkg_check_modules(DNSSD REQUIRED IMPORTED_TARGET avahi-compat-libdns_sd)
        target_link_libraries(${COMPONENT_LIB} PkgConfig::DNSSD)
    endif()
endif()
