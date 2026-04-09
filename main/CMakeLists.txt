set(SRCS
    "main.c"
    "config.c"
    "wifi_manager.c"
    "auth.c"
    "uart_bridge.c"
    "serial_port.c"
    "web_server.c"
    "gpio_control.c"
    "ota_github.c"
)

if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND SRCS "usb_cdc_bridge.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "${CMAKE_SOURCE_DIR}/frontend/index.html"
        "${CMAKE_SOURCE_DIR}/certs/server.crt"
        "${CMAKE_SOURCE_DIR}/certs/server.key"
    EMBED_FILES
        "${CMAKE_SOURCE_DIR}/frontend/lib/xterm.min.js.gz"
)
