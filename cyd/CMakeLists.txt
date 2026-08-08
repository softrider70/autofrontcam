cmake_minimum_required(VERSION 3.24)

# ESP-IDF Projekt - CYD (Cheap Yellow Display, ESP32-2432S028R)
# Display-Client: zeigt den Kamerastream vom ESP32-CAM (autofrontcam/esp32cam) an.
#
# Projekt-Root ist dieses Verzeichnis (cyd/).
# Gemeinsame Komponenten (z.B. nvs_config) liegen eine Ebene hoeher in ../components.
set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(autofrontcam_cyd)

# Build-Nummer + version.h wird in main/CMakeLists.txt generiert
