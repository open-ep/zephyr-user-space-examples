# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Panel wiring + Kakip console are shared by all pixpaper_213m apps; they live in ../../common/.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/kakip_cm33.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pixpaper_213m_refresh_bench)

target_sources(app PRIVATE src/main.c)
