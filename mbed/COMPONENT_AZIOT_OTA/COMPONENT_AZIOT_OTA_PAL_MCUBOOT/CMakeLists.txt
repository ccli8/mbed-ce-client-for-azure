# Copyright (c) 2020 ARM Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

target_include_directories(mbed-client-for-azure
    INTERFACE
        .
        mcuboot/boot/bootutil/include
        mcuboot/boot/bootutil/src
        mcuboot/boot/mbed/include
        mcuboot/boot/mbed/include
        mcubupdate_handler
)

target_sources(mbed-client-for-azure
    INTERFACE
        mcuboot/boot/bootutil/src/bootutil_public.c
        mcuboot_patch/flash_map_backend.cpp
        mcuboot_patch/secondary_bd.cpp
        mcubupdate_handler/mcubupdate_handler.cpp
)
