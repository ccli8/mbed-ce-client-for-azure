# Copyright (c) 2020 ARM Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Encounter MAX_PATH limit on Windows. Specify shorter binary_dir
add_subdirectory(mcuboot/boot/bootutil ${CMAKE_BINARY_DIR}/mcuboot/boot/bootutil)
add_subdirectory(mcuboot/boot/mbed ${CMAKE_BINARY_DIR}/mcuboot/boot/mbed)

target_include_directories(mbed-ce-client-for-azure
    PRIVATE
        .
        mcubupdate_handler
)

target_sources(mbed-ce-client-for-azure
    PRIVATE
        mcuboot_patch/secondary_bd.cpp
        mcubupdate_handler/mcubupdate_handler.cpp
)

target_link_libraries(mbed-ce-client-for-azure
    PUBLIC
        mbed-mcuboot
        $<$<IN_LIST:COMPONENT_DATAFLASH=1,${MBED_TARGET_DEFINITIONS}>:mbed-storage-dataflash>
        $<$<IN_LIST:DEVICE_FLASH=1,${MBED_TARGET_DEFINITIONS}>:mbed-storage-flashiap>
        $<$<IN_LIST:COMPONENT_I2CEE=1,${MBED_TARGET_DEFINITIONS}>:mbed-storage-i2cee>
        $<$<IN_LIST:COMPONENT_OSPIF=1,${MBED_TARGET_DEFINITIONS}>:mbed-storage-ospif>
        $<$<IN_LIST:COMPONENT_QSPIF=1,${MBED_TARGET_DEFINITIONS}>:mbed-storage-qspif>
        $<$<IN_LIST:COMPONENT_SD=1,${MBED_TARGET_DEFINITIONS}>:mbed-storage-sd>
        $<$<IN_LIST:COMPONENT_SPIF=1,${MBED_TARGET_DEFINITIONS}>:mbed-storage-spif>
        $<$<IN_LIST:COMPONENT_NUSD=1,${MBED_TARGET_DEFINITIONS}>:nusd>
        mbed-storage-kv-global-api
)

# Link override object file from static library
if("MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_PROVIDE_DEFAULT_SECONDARY_BLOCKDEVICE=1" IN_LIST MBED_CONFIG_DEFINITIONS)
    if(${MBED_TOOLCHAIN} STREQUAL "GCC_ARM")
        target_link_options(mbed-ce-client-for-azure
            INTERFACE
                LINKER:--undefined=LINK_SECONDARY_BD_CPP
        )
    endif()
endif()
