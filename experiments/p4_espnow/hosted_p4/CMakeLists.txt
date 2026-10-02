cmake_minimum_required(VERSION 3.16)
# The host side of the ESP-NOW bridge is the production component; the probe
# links the same source the P4 firmware ships, never a copy.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components/esp_now_hosted")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hw1_hosted_p4_probe)
