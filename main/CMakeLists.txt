# main/CMakeLists.txt
#
# The HID transport implementation is picked from ${IDF_TARGET}:
#   esp32s3 / esp32s2  -> usb_hid_task.c  (TinyUSB HID device)
#   esp32c6 / others   -> ble_hid_task.c  (NimBLE HID-over-GATT)
#
# Add a new chip by extending the if-block below.  The REQUIRES list also
# changes per-target because each transport pulls in different components.

set(_common_srcs
    main.c
    mouse_engine.c
    cmd_dispatcher.c
    wifi_manager.c
    http_server.c
)

set(_common_reqs
    driver
    freertos
    esp_timer
    esp_wifi
    esp_event
    esp_netif
    nvs_flash
    esp_http_server
    json
    log
)

if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32s2")
    list(APPEND _common_srcs usb_hid_task.c)
    list(APPEND _common_reqs esp_tinyusb)
    message(STATUS "mouseum: HID transport = TinyUSB USB (${IDF_TARGET})")
elseif(IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32h2"
        OR IDF_TARGET STREQUAL "esp32c3")
    list(APPEND _common_srcs ble_hid_task.c)
    list(APPEND _common_reqs bt)
    message(STATUS "mouseum: HID transport = NimBLE BLE HID (${IDF_TARGET})")
else()
    message(FATAL_ERROR "mouseum: no HID transport configured for IDF_TARGET=${IDF_TARGET}.\n"
                        "Add an arm to main/CMakeLists.txt.")
endif()

idf_component_register(
    SRCS         ${_common_srcs}
    INCLUDE_DIRS "."
    REQUIRES     ${_common_reqs}
)
