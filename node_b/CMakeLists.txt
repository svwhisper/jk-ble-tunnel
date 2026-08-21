# Node B — house / BLE 5.0 peripheral / four advertising identities / tunnel client.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(node_b)
