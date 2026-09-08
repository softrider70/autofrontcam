idf_component_register(
    SRCS
        main.c
        display.c
        touch.c
        stream.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c
        nvs_flash
        esp_timer
        freertos
        heap
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_jpeg
)

# Build-Nummer erhoehen + include/version.h generieren (VOR der main-Kompilierung)
add_custom_target(s3lcd_version ALL
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/../tools/increment_build.py --project-dir ${CMAKE_SOURCE_DIR}
    COMMENT "Erhoehe Build-Nummer und generiere version.h"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} s3lcd_version)
target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_SOURCE_DIR}/include/version.h)
