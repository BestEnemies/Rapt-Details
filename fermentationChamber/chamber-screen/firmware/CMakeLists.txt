cmake_minimum_required(VERSION 3.16)

# Replacement firmware for the KegLand RAPT FridgeG5 controller.
# Target hardware: ESP32-D0WD-V3 (rev v3.0), 4 MB flash, PSRAM (WROVER module).
# Pin map recovered from the stock `fancyfridge` firmware — see ../fridgeg5-map.html

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(chamber_screen)
