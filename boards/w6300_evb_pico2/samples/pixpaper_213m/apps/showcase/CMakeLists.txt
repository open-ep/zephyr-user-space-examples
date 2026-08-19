# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Panel wiring is shared by every app of this sample; it lives in ../../common/.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/w6300_evb_pico2.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pixpaper_showcase)

target_sources(app PRIVATE src/main.c)
