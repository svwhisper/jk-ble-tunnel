# House-side BLE scan probe (the retired C3 Node A board, revived over USB).
# Prints every named advert line-by-line on the USB-Serial-JTAG console so the
# Mac can watch what Node B's clone sets actually put on the air.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(scan_probe)
