set(UI_FILELIST "${CMAKE_CURRENT_LIST_DIR}/ui/filelist.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${UI_FILELIST}")
file(STRINGS "${UI_FILELIST}" UI_SOURCE_FILES)
list(TRANSFORM UI_SOURCE_FILES PREPEND "ui/")

idf_component_register(
    SRCS
        "main.c"
        "lcd.c"
        "lvgl_demo.c"
        "es8311_codec.c"
        "music/MP3.cpp"
        "music/mp3_duration.c"
        "music/mp3_cover.c"
        "music/gecilrc.c"
        "sd.c"
        "wifi.c"
        "time/ntp.c"
        "time/time_ui.c"
        "weatherfl/weather.c"
        "historyfl/history_today.c"
        "memo/memo.c"
        "usb_extend_screen/usb_extend_screen.c"
        "usb_extend_screen/usb_extend_frame.c"
        "usb_extend_screen/usb_extend_display.c"
        "usb_extend_screen/usb_extend_vendor.c"
        "usb_extend_screen/usb_extend_hid.c"
        "usb_extend_screen/usb_extend_usb.c"
        "usb_extend_screen/usb_extend_screen_ui.c"
        "music/progress_seek.c"
        "ui_envent.c"
        ${UI_SOURCE_FILES}
    PRIV_REQUIRES
        spi_flash
        esp_lcd
        esp_timer
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_sdmmc
        esp_driver_jpeg
        esp_driver_ppa
        esp_http_client
        esp_http_server
        esp_event
        esp_hw_support
        esp_netif
        esp_rom
        esp_wifi
        esp_wifi_remote
        esp-tls
        esp_codec_dev
        esphome__micro-decoder
        esp_lcd_st7701
        esp_lcd_touch
        esp_lcd_touch_gt911
        lvgl
        sdmmc
        fatfs
        json
        lwip
        nvs_flash
        pthread
        espressif__tinyusb
        usb
    INCLUDE_DIRS
        "."
        "music"
        "time"
        "weatherfl"
        "historyfl"
        "memo"
        "usb_extend_screen"
        "ui"
        "ui/screens"
        "ui/components"
)

idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_name tinyusb)
else()
    set(tinyusb_name espressif__tinyusb)
endif()
idf_component_get_property(tinyusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tinyusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/usb_extend_screen")
target_sources(${tinyusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/usb_extend_screen/usb_extend_descriptors.c")

cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tinyusb_lib} PRIVATE ${COMPONENT_LIB})
