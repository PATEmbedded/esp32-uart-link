# Inside an ESP-IDF build the repository is a component: the portable core plus the UART glue.
if(ESP_PLATFORM)
    # The UART driver moved from "driver" to "esp_driver_uart" in ESP-IDF 5.3; "driver" is gone in 6.0.
    if(IDF_VERSION_MAJOR GREATER_EQUAL 6 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 3))
        set(uart_component esp_driver_uart)
    else()
        set(uart_component driver)
    endif()

    idf_component_register(
        SRCS "core/src/cobs.c" "core/src/crc16.c" "core/src/frame.c" "core/src/link.c" "src/uart_link.c"
        INCLUDE_DIRS "include" "core/include"
        REQUIRES ${uart_component} esp_timer)

    target_compile_definitions(${COMPONENT_LIB} PUBLIC UL_MAX_PAYLOAD=${CONFIG_UART_LINK_MAX_PAYLOAD}u)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
    # The examples turn this on, so CI fails on any warning in the component.
    if(UART_LINK_WERROR)
        target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
    endif()
    return()
endif()

cmake_minimum_required(VERSION 3.20)
project(esp32_uart_link VERSION 1.0.0 LANGUAGES C)

# Host build: the portable core, its unit tests, the command-line tool and the fuzz target.

option(UL_BUILD_TESTS "Build the unit tests" ON)
option(UL_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(UL_FUZZ "Build the libFuzzer target (Clang only)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(UL_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wcast-qual
                -Wstrict-prototypes -Wmissing-prototypes -Werror)

if(UL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()
if(UL_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(ul_core STATIC
    core/src/cobs.c
    core/src/crc16.c
    core/src/frame.c
    core/src/link.c
)
target_include_directories(ul_core PUBLIC core/include)
target_compile_options(ul_core PRIVATE ${UL_WARNINGS})

add_executable(ul_tool tools/cli/ul_tool.c)
target_link_libraries(ul_tool PRIVATE ul_core)
target_compile_options(ul_tool PRIVATE ${UL_WARNINGS})

if(UL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test/host)
endif()

if(UL_FUZZ)
    add_executable(fuzz_receive fuzz/fuzz_receive.c)
    target_link_libraries(fuzz_receive PRIVATE ul_core)
    target_link_options(fuzz_receive PRIVATE -fsanitize=fuzzer)
endif()
