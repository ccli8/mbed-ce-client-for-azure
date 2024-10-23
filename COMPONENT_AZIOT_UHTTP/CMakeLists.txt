# Copyright (c) 2020 ARM Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

target_include_directories(mbed-ce-client-for-azure
    PUBLIC
        azure-uhttp-c/inc
)

target_sources(mbed-ce-client-for-azure
    PRIVATE
        azure-uhttp-c/src/uhttp.c
)
