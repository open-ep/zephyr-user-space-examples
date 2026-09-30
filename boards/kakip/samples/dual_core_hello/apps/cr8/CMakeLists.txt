# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Keeps the last 4 KB of R8SRAM free for the shared log ring, drops the EVK console.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/kakip_cr8.overlay)

# The CR8 image has two load regions: the vector table at CR8 address 0 (ITCM) and
# everything else in R8SRAM. Write them as two files for U-Boot to copy separately.
# Must be set before find_package(Zephyr): that is where the post-build list is read.
set(elf ${CMAKE_BINARY_DIR}/zephyr/zephyr.elf)
set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
  COMMAND $<TARGET_PROPERTY:bintools,elfconvert_command> -O binary -j rom_start
          ${elf} ${CMAKE_BINARY_DIR}/zephyr/cr8_itcm.bin
  COMMAND $<TARGET_PROPERTY:bintools,elfconvert_command> -O binary -R rom_start
          ${elf} ${CMAKE_BINARY_DIR}/zephyr/cr8_sram.bin
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kakip_dual_core_hello_cr8)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../../common)
