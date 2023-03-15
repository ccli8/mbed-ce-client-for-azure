/**
 * @file mbed_adu_core_impl.cpp
 * @brief Implements adu core for Mbed OS.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include "mbed_adu_core_impl.hpp"

#include <chrono>
//#include <future> // this_thread

#include <cstring>
#include <vector>

#include "aduc/calloc_wrapper.hpp"
#include "aduc/extension_manager.hpp"
#include "aduc/logging.h"
#include "aduc/string_c_utils.h"
#include "aduc/string_utils.hpp"
#include "aduc/workflow_data_utils.h"
#include "aduc/workflow_utils.h"

using ADUC::MbedPlatformLayer;
using ADUC::StringUtils::cstr_wrapper;

// The update manifest handler type.
#define UPDATE_MANIFEST_DEFAULT_HANDLER "microsoft/update-manifest"

std::unique_ptr<MbedPlatformLayer> MbedPlatformLayer::Create()
{
    return std::unique_ptr<MbedPlatformLayer>{ new MbedPlatformLayer() };
}

/**
 * @brief Set the ADUC_UpdateActionCallbacks object
 *
 * @param data Object to set.
 * @return ADUC_Result ResultCode.
 */
ADUC_Result MbedPlatformLayer::SetUpdateActionCallbacks(ADUC_UpdateActionCallbacks* data)
{
    // Message handlers.
    data->IdleCallback = IdleCallback;
    data->DownloadCallback = DownloadCallback;
    data->BackupCallback = BackupCallback;
    data->InstallCallback = InstallCallback;
    data->ApplyCallback = ApplyCallback;
    data->RestoreCallback = RestoreCallback;
    data->CancelCallback = CancelCallback;

    data->IsInstalledCallback = IsInstalledCallback;

    data->SandboxCreateCallback = SandboxCreateCallback;
    data->SandboxDestroyCallback = SandboxDestroyCallback;

    data->DoWorkCallback = DoWorkCallback;

    // Opaque token, passed to callbacks.

    data->PlatformLayerHandle = this;

    return ADUC_Result{ ADUC_Result_Register_Success };
}

void MbedPlatformLayer::Idle(const char* workflowId)
{
    Log_Info("Now idle. workflowId: %s", workflowId);
    _IsCancellationRequested = false;
}

static ContentHandler* GetUpdateManifestHandler(const ADUC_WorkflowData* workflowData, ADUC_Result* result)
{
    ContentHandler* contentHandler = nullptr;

    ADUC_Result loadResult = {};

    // Starting from version 4, the top-level update manifest doesn't contains the 'updateType' property.
    // The manifest contains an Instruction (steps) data, which requires special processing.
    // For backword compatibility and avoid code complexity, for V4, we will process the top level update content
    // using 'microsoft/update-manifest:4'
    int updateManifestVersion = workflow_get_update_manifest_version(workflowData->WorkflowHandle);
    if (updateManifestVersion >= 4)
    {
        const cstr_wrapper updateManifestHandler{ ADUC_StringFormat(
            "microsoft/update-manifest:%d", updateManifestVersion) };

        Log_Info(
            "Try to load a handler for current update manifest version %d (handler: '%s')",
            updateManifestVersion,
            updateManifestHandler.get());

        loadResult =
            ExtensionManager::LoadUpdateContentHandlerExtension(updateManifestHandler.get(), &contentHandler);

        // If handler for the current manifest version is not available,
        // fallback to the V4 default handler.
        if (IsAducResultCodeFailure(loadResult.ResultCode))
        {
            loadResult = ExtensionManager::LoadUpdateContentHandlerExtension(
                UPDATE_MANIFEST_DEFAULT_HANDLER, &contentHandler);
        }
    }
    else
    {
        loadResult = { .ResultCode = ADUC_Result_Failure,
                       .ExtendedResultCode =
                           ADUC_ERC_UTILITIES_UPDATE_DATA_PARSER_UNSUPPORTED_UPDATE_MANIFEST_VERSION };
    }

    if (IsAducResultCodeFailure(loadResult.ResultCode))
    {
        contentHandler = nullptr;
        *result = loadResult;
    }

    return contentHandler;
}

