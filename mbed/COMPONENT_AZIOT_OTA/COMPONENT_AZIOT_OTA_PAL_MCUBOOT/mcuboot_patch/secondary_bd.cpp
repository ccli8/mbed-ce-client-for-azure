/*
 * Copyright (c) 2022, Nuvoton Technology Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_PROVIDE_DEFAULT_SECONDARY_BLOCKDEVICE) && MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_PROVIDE_DEFAULT_SECONDARY_BLOCKDEVICE == 1UL

/* Mbed includes */
#include "mbed.h"
#include "FlashIAP/FlashIAPBlockDevice.h"
#include "blockdevice/SlicingBlockDevice.h"
#if COMPONENT_SPIF
#include "SPIFBlockDevice.h"
#endif
#if COMPONENT_NUSD
#include "NuSDFlashSimBlockDevice.h"
#endif

/* MCUboot includes */
#include "flash_map_backend/secondary_bd.h"

#if !defined(MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_DEFAULT_SECONDARY_BLOCKDEVICE_TYPE)
#define MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_DEFAULT_SECONDARY_BLOCKDEVICE_TYPE     DEFAULT
#endif

#define MCUBOOT_SECONDARY_BD_DEFAULT        0
#define MCUBOOT_SECONDARY_BD_FLASHIAP       1
#define MCUBOOT_SECONDARY_BD_SPIF           2
#define MCUBOOT_SECONDARY_BD_NUSD           3

#define MCUBOOT_SECONDARY_BD_TYPE_(X)   MCUBOOT_SECONDARY_BD_ ## X
#define MCUBOOT_SECONDARY_BD_TYPE(X)    MCUBOOT_SECONDARY_BD_TYPE_(X)

BlockDevice *get_secondary_bd(void) {
#   if MCUBOOT_SECONDARY_BD_TYPE(MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_DEFAULT_SECONDARY_BLOCKDEVICE_TYPE) == MCUBOOT_SECONDARY_BD_FLASHIAP
    static FlashIAPBlockDevice fbd(MCUBOOT_PRIMARY_SLOT_START_ADDR + MCUBOOT_SLOT_SIZE, MCUBOOT_SLOT_SIZE);
    return &fbd;
#   elif MCUBOOT_SECONDARY_BD_TYPE(MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_DEFAULT_SECONDARY_BLOCKDEVICE_TYPE) == MCUBOOT_SECONDARY_BD_SPIF
#       if TARGET_NUMAKER_IOT_M467
    /* Whether or not QE bit is set, explicitly disable WP/HOLD functions for safe. */
    static mbed::DigitalOut onboard_spi_wp(PI_13, 1);
    static mbed::DigitalOut onboard_spi_hold(PI_12, 1);
#       elif TARGET_NUMAKER_PFM_M487 || TARGET_NUMAKER_IOT_M487
    /* Whether or not QE bit is set, explicitly disable WP/HOLD functions for safe. */
    static mbed::DigitalOut onboard_spi_wp(PC_5, 1);
    static mbed::DigitalOut onboard_spi_hold(PC_4, 1);
#       endif
    static SPIFBlockDevice spif_bd(MBED_CONF_SPIF_DRIVER_SPI_MOSI,
                                   MBED_CONF_SPIF_DRIVER_SPI_MISO,
                                   MBED_CONF_SPIF_DRIVER_SPI_CLK,
                                   MBED_CONF_SPIF_DRIVER_SPI_CS);
    static mbed::SlicingBlockDevice sliced_bd(&spif_bd, 0x0, MCUBOOT_SLOT_SIZE);
    return &sliced_bd;
#   elif MCUBOOT_SECONDARY_BD_TYPE(MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_DEFAULT_SECONDARY_BLOCKDEVICE_TYPE) == MCUBOOT_SECONDARY_BD_NUSD
    /* For NUSD, use the flash-simulate variant to fit MCUboot flash map backend. */
    static NuSDFlashSimBlockDevice nusd_flashsim;
    static mbed::SlicingBlockDevice sliced_bd(&nusd_flashsim, 0x0, MCUBOOT_SLOT_SIZE);
    return &sliced_bd;
#   elif MCUBOOT_SECONDARY_BD_TYPE(MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_DEFAULT_SECONDARY_BLOCKDEVICE_TYPE) == MCUBOOT_SECONDARY_BD_DEFAULT
    mbed::BlockDevice* default_bd = mbed::BlockDevice::get_default_instance();
    static mbed::SlicingBlockDevice sliced_bd(default_bd, 0x0, MCUBOOT_SLOT_SIZE);
    return &sliced_bd;
#   else
#   error("Target not support: Block device for secondary slot")
#   endif
}

/*
 * With e.g. GCC linker option "--undefined=<LINK_FOO>", pull in this
 * object file anyway for being able to override weak symbol successfully
 * even though from static library. See:
 * https://stackoverflow.com/questions/42588983/what-does-the-gnu-ld-undefined-option-do
 *
 * NOTE: For C++ name mangling, 'extern "C"' is necessary to match the
 *       <LINK_FOO> symbol correctly.
 */
extern "C"
void LINK_SECONDARY_BD_CPP(void)
{
}

#endif  /* MBED_CONF_AZURE_CLIENT_OTA_MCUBOOT_PROVIDE_DEFAULT_SECONDARY_BLOCKDEVICE */
