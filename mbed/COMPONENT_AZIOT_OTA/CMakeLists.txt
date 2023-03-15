# Copyright (c) 2020 ARM Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if("AZIOT_OTA_PAL_MCUBOOT" IN_LIST MBED_TARGET_LABELS)
    add_subdirectory(COMPONENT_AZIOT_OTA_PAL_MCUBOOT)
endif()

target_include_directories(mbed-client-for-azure
    INTERFACE
        azure-iot-sdk-c_patch
        compiler_patch
        iot-hub-device-update_patch/agent/pnp_helper
        iot-hub-device-update_patch/agent_orchestration
        iot-hub-device-update_patch/update_manifest_handlers
        iot-hub-device-update_patch/update_manifest_handlers/steps_handler
        iot-hub-device-update_patch/extensions
        iot-hub-device-update_patch/extensions/extension_manager
        iot-hub-device-update_patch/utils/d2c_messaging
        iot-hub-device-update_patch/utils/retry_utils
        copy_n_patch/TOOLCHAIN_ARM
        mbed-http/http_parser
        mbed-http/source
        mbed_platform_layer
        posix_patch
)

if(${MBED_TOOLCHAIN} STREQUAL "ARM")
    target_include_directories(mbed-client-for-azure
        INTERFACE
            posix_patch/TOOLCHAIN_ARM
    )
elseif(${MBED_TOOLCHAIN} STREQUAL "GCC_ARM")
    target_include_directories(mbed-client-for-azure
        INTERFACE
            posix_patch/TOOLCHAIN_GCC_ARM
    )
elseif(${MBED_TOOLCHAIN} STREQUAL "IAR")
    target_include_directories(mbed-client-for-azure
        INTERFACE
            posix_patch/TOOLCHAIN_IAR
    )
endif()

target_sources(mbed-client-for-azure
    INTERFACE
        iot-hub-device-update_patch/agent/adu_core_interface/device_properties.c
        iot-hub-device-update_patch/adu_workflow/agent_workflow.cpp
        iot-hub-device-update_patch/iothub_communication_manager/iothub_communication_manager.c
        iot-hub-device-update_patch/update_manifest_handlers/steps_handler/steps_handler.cpp
        iot-hub-device-update_patch/extensions/extension_manager/extension_manager.cpp
        iot-hub-device-update_patch/utils/c_utils/string_c_utils.c
        iot-hub-device-update_patch/utils/config_utils/config_utils.c
        iot-hub-device-update_patch/utils/crypto_utils/crypto_lib.c
        iot-hub-device-update_patch/utils/d2c_messaging/d2c_messaging.cpp
        iot-hub-device-update_patch/utils/jws_utils/jws_utils.c
        iot-hub-device-update_patch/utils/retry_utils/retry_utils.c
        iot-hub-device-update_patch/utils/workflow_utils/workflow_utils.c
        diagnostics_interface/diagnostics_interface.c
        mbed-http/http_parser/http_parser.c
        mbed_platform_layer/mbed_adu_core_exports.cpp
        mbed_platform_layer/mbed_adu_core_impl.cpp
        mbed_platform_layer/mbed_device_info_exports.cpp
)
