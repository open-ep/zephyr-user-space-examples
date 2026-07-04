# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Panel wiring is shared by all pixpaper_213m apps; it lives in ../../common/.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/uiapduino_pro_micro_ch32v003.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pixpaper_213m_console)

target_sources(app PRIVATE src/main.c)
