/**
 * @file extension_manager.cpp
 * @brief Implementation of ExtensionManager.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

#include "aduc/extension_manager.h"

#include <aduc/c_utils.h>
#include <aduc/calloc_wrapper.hpp> // ADUC::StringUtils::cstr_wrapper
// NUVOTON: For this port
#if 0
#include <aduc/component_enumerator_extension.hpp>
#include <aduc/content_downloader_extension.hpp>
#endif
#include <aduc/content_handler.hpp>
#include <aduc/contract_utils.h>
// NUVOTON: For no C++ exception implementation
#if 0
#include <aduc/exceptions.hpp>
#endif
// NUVOTON: For this port
#if 0
#include <aduc/exports/extension_export_symbols.h>
#endif
#include <aduc/extension_manager.hpp>
// NUVOTON: For static link implementation
#if 0
#include <aduc/extension_manager_helper.hpp>
#include <aduc/extension_utils.h>
#endif
#include <aduc/hash_utils.h> // for SHAversion
#include <aduc/logging.h>
#include <aduc/parser_utils.h>
#include <aduc/path_utils.h> // SanitizePathSegment
// NUVOTON: For static link implementation
#if 0
#include <aduc/plugin_exception.hpp>
#endif
#include <aduc/result.h>
#include <aduc/string_c_utils.h>
#include <aduc/string_handle_wrapper.hpp>
#include <aduc/string_utils.hpp>
#include <aduc/types/workflow.h> // ADUC_WorkflowHandle
#include <aduc/workflow_utils.h>

// NUVOTON: For steps handler
#include <aduc/steps_handler.hpp>

// NUVOTON: For MCUbUpdate handler
#if COMPONENT_AZIOT_OTA_PAL_MCUBOOT
#include "aduc/mcubupdate_handler.hpp"
#endif

#include <cstring>
#include <unordered_map>

// Note: this requires ${CMAKE_DL_LIBS}
// NUVOTON: For static link implementation
#if 0
#include <dlfcn.h>
#endif
#include <unistd.h>

// type aliases
using UPDATE_CONTENT_HANDLER_CREATE_PROC = ContentHandler* (*)(ADUC_LOG_SEVERITY logLevel);
using GET_CONTRACT_INFO_PROC = ADUC_Result (*)(ADUC_ExtensionContractInfo* contractInfo);
using WorkflowHandle = void*;
using ADUC::StringUtils::cstr_wrapper;

EXTERN_C_BEGIN
ExtensionManager_Download_Options Default_ExtensionManager_Download_Options = {
    .retryTimeout = 60 * 60 * 24 /* default : 24 hour */,
};
EXTERN_C_END

// Static members.
// NUVOTON: For static link implementation
#if 0
std::unordered_map<std::string, void*> ExtensionManager::_libs;
#endif
std::unordered_map<std::string, ContentHandler*> ExtensionManager::_contentHandlers;
// NUVOTON: For this port
#if 0
void* ExtensionManager::_contentDownloader;
ADUC_ExtensionContractInfo ExtensionManager::_contentDownloaderContractVersion;
void* ExtensionManager::_componentEnumerator;
ADUC_ExtensionContractInfo ExtensionManager::_componentEnumeratorContractVersion;
#endif

/**
 * @brief Loads extension shared library file.
 * @param extensionName An extension name.
 * @param extensionFolder An extension root folder.
 * @param extensionSubfolder An extension sub-folder.
 * @param extensionRegFileName An extension registration file name.
 * @param requiredFunctionName An required function.
 * @param facilityCode Facility code for extended error report.
 * @param componentCode Component code for extended error report.
 * @param libHandle A buffer for storing output extension library handle.
 * @return ADUC_Result contains result code and extended result code.
 */
ADUC_Result ExtensionManager::LoadExtensionLibrary(
    const char* extensionName,
    const char* extensionPath,
    const char* extensionSubfolder,
    const char* extensionRegFileName,
    const char* requiredFunction,
    int facilityCode,
    int componentCode,
    void** libHandle)
{
    ADUC_Result result{ ADUC_GeneralResult_Failure };
    Log_Error("No implementation for static-link");
    return result;
}

/**
 * @brief Loads UpdateContentHandler for specified @p updateType
 * @param updateType An update type string.
 * @param handler A buffer for storing an output UpdateContentHandler object.
 * @return ADUCResult contains result code and extended result code.
 * */