/**
 * @brief Definitions of download/backup/install/apply/restore workers
 */
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
rtos::Thread *MbedPlatformLayer::downloadWorker = nullptr;
uint64_t MbedPlatformLayer::downloadWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];

rtos::Thread *MbedPlatformLayer::backupWorker = nullptr;
uint64_t MbedPlatformLayer::backupWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];

rtos::Thread *MbedPlatformLayer::installWorker = nullptr;
uint64_t MbedPlatformLayer::installWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];

rtos::Thread *MbedPlatformLayer::applyWorker = nullptr;
uint64_t MbedPlatformLayer::applyWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];

rtos::Thread *MbedPlatformLayer::restoreWorker = nullptr;
uint64_t MbedPlatformLayer::restoreWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];
#else
std::unique_ptr<rtos::Thread> MbedPlatformLayer::downloadWorker;
std::unique_ptr<rtos::Thread> MbedPlatformLayer::backupWorker;
std::unique_ptr<rtos::Thread> MbedPlatformLayer::installWorker;
std::unique_ptr<rtos::Thread> MbedPlatformLayer::applyWorker;
std::unique_ptr<rtos::Thread> MbedPlatformLayer::restoreWorker;
#endif

/**
 * @brief Class implementation of Download method.
 * @return ADUC_Result
 */
ADUC_Result MbedPlatformLayer::Download(const ADUC_WorkflowData* workflowData)
{
    ADUC_Result result{ ADUC_Result_Failure };
    ContentHandler* contentHandler = GetUpdateManifestHandler(workflowData, &result);

    if (contentHandler == nullptr)
    {
        goto done;
    }

    result = contentHandler->Download(workflowData);
    if (_IsCancellationRequested)
    {
        result = ADUC_Result{ ADUC_Result_Failure_Cancelled };
        _IsCancellationRequested = false; // For replacement, we can't call Idle so reset here
    }

done:
    return result;
}

/**
 * @brief Class implementation of Backup method.
 * @return ADUC_Result
 */
ADUC_Result MbedPlatformLayer::Backup(const ADUC_WorkflowData* workflowData)
{
    ADUC_Result result{ ADUC_Result_Failure };
    ContentHandler* contentHandler = GetUpdateManifestHandler(workflowData, &result);
    if (contentHandler == nullptr)
    {
        goto done;
    }

    result = contentHandler->Backup(workflowData);

    // If cancel is requested during backup, we will proceed to finish the backup.
    if (_IsCancellationRequested)
    {
        result = ADUC_Result{ ADUC_Result_Failure_Cancelled };
        _IsCancellationRequested = false; // For replacement, we can't call Idle so reset here
    }

done:
    return result;
}

/**
 * @brief Class implementation of Install method.
 * @return ADUC_Result
 */
ADUC_Result MbedPlatformLayer::Install(const ADUC_WorkflowData* workflowData)
{
    ADUC_Result result{ ADUC_Result_Failure };
    ContentHandler* contentHandler = GetUpdateManifestHandler(workflowData, &result);
    if (contentHandler == nullptr)
    {
        goto done;
    }

    result = contentHandler->Install(workflowData);
    if (_IsCancellationRequested)
    {
        result = ADUC_Result{ ADUC_Result_Failure_Cancelled };
        _IsCancellationRequested = false; // For replacement, we can't call Idle so reset here
    }

done:
    return result;
}

/**
 * @brief Class implementation of Apply method.
 * @return ADUC_Result
 */
ADUC_Result MbedPlatformLayer::Apply(const ADUC_WorkflowData* workflowData)
{
    ADUC_Result result{ ADUC_Result_Failure };
    ContentHandler* contentHandler = GetUpdateManifestHandler(workflowData, &result);
    if (contentHandler == nullptr)
    {
        goto done;
    }

    result = contentHandler->Apply(workflowData);
    if (_IsCancellationRequested)
    {
        result = ADUC_Result{ ADUC_Result_Failure_Cancelled };
        _IsCancellationRequested = false; // For replacement, we can't call Idle so reset here
    }

done:
    return result;
}

