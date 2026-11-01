# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Shared by all pixpaper_426m apps: panel wiring + console overlay, panel binding, display driver.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/kakip_cm33.overlay)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pixpaper_426m_image)

target_sources(app PRIVATE src/main.c ../../common/pixpaper_426m.c)
