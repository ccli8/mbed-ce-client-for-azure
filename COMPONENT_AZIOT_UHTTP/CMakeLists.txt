# Copyright (c) 2020 ARM Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

target_include_directories(mbed-client-for-azure
    INTERFACE
        azure-uhttp-c/inc
)

target_sources(mbed-client-for-azure
    INTERFACE
        azure-uhttp-c/src/uhttp.c
)
