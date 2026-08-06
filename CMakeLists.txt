cmake_minimum_required(VERSION 3.24)

# ESP-IDF Projekt - Autofrontcam (AI-Thinker ESP32-CAM + OV2640)
set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(autofrontcam)