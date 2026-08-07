idf_component_register(
    SRCS
        main.c
        camera.c
        nvs_config.c
        heap_monitor.c
        stack_monitor.c
        wifi.c
        ota.c
        voltage.c
        sleep.c
        lines.c
        dns_server.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
    EMBED_FILES
        index.html
    REQUIRES
        nvs_flash
        esp_timer
        driver
        esp_wifi
        esp_netif
        esp_http_server
        app_update
        esp_driver_gpio
        esp_adc
        esp_hw_support
        heap
        esp32-camera
)

# Build-Nummer erhoehen + include/version.h generieren (VOR der main-Kompilierung)
add_custom_target(autofrontcam_version ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/increment_build.py
    COMMENT "Erhoehe Build-Nummer und generiere version.h"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} autofrontcam_version)
# main.c.obj haengt EXPLIZIT von version.h ab -> Generierung laeuft immer vor der
# Kompilierung und main.c wird bei jeder Aenderung der Buildnummer neu gebaut
target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_SOURCE_DIR}/include/version.h)