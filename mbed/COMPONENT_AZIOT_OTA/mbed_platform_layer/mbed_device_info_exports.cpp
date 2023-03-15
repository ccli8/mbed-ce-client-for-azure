/**
 * @file mbed_device_info_exports.cpp
 * @brief Implements exported methods for platform-specific device information code.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include <functional>
#include <unordered_map>

#include <cstdlib>
#include <cstring>

#include <aduc/device_info_exports.h>
#include <aduc/config_utils.h>

// Azure Device Update user configuration
#include MBED_CONF_AZURE_CLIENT_OTA_ADUC_USER_CONFIG_FILE

/**
 * @brief Alternative to strdup()
 *
 * strdup() is just POSIX standard, not C standard, and isn't available across all platforms.
 * Provide nu_strdup() as alternative to strdup().
 */
char *nu_strdup(const char *s1)
{
    size_t size = strlen(s1) + 1;
    char *str = (char *) malloc(size);
    memcpy(str, s1, size);

    return str;
}

#define STRDUP  nu_strdup

/**
 * @brief Get manufacturer
 * Company name of the device manufacturer.
 * This could be the same as the name of the original equipment manufacturer (OEM).
 * e.g. Contoso
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetManufacturer()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (!valueIsDirty)
    {
        return nullptr;
    }

    char* result = nullptr;

    ADUC_ConfigInfo config = {};
    if (ADUC_ConfigInfo_Init(&config, ADUC_CONF_FILE_PATH) && config.manufacturer != nullptr)
    {
        result = STRDUP(config.manufacturer);
    }
    else
    {
        // If file doesn't exist, or value wasn't specified, use build default.
        result = STRDUP(ADUC_DEVICEINFO_MANUFACTURER);
    }

    valueIsDirty = false;
    ADUC_ConfigInfo_UnInit(&config);
    return result;
}

/**
 * @brief Get device model.
 * Device model name or ID.
 * e.g. Surface Book 2
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetModel()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (!valueIsDirty)
    {
        return nullptr;
    }

    char* result = nullptr;
    ADUC_ConfigInfo config = {};
    if (ADUC_ConfigInfo_Init(&config, ADUC_CONF_FILE_PATH) && config.model != nullptr)
    {
        result = STRDUP(config.model);
    }
    else
    {
        // If file doesn't exist, or value wasn't specified, use build default.
        result = STRDUP(ADUC_DEVICEINFO_MODEL);
    }

    valueIsDirty = false;
    ADUC_ConfigInfo_UnInit(&config);
    return result;
}

/**
 * @brief Get operating system name.
 * Name of the operating system on the device.
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetOsName()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (valueIsDirty)
    {
        valueIsDirty = false;
        return STRDUP("Mbed OS");
    }

    // Value not expected to change again, so return nullptr;
    return nullptr;
}

/**
 * @brief Get software version.
 * Version of the software on your device.
 * This could be the version of your firmware.
 * e.g. 1.3.45
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetSwVersion()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (valueIsDirty)
    {
        valueIsDirty = false;
        return STRDUP(ADUC_DEVICEINFO_SW_VERSION);
    }

    // Value not expected to change again, so return nullptr;
    return nullptr;
}

/**
 * @brief Get processor architecture.
 * Architecture of the processor on the device.
 * e.g. x64
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetProcessorArchitecture()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (valueIsDirty)
    {
        valueIsDirty = false;
        return STRDUP("Cortex-M based");
    }

    // Value not expected to change again, so return nullptr;
    return nullptr;
}

/**
 * @brief Get processor manufacturer.
 * Name of the manufacturer of the processor on the device.
 * e.g. Intel
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetProcessorManufacturer()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (valueIsDirty)
    {
        valueIsDirty = false;
        return STRDUP("Nuvoton");
    }

    // Value not expected to change again, so return nullptr;
    return nullptr;
}

/**
 * @brief Get total memory.
 * Total available memory on the device in kilobytes.
 * e.g. 256000
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetTotalMemory()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (valueIsDirty)
    {
        valueIsDirty = false;
        return STRDUP("0");
    }

    // Value not expected to change again, so return nullptr;
    return nullptr;
}

/**
 * @brief Get total storage.
 * Total available storage on the device in kilobytes.
 * e.g. 2048000
 *
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
static char* DeviceInfo_GetTotalStorage()
{
    // Value must be returned at least once, so initialize to true.
    static bool valueIsDirty = true;
    if (valueIsDirty)
    {
        valueIsDirty = false;
        return STRDUP("0");
    }

    // Value not expected to change again, so return nullptr;
    return nullptr;
}

//
// Exported methods
//

EXTERN_C_BEGIN

/**
 * @brief Return a specific device information value.
 *
 * @param property Property to retrieve
 * @return char* Value of property allocated with malloc, or nullptr on error or value not changed since last call.
 */
char* DI_GetDeviceInformationValue(DI_DeviceInfoProperty property)
{
    char* value = nullptr;

    {
        const std::unordered_map<DI_DeviceInfoProperty, std::function<char*()>> simulationMap{
            { DIIP_Manufacturer, DeviceInfo_GetManufacturer },
            { DIIP_Model, DeviceInfo_GetModel },
            { DIIP_OsName, DeviceInfo_GetOsName },
            { DIIP_SoftwareVersion, DeviceInfo_GetSwVersion },
            { DIIP_ProcessorArchitecture, DeviceInfo_GetProcessorArchitecture },
            { DIIP_ProcessorManufacturer, DeviceInfo_GetProcessorManufacturer },
            { DIIP_TotalMemory, DeviceInfo_GetTotalMemory },
            { DIIP_TotalStorage, DeviceInfo_GetTotalStorage },
        };

        // Call the handler for the device info property to retrieve current value.
        value = simulationMap.at(property)();
    }

    return value;
}

EXTERN_C_END
