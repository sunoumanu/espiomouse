# mouseum-esp32 — root CMake
# ESP-IDF project for a USB HID mouse emulator with HTTP REST control plane.
#
# Build:
#   idf.py set-target esp32s3
#   idf.py build
#   idf.py -p <PORT> flash monitor
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mouseum-esp32 C)
