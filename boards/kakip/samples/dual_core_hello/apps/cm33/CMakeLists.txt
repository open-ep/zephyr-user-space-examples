# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Kakip wiring (console on SCI5, CN6 pin 8/10) is shared in ../../common/.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/kakip_cm33.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kakip_dual_core_hello_cm33)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../../common)
