# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Shared by all pixpaper_426m apps: panel wiring + console overlay, panel binding, display driver.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/kakip_cm33.overlay)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pixpaper_426m_lvgl)

target_sources(app PRIVATE src/main.c ../../common/pixpaper_426m.c)

# Optional: LVGL's own widgets demo (-DCONFIG_LV_USE_DEMO_WIDGETS=y).
if(CONFIG_LV_USE_DEMO_WIDGETS)
  set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})
  file(GLOB demo_sources ${LVGL_DIR}/demos/widgets/*.c ${LVGL_DIR}/demos/widgets/assets/*.c)
  target_sources(app PRIVATE ${LVGL_DIR}/demos/lv_demos.c ${demo_sources})
  target_include_directories(app PRIVATE ${LVGL_DIR}/demos)
endif()
