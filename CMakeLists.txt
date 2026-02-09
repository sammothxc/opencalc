cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
set(PICO_BOARD pico2 CACHE STRING "Board type")
include(pico_sdk_import.cmake)
project(opencalc C CXX ASM)

pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
add_subdirectory(i2ckbd)
add_subdirectory(lcdspi)

add_executable(opencalc
	opencalc.c
	)

target_link_libraries(opencalc
    pico_stdlib
    hardware_flash
    hardware_irq
    hardware_adc
    hardware_pwm
    hardware_i2c
    hardware_spi
    hardware_dma
    hardware_exception
	hardware_pio
    pico_multicore
	i2ckbd
	lcdspi
	)

pico_set_program_name(opencalc "opencalc")
pico_set_program_version(opencalc "0.1")

target_include_directories(opencalc PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_enable_stdio_uart(opencalc 0)
pico_enable_stdio_usb(opencalc 1)
pico_add_extra_outputs(opencalc)
