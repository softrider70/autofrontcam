idf_component_register(
    SRCS
        main.c
        display.c
        stream.c
        touch.c
        ui.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
    REQUIRES
        nvs_config
        nvs_flash
        esp_timer
        esp_event
        esp_wifi
        esp_netif
        esp_http_client
        esp_lcd
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c
        driver
        heap
        esp_jpeg
)

# Build-Nummer erhoehen + include/version.h generieren (VOR der main-Kompilierung)
# increment_build.py liegt im Repo-Root unter tools/, --project-dir zeigt auf dieses Projekt.
add_custom_target(cyd_version ALL
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/../tools/increment_build.py --project-dir ${CMAKE_SOURCE_DIR}
    COMMENT "Erhoehe Build-Nummer und generiere version.h"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} cyd_version)
# main.c.obj haengt EXPLIZIT von version.h ab -> Generierung laeuft immer vor der
# Kompilierung und main.c wird bei jeder Aenderung der Buildnummer neu gebaut
target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_SOURCE_DIR}/include/version.h)
