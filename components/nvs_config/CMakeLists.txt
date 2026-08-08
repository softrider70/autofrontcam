# Gemeinsame Komponente: NVS-Config-Helfer
# Wird von beiden Projekten (esp32cam/ und cyd/) verwendet.
idf_component_register(
    SRCS
        nvs_config.c
    INCLUDE_DIRS
        include
    REQUIRES
        nvs_flash
)
