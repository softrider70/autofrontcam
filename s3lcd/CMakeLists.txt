cmake_minimum_required(VERSION 3.24)

# ESP-IDF Projekt - s3lcd (LCDWIKI ES3C40P)
# Display-Client: ESP32-S3 + 4.0" IPS (ST7796S 320x480, SPI) + FT6336U-Touch.
# Rolle: zeigt den Kamerastream vom ESP32-CAM (autofrontcam/esp32cam) an
# (abgeleitet von tft/cyd, angepasst an ESP32-S3 + ES3C40P).
#
# Projekt-Root ist dieses Verzeichnis (s3lcd/).
# Gemeinsame Komponenten liegen eine Ebene hoeher in ../components.
# WICHTIG: NUR die benoetigte Komponente angeben - esp_hal_clock unterstuetzt
# KEIN esp32s3 (fehlende esp32s3/clk_tree_hal.c) und wuerde den S3-Build brechen!
set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/../components/nvs_config)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3lcd)

# Build-Nummer + version.h wird in main/CMakeLists.txt generiert
