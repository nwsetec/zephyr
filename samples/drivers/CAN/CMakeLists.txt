# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/nordic.overlay ${CMAKE_CURRENT_SOURCE_DIR}/mcp2515-dts.overlay")

#set(CMAKE_GENERATOR "Ninja")
#message("generator is set to ${CMAKE_GENERATOR}")

set(CMAKE_EXTRA_GENERATOR "Eclipse CDT4 - Ninja")
message("extra generator is set to ${CMAKE_EXTRA_GENERATOR}")

set(CMAKE_ECLIPSE_VERSION 4.8)
message("eclipse generator version is set to ${CMAKE_ECLIPSE_VERSION}")

set(BOARD nrf52_pca10040)
message("Zephyr BOARD is set to ${BOARD}")

set(CONF_FILE "prj.conf prj.conf.mcp2515")
message("configuration file is set to ${CONF_FILE}")

include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(CAN)

target_sources(app PRIVATE src/main.c)
