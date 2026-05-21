#
# Copyright (c) 2018 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(IOT_PIPELINE)



# NORDIC SDK APP START

target_sources(app PRIVATE
    src/main.c
    src/mqtt_connection.c
    modules/bosch/bmp581/bmp5.c
)

# Add include directories

target_include_directories(app PRIVATE
     include
     modules/bosch/bmp581
) # To find "sen55.h"