ADUC_Result
ExtensionManager::LoadUpdateContentHandlerExtension(const std::string& updateType, ContentHandler** handler)
{
    ADUC_Result result = { ADUC_Result_Failure };

    UPDATE_CONTENT_HANDLER_CREATE_PROC createUpdateContentHandlerExtensionFn = nullptr;
    GET_CONTRACT_INFO_PROC getContractInfoFn = nullptr;
    ADUC_ExtensionContractInfo contractInfo{};

    Log_Info("Loading Update Content Handler for '%s'.", updateType.c_str());

    if (handler == nullptr)
    {
        Log_Error("Invalid argument(s).");
        result.ExtendedResultCode =
            ADUC_ERC_EXTENSION_CREATE_FAILURE_INVALID_ARG(ADUC_FACILITY_EXTENSION_UPDATE_CONTENT_HANDLER, 0);
        return result;
    }

    // Try to find cached handler.
    *handler = nullptr;
    if (_contentHandlers.count(updateType) > 0)
    {
        {
            *handler = _contentHandlers.at(updateType);
        }
    }

    if (IsAducResultCodeSuccess(result.ResultCode) && *handler != nullptr)
    {
        goto done;
    }

    {
        // NUVOTON: Search update types with steps handler in iot-hub-device-update
        //          to confirm the list below is correct
        if (updateType == "microsoft/steps:1" ||
            updateType == "microsoft/update-manifest" ||
            updateType == "microsoft/update-manifest:4" ||
            updateType == "microsoft/update-manifest:5")
        {
            *handler = StepsHandlerImpl::CreateContentHandler();
        }
#if COMPONENT_AZIOT_OTA_PAL_MCUBOOT
        else if (updateType == "nuvoton/mcubupdate:1")
        {
            *handler = MCUbUpdateHandlerImpl::CreateContentHandler();
        }
#endif
        else
        {
            Log_Error("Unsupported Update Content Handler for '%s'.", updateType.c_str());
        }
    }

    if (*handler == nullptr)
    {
        result = { ADUC_GeneralResult_Failure, ADUC_ERC_UPDATE_CONTENT_HANDLER_CREATE_FAILURE_CREATE };
        goto done;
    }

    Log_Debug("Determining contract version for '%s'.", updateType.c_str());

    {
        contractInfo.majorVer = ADUC_V1_CONTRACT_MAJOR_VER;
        contractInfo.minorVer = ADUC_V1_CONTRACT_MINOR_VER;
    }

    (*handler)->SetContractInfo(contractInfo);

    Log_Debug("Caching new content handler for '%s'.", updateType.c_str());
    _contentHandlers.emplace(updateType, *handler);

    result = { ADUC_GeneralResult_Success };

done:
    if (result.ResultCode == 0)
    {
    }

    return result;
}

/**
 * @brief Sets UpdateContentHandler for specified @p updateType
 * @param updateType An update type string.
 * @param handler A ContentHandler object.
 * @return ADUCResult contains result code and extended result code.
 * */
ADUC_Result ExtensionManager::SetUpdateContentHandlerExtension(const std::string& updateType, ContentHandler* handler)
{
    ADUC_Result result = { ADUC_Result_Failure };

    Log_Info("Setting Content Handler for '%s'.", updateType.c_str());

    if (handler == nullptr)
    {
        Log_Error("Invalid argument(s).");
        result.ExtendedResultCode =
            ADUC_ERC_EXTENSION_CREATE_FAILURE_INVALID_ARG(ADUC_FACILITY_EXTENSION_UPDATE_CONTENT_HANDLER, 0);
        goto done;
    }

    // Remove existing one.
    _contentHandlers.erase(updateType);

    _contentHandlers.emplace(updateType, handler);

    result = { ADUC_GeneralResult_Success };

done:
    return result;
}

void ExtensionManager::UnloadAllUpdateContentHandlers()
{
    for (auto& contentHandler : _contentHandlers)
    {
        delete (contentHandler.second); // NOLINT(cppcoreguidelines-owning-memory)
    }

    _contentHandlers.clear();
}

/**
 * @brief This API unloads all handlers then unload all extension libraries.
 *
 */
void ExtensionManager::UnloadAllExtensions()
{
    // Make sure we unload every handlers first.
    UnloadAllUpdateContentHandlers();
}

void ExtensionManager::Uninit()
{
    ExtensionManager::UnloadAllExtensions();
}

ADUC_Result ExtensionManager::LoadContentDownloaderLibrary(void** contentDownloaderLibrary)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

ADUC_Result ExtensionManager::SetContentDownloaderLibrary(void* contentDownloaderLibrary)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

ADUC_Result ExtensionManager::GetContentDownloaderContractVersion(ADUC_ExtensionContractInfo* contractInfo)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

ADUC_Result ExtensionManager::GetComponentEnumeratorContractVersion(ADUC_ExtensionContractInfo* contractInfo)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

bool ExtensionManager::IsComponentsEnumeratorRegistered()
{
    return false;
}

ADUC_Result ExtensionManager::LoadComponentEnumeratorLibrary(void** componentEnumerator)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

void ExtensionManager::_FreeComponentsDataString(char* componentsJson)
{
}

/**
 * @brief Returns all components information in JSON format.
 * @param[out] outputComponentsData An output string containing components data.
 */
ADUC_Result ExtensionManager::GetAllComponents(std::string& outputComponentsData)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

/**
 * @brief Returns all components information in JSON format.
 * @param selector A JSON string contains name-value pairs used for selecting components.
 * @param[out] outputComponentsData An output string containing components data.
 */
ADUC_Result ExtensionManager::SelectComponents(const std::string& selector, std::string& outputComponentsData)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

ADUC_Result ExtensionManager::InitializeContentDownloader(const char* initializeData)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

ADUC_Result ExtensionManager::Download(
    const ADUC_FileEntity* entity,
    WorkflowHandle workflowHandle,
    ExtensionManager_Download_Options* options,
    ADUC_DownloadProgressCallback downloadProgressCallback)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Error("No implementation for this port");
    return result;
}

EXTERN_C_BEGIN

ADUC_Result ExtensionManager_InitializeContentDownloader(const char* initializeData)
{
    return ExtensionManager::InitializeContentDownloader(initializeData);
}

ADUC_Result ExtensionManager_Download(
    const ADUC_FileEntity* entity,
    ADUC_WorkflowHandle workflowHandle,
    ExtensionManager_Download_Options* options,
    ADUC_DownloadProgressCallback downloadProgressCallback)
{
    return ExtensionManager::Download(entity, workflowHandle, options, downloadProgressCallback);
}

/**
 * @brief Uninitializes the extension manager.
 */
void ExtensionManager_Uninit()
{
    ExtensionManager::Uninit();
}

EXTERN_C_END
