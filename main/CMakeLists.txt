idf_component_register(
    SRCS
        "main.c"
        "config.c"
        "wifi_manager.c"
        "auth.c"
        "uart_bridge.c"
        "web_server.c"
        "gpio_control.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "${CMAKE_SOURCE_DIR}/frontend/index.html"
        "${CMAKE_SOURCE_DIR}/certs/server.crt"
        "${CMAKE_SOURCE_DIR}/certs/server.key"
    EMBED_FILES
        "${CMAKE_SOURCE_DIR}/frontend/lib/xterm.min.js.gz"
)
