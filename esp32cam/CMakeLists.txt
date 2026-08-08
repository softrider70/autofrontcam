cmake_minimum_required(VERSION 3.24)

# ESP-IDF Projekt - Autofrontcam (AI-Thinker ESP32-CAM + OV2640)
# Kamera + Streaming-Server (Web-UI / iPhone)
#
# Projekt-Root ist dieses Verzeichnis (esp32cam/).
# Gemeinsame Komponenten (z.B. nvs_config) liegen eine Ebene hoeher in ../components.
set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(autofrontcam)

# Build-Nummer + version.h wird in main/CMakeLists.txt generiert
