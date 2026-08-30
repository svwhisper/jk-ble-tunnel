# app_probe — house-side fake-app endpoint tester (the retired C3 board).
# Connects to a Node B clone exactly the way the JK app does and hex-logs
# every notify with ms timestamps on the USB console, so failed app sessions
# can be reproduced and inspected byte-by-byte without the phone.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app_probe)