/**
 * @brief Class implementation of Restore method.
 * @return ADUC_Result
 */
ADUC_Result MbedPlatformLayer::Restore(const ADUC_WorkflowData* workflowData)
{
    ADUC_Result result{ ADUC_Result_Failure };
    ContentHandler* contentHandler = GetUpdateManifestHandler(workflowData, &result);
    if (contentHandler == nullptr)
    {
        goto done;
    }

    result = contentHandler->Restore(workflowData);

    // If cancel is requested during restore, it means that the user wants to cancel the deployment (which already failed),
    // so the agent should try to restore to the previous state - proceed to finish the restore.

done:
    return result;
}

/**
 * @brief Class implementation of Cancel method.
 */
void MbedPlatformLayer::Cancel(const ADUC_WorkflowData* workflowData)
{
    ADUC_Result result{ ADUC_Result_Failure };

    _IsCancellationRequested = true;

    ContentHandler* contentHandler = GetUpdateManifestHandler(workflowData, &result);
    if (contentHandler == nullptr)
    {
        Log_Error("Could not get content handler!");
        goto done;
    }

    // Since this is coming in from main thread, let the content handler know that a cancel has been requested
    // so that it can interrupt the current operation (download, install, apply) that's occurring on the
    // worker thread. Cancel on the contentHandler is blocking call and once content handler confirms the
    // operation has been cancelled, it returns success or failure for the cancel.
    // After each blocking Download, Install, Apply calls above into content handler, it checks if
    // _IsCancellationRequested is true and sets result to ADUC_Result_Failure_Cancelled
    result = contentHandler->Cancel(workflowData);
    if (IsAducResultCodeSuccess(result.ResultCode))
    {
        Log_Info("Successful cancel of workflowId: %s", workflow_peek_id(workflowData->WorkflowHandle));
    }
    else
    {
        Log_Warn("Failed to cancel workflowId: %s", workflow_peek_id(workflowData->WorkflowHandle));
    }

done:
    return;
}

/**
 * @brief Class implementation of the IsInstalled callback.
 * Calls into the content handler or step handler to determine if the update in the given workflow
 * is installed.
 *
 * @param workflowData The workflow data object.
 * @return ADUC_Result The result of the IsInstalled call.
 */
ADUC_Result MbedPlatformLayer::IsInstalled(const ADUC_WorkflowData* workflowData)
{
    ContentHandler* contentHandler = nullptr;

    if (workflowData == nullptr)
    {
        return ADUC_Result{ .ResultCode = ADUC_Result_Failure,
                            .ExtendedResultCode = ADUC_ERC_UPDATE_CONTENT_HANDLER_ISINSTALLED_FAILURE_NULL_WORKFLOW };
    }

    ADUC_Result result;
    contentHandler = GetUpdateManifestHandler(workflowData, &result);
    if (contentHandler == nullptr)
    {
        return ADUC_Result{ .ResultCode = ADUC_Result_Failure,
                            .ExtendedResultCode = ADUC_ERC_UPDATE_CONTENT_HANDLER_ISINSTALLED_FAILURE_BAD_UPDATETYPE };
    }

    return contentHandler->IsInstalled(workflowData);
}

ADUC_Result MbedPlatformLayer::SandboxCreate(const char* workflowId, char* workFolder)
{
    if (IsNullOrEmpty(workflowId))
    {
        Log_Error("Invalid workflowId passed to SandboxCreate! Uninitialized workflow?");
        return ADUC_Result{ .ResultCode = ADUC_Result_Failure };
    }

    const char *workFolder_ = IsNullOrEmpty(workFolder) ? "null" : workFolder;
    Log_Info("{%s} Creating dummy sandbox %s", workflowId, workFolder_);

    return ADUC_Result{ ADUC_Result_SandboxCreate_Success };
}

void MbedPlatformLayer::SandboxDestroy(const char* workflowId, const char* workFolder)
{
    if (IsNullOrEmpty(workflowId))
    {
        return;
    }

    const char *workFolder_ = IsNullOrEmpty(workFolder) ? "null" : workFolder;
    Log_Info("{%s} Deleting dummy sandbox: %s", workflowId, workFolder_);
}
