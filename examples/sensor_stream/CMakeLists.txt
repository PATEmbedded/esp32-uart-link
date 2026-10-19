cmake_minimum_required(VERSION 3.16)

# Build against the component in this repository, with warnings as errors.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
set(UART_LINK_WERROR ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uart_link_sensor_stream)
