cmake_minimum_required(VERSION 3.24)

# ESP-IDF Projekt - tft (generisches ESP32-Dev-Board + 2.8" SPI-Display, ILI9341)
# Display-Client: zeigt den Kamerastream vom ESP32-CAM (autofrontcam/esp32cam) an.
# Abgeleitet von cyd/ (Git-Branch "tft").
#
# Projekt-Root ist dieses Verzeichnis (tft/).
# Gemeinsame Komponenten (z.B. nvs_config) liegen eine Ebene hoeher in ../components.
set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(autofrontcam_tft)

# Build-Nummer + version.h wird in main/CMakeLists.txt generiert
