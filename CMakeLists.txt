# PicoCalc Space Game  (by Thomas Dzubin)
#
# Reuses the drivers from ./picocalc-text-starter-main (lcd, southbridge, font),
# which is picocalc-text-starter by Blair Leduc, plus the Raspberry Pi Pico SDK.
# Build it the same way you build that starter: with the Raspberry Pi Pico VS
# Code extension, or from a terminal that has PICO_SDK_PATH set (see README.md).

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
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in the Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(picocalc-space-game C CXX ASM)

pico_sdk_init()

# Location of the picocalc-text-starter drivers we build against
set(STARTER ${CMAKE_CURRENT_LIST_DIR}/picocalc-text-starter-main)

add_executable(picocalc-space-game
    space_game.c
    space_game.h
    GAME-DETAILS.txt              # long-form design notes (not compiled; IDE only)
    space_game_screenshot.c       # optional F5 screenshot - safe to remove this
    space_game_screenshot.h       #   line + both files (see the .h for the recipe)
    ${STARTER}/drivers/lcd.c
    ${STARTER}/drivers/southbridge.c
    ${STARTER}/drivers/font-8x10.c
    ${STARTER}/drivers/audio.c
    ${STARTER}/drivers/sdcard.c    # SD card + FAT32 - only referenced by the
    ${STARTER}/drivers/fat32.c    #   screenshot; gc-sections drops both otherwise
)

pico_set_program_name(picocalc-space-game "picocalc-space-game")
pico_set_program_version(picocalc-space-game "V1.00A")   # keep in sync with VERSION_STR in space_game.h

# Build the audio PIO program (generates audio.pio.h that drivers/audio.c needs)
pico_generate_pio_header(picocalc-space-game ${STARTER}/drivers/audio.pio)

# No serial console needed; keep USB free so ESC -> BOOTSEL is clean
pico_enable_stdio_uart(picocalc-space-game 0)
pico_enable_stdio_usb(picocalc-space-game 0)

target_compile_options(picocalc-space-game PRIVATE -Wall)

target_include_directories(picocalc-space-game PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${STARTER}
    ${STARTER}/drivers
)

target_link_libraries(picocalc-space-game
    pico_stdlib
    pico_multicore
    pico_sync          # pico/sem.h - used by drivers/fat32.c
    pico_float
    hardware_gpio
    hardware_i2c
    hardware_spi
    hardware_watchdog   # watchdog_reboot(): leaving for the UF2 Loader
    hardware_pio
    hardware_clocks
)

# The chip is part of every output file name (space-game-RP2040.uf2 /
# space-game-RP2350.uf2, and the matching .elf/.bin/.hex), so a downloaded file
# always says which board it was built for.
if(PICO_PLATFORM MATCHES "^rp2040")
    set(CHIP_NAME RP2040)
elseif(PICO_PLATFORM MATCHES "^rp2350")
    set(CHIP_NAME RP2350)
else()
    set(CHIP_NAME ${PICO_PLATFORM})
endif()
set_target_properties(picocalc-space-game PROPERTIES OUTPUT_NAME space-game-${CHIP_NAME})

pico_add_extra_outputs(picocalc-space-game)

# Copy the finished .uf2 (same name) to the top-level project folder, so the
# RP2040 and RP2350 builds can sit side by side.
add_custom_command(TARGET picocalc-space-game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:picocalc-space-game>/$<TARGET_FILE_BASE_NAME:picocalc-space-game>.uf2
            ${CMAKE_CURRENT_LIST_DIR}/
)

