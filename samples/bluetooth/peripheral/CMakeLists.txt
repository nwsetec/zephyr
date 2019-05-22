# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/ext_flash.overlay")

include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(peripheral)

target_sources(app PRIVATE
  src/main.c
  ../gatt/hrs.c
  ../gatt/bas.c
  ../gatt/cts.c
)

zephyr_library_include_directories($ENV{ZEPHYR_BASE}/samples/bluetooth)
zephyr_library_include_directories($ENV{ZEPHYR_BASE}/subsystem/storage)
