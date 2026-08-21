# Bench test board — TCP-driven BLE emulator (app-central or BMS-peripheral).
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_board)
