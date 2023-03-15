/**
 * @file mcubupdate_handler.cpp
 * @brief Implementation of ContentHandler API for MCUboot firmware upgrade.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include "aduc/hash_utils.h"
#include "aduc/mcubupdate_handler.hpp"
#include "aduc/logging.h"
#include "aduc/parser_utils.h"
#include "aduc/string_c_utils.h"
#include "aduc/types/update_content.h"  // ADUC_FileEntity
#include "aduc/workflow_data_utils.h"
#include "aduc/workflow_utils.h"
#include <azure_c_shared_utility/azure_base64.h>
#include <azure_c_shared_utility/sha.h>
#include <string>

#include "mbed.h"               // for Mbed OS
#include "kvstore_global_api/kvstore_global_api.h"

#include "bootutil/bootutil.h"  // for MCUboot
#include "bootutil/image.h"
#include "flash_map_backend/secondary_bd.h"
#include "sysflash/sysflash.h"

#include "http_request.h"       // for mbed-http
#include "https_request.h"
#include "NetworkInterface.h"

#include <stddef.h>             // for offsetof
#include <memory>               // for unique_ptr
#include <functional>           // for function

/* Default read block size for calculating image digest from secondary bd */
#define FWU_READ_BLOCK_DEFSIZE                      1024

/* KVStore key to in-storage struct OTA_NonVolatileImageUpgradeState_t */
#define OTA_IMAGE_UPDATE_STATE_KEY              "ota_image_update_state"

/* Stringize */
#define STR_EXPAND(tok) #tok
#define STR(tok) STR_EXPAND(tok)

/* Convert to KVStore default fully-qualified key path */
#define KV_DEF_FQ_KEY(KEY)                      \
    "/" STR(MBED_CONF_STORAGE_DEFAULT_KV) "/" KEY

/* Maximum characters of installed criteria, excluding tailing null character */
#define INSTALLEDCRITERIA_MAXCHAR               64

/*-----------------------------------------------------------*/

/* In-storage struct for holding OTA PAL/MCUboot FWU states which need to be non-volatile to cross reset cycle */
typedef struct {
    /* MCUboot version of stage, non-secure image */
    bool                stageVersion_valid;
    struct image_version    stageVersion;

    /* Flag for install rebooted */
    bool                installRebooted_valid;
    bool                installRebooted;

    /* ADU stage installed criteria */
    bool                stageInstalledCriteria_valid;
    char                stageInstalledCriteria[INSTALLEDCRITERIA_MAXCHAR + 1];

    /* Mark the following area is reserved for not being cleared */
    uint32_t            reserved;

    /* ADU persistent installed criteria */
    bool                persistentInstalledCriteria_valid;
    char                persistentInstalledCriteria[INSTALLEDCRITERIA_MAXCHAR + 1];
} OTA_NonVolatileImageUpgradeState_t;

/* Routines to operate in-storage OTA_NonVolatileImageUpgradeState_t struct */
static bool nvImgUpgSt_reset(bool includeReserved);
static bool nvImgUpgSt_setStageVersion(struct image_version *stageVersion);
static bool nvImgUpgSt_setInstallRebooted(bool installRebooted);
static bool nvImgUpgSt_setStageInstalledCriteria(const char *installedCriteria);
static bool nvImgUpgSt_settleInstalledCriteria(void);
static bool nvImgUpgSt_installed(bool *confirmed);
static bool nvImgUpgSt_installRebooted(bool *installRebooted);
static bool nvImgUpgSt_persistentInstalledCriteria(char *installedCriteria, size_t installedCriteria_maxlen);
static bool nvImgUpgSt_setAll(const OTA_NonVolatileImageUpgradeState_t *imageUpgradeState);
static bool nvImgUpgSt_getAll(OTA_NonVolatileImageUpgradeState_t *imageUpgradeState);

/* Helper class for updating OTA_NonVolatileImageUpgradeState_t immediately after reboot */
class Update_NVImgUpgSt_PostReboot
{
public:
    Update_NVImgUpgSt_PostReboot();
};

Update_NVImgUpgSt_PostReboot::Update_NVImgUpgSt_PostReboot()
{
    /* Indicate install rebooted */
    bool installRebooted = false;
    if (nvImgUpgSt_installRebooted(&installRebooted) &&
        !installRebooted) {
        nvImgUpgSt_setInstallRebooted(true);
    }

    /* Try to confirm MCUboot firmware upgrade anyway for "test"
     * swap type because ADU doesn't define self-test flow. */
    bool confirmed = false;
    if (nvImgUpgSt_installed(&confirmed) && !confirmed) {
        /* Mark the image with index 0 in the primary slot as confirmed. 
         * The system will continue booting into the image in the primary
         * slot until told to boot from a different slot. */
        boot_set_confirmed();
    }

    /* Settle ADU installed criteria only after MCUboot firmware upgrade has confirmed. */
    if (nvImgUpgSt_installed(&confirmed)) {
        if (confirmed) {
            /* MCUboot firmware upgrade has confirmed.
             * Make ADU stage installed criteria become persistent. */
            nvImgUpgSt_settleInstalledCriteria();
            nvImgUpgSt_reset(false);
        } else {
            /* MCUboot firmware upgrade hasn't confirmed for some error.
             * Re-restart for image revert. */
            nvImgUpgSt_reset(false);
            NVIC_SystemReset();
        }
    }
}

/* Confirm firmware upgrade when MCUboot upgrade strategy is SWAP through C++ global object constructor */
Update_NVImgUpgSt_PostReboot update_nvImgUpgSt_postReboot;

/*-----------------------------------------------------------*/

/** Read program unit of mbed::BlockDevice
 *
 * @param bd                Target mbed::BlockDevice
 * @param progunit          Destination buffer to place read program unit
 * @param progunit_size     Program unit size
 * @param readblock         Helper buffer to place read block
 * @param readblock_size    Read block size
 * @param offset            Offset in bytes to indicate which program unit to read
 * 
 * @return mbed::BlockDevice API return code
 */
static int bd_read_program_unit(mbed::BlockDevice *bd,
                                void *progunit,
                                size_t progunit_size,
                                void *readblock,
                                size_t readblock_size,
                                size_t offset);

/*-----------------------------------------------------------*/

/**
 * @brief Network interface for mbed-http. Can override by user application
 */
__attribute__((weak))
NetworkInterface *mbed_http_network = NetworkInterface::get_default_instance();

/*-----------------------------------------------------------*/

/* OTA operation control block. */
typedef struct
{
    /* MCUboot firmware update context: Active */
    struct fwu_active_s {
        struct image_header     image_header;
    } fwu_active;

    /* MCUboot firmware update context: Stage */
    struct fwu_stage_s {
        struct image_header     image_header;               // Cached image header on the fly
        BlockDevice *           secondary_bd;               // Secondary BlockDevice
        bool                    secondary_bd_inited;
        void *                  secondary_bd_progunit;      // Program unit buffer to cover unaligned last data block
        size_t                  secondary_bd_progunit_size;
        void *                  secondary_bd_readblock;     // Read block buffer which must align on read unit boundary
        size_t                  secondary_bd_readblock_size;
    } fwu_stage;

    /* Download progress */
    struct dl_prog_s {
        size_t                  offset;                     // Downloaded bytes
        size_t                  total_exp;                  // Expected total bytes to download
        size_t                  total_act;                  // Actual total bytes downloaded
    } dl_prog;

} OTA_OperationContext_t;

/*-----------------------------------------------------------*/

/**
 * @brief Constructor for the MCUbUpdate Handler Impl class.
 */
MCUbUpdateHandlerImpl::MCUbUpdateHandlerImpl()
    : otaCtx_opaque(nullptr)
{
}
    
/**
 * @brief Destructor for the MCUbUpdate Handler Impl class.
 */
MCUbUpdateHandlerImpl::~MCUbUpdateHandlerImpl() // override
{
    OTACtx_Deinit();

    /* About ADUC_Logging API
     *
     * Useing xlogging as backend, we needn't invoke ADUC_Logging_Init()/Uninit()
     * and then suffer the effort of enforcing their pairing. See:
     * https://github.com/Azure/iot-hub-device-update/blob/79ce3ba24c411d3b014226cd869e2b2d02159a20/src/logging/inc/aduc/logging.h#L74-L78
     */
    //ADUC_Logging_Uninit();
}

/**
 * @brief Creates a new MCUbUpdateHandlerImpl object and casts to a ContentHandler.
 * Note that there is no way to create a MCUbUpdateHandlerImpl directly.
 *
 * @return ContentHandler* MCUbUpdateHandlerImpl object as a ContentHandler.
 */
ContentHandler* MCUbUpdateHandlerImpl::CreateContentHandler()
{
    return new MCUbUpdateHandlerImpl();
}

/**
 * @brief Performs a download task.
 *
 * @return ADUC_Result The result of this download task.
 */
ADUC_Result MCUbUpdateHandlerImpl::Download(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { .ResultCode = ADUC_Result_Download_Success };
    ADUC_WorkflowHandle handle = workflowData->WorkflowHandle;
    int fileCount = 0;
    ADUC_FileEntity fileEntity;
    memset(&fileEntity, 0, sizeof(fileEntity));

    /* Abort on cancel requested */
    if (workflow_is_cancel_requested(handle))
    {
        result = this->Cancel(workflowData);
        goto done;
    }

    /* For 'nuvoton/mcubupdate:1', we're expecting 1 payload file. */
    fileCount = workflow_get_update_files_count(handle);
    if (fileCount != 1)
    {
        Log_Error("MCUbUpdate expecting one file. (%d)", fileCount);
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    /* Get upgrade firmware information */
    if (!workflow_get_update_file(handle, 0, &fileEntity))
    {
        Log_Error("Get upgrade firmware information failed");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    /* Show upgrade firmware information */
    Log_Info("Upgrade firmware: FileId %s", fileEntity.FileId);
    Log_Info("Upgrade firmware: DownloadUri %s", fileEntity.DownloadUri);
    Log_Info("Upgrade firmware: TargetFilename %s", fileEntity.TargetFilename);
    Log_Info("Upgrade firmware: SizeInBytes %d", fileEntity.SizeInBytes);

    /* OTA operation context */
    if (!OTACtx_Reinit()) {
        Log_Error("OTACtx_Reinit() failed");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }
    MBED_ASSERT(otaCtx_opaque != nullptr);
    OTA_OperationContext_t *otaCtx_inst; otaCtx_inst = static_cast<OTA_OperationContext_t *>(otaCtx_opaque);

    /* Get active image's version */
    {
        const struct image_header *header = (const struct image_header *) MCUBOOT_PRIMARY_SLOT_START_ADDR;
        if (header->ih_magic != IMAGE_MAGIC) {
            Log_Error("Active image header error: Magic: EXP 0x%08x ACT 0x%08" PRIx32, IMAGE_MAGIC, header->ih_magic);
            result = { .ResultCode = ADUC_Result_Failure };
            goto done;
        }
        memcpy(&(otaCtx_inst->fwu_active.image_header),
               header,
               sizeof(struct image_header));

        struct image_version *active_image_version = &(otaCtx_inst->fwu_active.image_header.ih_ver);
        Log_Info("Active image version: %d.%d.%d+%" PRIu32,
                 active_image_version->iv_major,
                 active_image_version->iv_minor,
                 active_image_version->iv_revision,
                 active_image_version->iv_build_num);
    }

    /* Initialize download progress */
    {
        otaCtx_inst->dl_prog.offset = 0;
        otaCtx_inst->dl_prog.total_exp = fileEntity.SizeInBytes;
        otaCtx_inst->dl_prog.total_act = 0;
    }

    /* Combine mbed-http download and install by chunk
     *
     * Manage dynamic objects with RAII
     */
    {
        bool isHttps = (fileEntity.DownloadUri[4] == 's') || 
            (fileEntity.DownloadUri[4] == 'S');

        std::unique_ptr<HttpsRequest> scoped_https_download_request;
        std::unique_ptr<HttpRequest> scoped_http_download_request;

        /* Note 'result' is captured by reference, so we can get callback returned result. */
        auto CombinedDownloadInstallTask = [&](const char *dl_data, uint32_t dl_length) {
            /* Cancel HTTPS/HTTP transfer on previous failure or cancel requested */
            if (IsAducResultCodeFailure(result.ResultCode) ||
                workflow_is_cancel_requested(handle)) {
                if (isHttps) {
                    if (scoped_https_download_request) {    // Check managed object for safe
                        scoped_https_download_request->cancel();
                    }
                } else {
                    if (scoped_http_download_request) {     // Check managed object for safe
                        scoped_http_download_request->cancel();
                    }
                }
                return;
            }

            result = this->CombinedDownloadInstall(workflowData, dl_data, dl_length);
        };

        /* Notes on passing body callback to mbed-http HttpsRequest/HttpRequest
         *
         * Easily break the mbed Callback requirement if we pass lambda expression or std::function straight which are bigger:
         * https://github.com/ARMmbed/mbed-os/blob/17dc3dc2e6e2817a8bd3df62f38583319f0e4fed/platform/include/platform/Callback.h#L639-L641
         *
         * With C++ RTTI disabled, std::function member function 'target' is not available,
         * so we cannot get its function pointer like below to pass:
         *
         * std::function<void(const char *, uint32_t)> CombinedDownloadInstallFunc = CombinedDownloadInstallTask;
         * void (*CombinedDownloadInstallFuncPtr)(const char *, uint32_t) = CombinedDownloadInstallFunc.target<void(const char *, uint32_t)>();
         * MBED_ASSERT(CombinedDownloadInstallFuncPtr != nullptr);
         *
         * Finally, we solve this problem using 2-level lambda expression to pass smaller lambda expression (catch less).
         */
        auto CombinedDownloadInstallTask_simple = [&](const char *dl_data, uint32_t dl_length) {
            CombinedDownloadInstallTask(dl_data, dl_length);
        };

        /* Distinguish HTTPS/HTTP */
        if (isHttps) {
            scoped_https_download_request.reset(new HttpsRequest(mbed_http_network,
                                                                 nullptr,   // TODO: CA certificate
                                                                 HTTP_GET,
                                                                 fileEntity.DownloadUri,
                                                                 CombinedDownloadInstallTask_simple));

            /* Start HTTP download (blocking call) */
            HttpResponse* http_response = scoped_https_download_request->send();
            if (!http_response && !workflow_is_cancel_requested(handle)) {
                Log_Error("mbed-http failed: Error code %d", scoped_https_download_request->get_error());
                result = { .ResultCode = ADUC_Result_Failure };
                goto done;
            }
        } else {
            scoped_http_download_request.reset(new HttpRequest(mbed_http_network,
                                                               HTTP_GET,
                                                               fileEntity.DownloadUri,
                                                               CombinedDownloadInstallTask_simple));

            /* Start HTTP download (blocking call) */
            HttpResponse* http_response = scoped_http_download_request->send();
            if (!http_response && !workflow_is_cancel_requested(handle)) {
                Log_Error("mbed-http failed: Error code %d", scoped_http_download_request->get_error());
                result = { .ResultCode = ADUC_Result_Failure };
                goto done;
            }
        }

        /* Abort on cancel requested */
        if (workflow_is_cancel_requested(handle)) {
            result = this->Cancel(workflowData);
            goto done;
        }

        /* Check callback returned result */
        if (IsAducResultCodeFailure(result.ResultCode)) {
            goto done;
        }

        /* Check download length */
        otaCtx_inst->dl_prog.total_act = otaCtx_inst->dl_prog.offset;
        if (otaCtx_inst->dl_prog.total_act != otaCtx_inst->dl_prog.total_exp) {
            Log_Error("HTTP download: Expected %d bytes, but actual %d bytes", otaCtx_inst->dl_prog.total_exp, otaCtx_inst->dl_prog.total_act);
            result = { .ResultCode = ADUC_Result_Failure };
            goto done;
        }

        /* HTTP download completed */
        Log_Info("HTTP download: Completed %d/%d bytes", otaCtx_inst->dl_prog.total_act, otaCtx_inst->dl_prog.total_exp);
    }

    /* Verify signature */
    if (!VerifySignature(workflowData, &fileEntity)) {
        Log_Error("VerifySignature() failed");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

done:
    ADUC_FileEntity_Uninit(&fileEntity);
    return result;
}

/**
 * @brief Performs a backup task.
 *
 * @return ADUC_Result The result of this backup task (always success)
 */
ADUC_Result MCUbUpdateHandlerImpl::Backup(const tagADUC_WorkflowData* workflowData)
{
    UNREFERENCED_PARAMETER(workflowData);
    ADUC_Result result = { .ResultCode = ADUC_Result_Backup_Success_Unsupported, .ExtendedResultCode = 0 };
    Log_Info("MCUbUpdate backup & restore is not supported. (no-op)");
    return result;
}

/**
 * @brief Performs a install task.
 *
 * @return ADUC_Result The result of this install task
 */
ADUC_Result MCUbUpdateHandlerImpl::Install(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { .ResultCode = ADUC_Result_Install_Success };

    /* Download and install tasks are combined into Download function above,
     * so Install function here is trivial. */

    return result;
}

/**
 * @brief Performs a apply task.
 *
 * @return ADUC_Result The result of this apply task
 */
ADUC_Result MCUbUpdateHandlerImpl::Apply(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { .ResultCode = ADUC_Result_Apply_Success };
    ADUC_WorkflowHandle handle = workflowData->WorkflowHandle;

    /* Stage installedCriteria */
    char* installedCriteria = workflow_get_installed_criteria(handle);
    if (IsNullOrEmpty(installedCriteria))
    {
        workflow_set_result_details(workflowData->WorkflowHandle, "Property 'installedCriteria' in handlerProperties is missing or empty.");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    /* Abort on cancel requested */
    if (workflow_is_cancel_requested(handle))
    {
        result = this->Cancel(workflowData);
        goto done;
    }

    /* Keep stage installedCriteria across reboot */
    if (!nvImgUpgSt_setStageInstalledCriteria(installedCriteria)) {
        Log_Info("nvImgUpgSt_setStageInstalledCriteria() failed");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    /* Mark secondary image pending, non-permanent to enable image revert */
    if (boot_set_pending(false) != 0) {
        Log_Info("boot_set_pending() failed: Mark secondary image pending");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    /* Indicate not reboot yet for install */
    if (!nvImgUpgSt_setInstallRebooted(false)) {
        Log_Error("nvImgUpgSt_setInstallRebooted(false) failed");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    /* Request reboot to go MCUboot image swap */
    result = { .ResultCode = ADUC_Result_Apply_RequiredReboot };
    workflow_request_reboot(handle);

done:
    workflow_free_string(installedCriteria);
    return result;
}

/**
 * @brief Performs a restore task.
 *
 * @return ADUC_Result The result of this restore task
 */
ADUC_Result MCUbUpdateHandlerImpl::Restore(const tagADUC_WorkflowData* workflowData)
{
    UNREFERENCED_PARAMETER(workflowData);
    ADUC_Result result = { .ResultCode = ADUC_Result_Restore_Success_Unsupported, .ExtendedResultCode = 0 };
    Log_Info("MCUbUpdate backup & restore is not supported. (no-op)");
    return result;
}

/**
 * @brief Performs a cancel task.
 *
 * @return ADUC_Result The result of this cancel task
 */
ADUC_Result MCUbUpdateHandlerImpl::Cancel(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { .ResultCode = ADUC_Result_Cancel_Success, .ExtendedResultCode = 0 };
    ADUC_WorkflowHandle handle = workflowData->WorkflowHandle;
    //ADUC_WorkflowHandle stepWorkflowHandle = nullptr;

    const char* workflowId = workflow_peek_id(handle);
    int workflowLevel = workflow_get_level(handle);
    int workflowStep = workflow_get_step_index(handle);

    Log_Info(
        "Requesting cancel operation (workflow id '%s', level %d, step %d).", workflowId, workflowLevel, workflowStep);
    if (!workflow_request_cancel(handle))
    {
        Log_Error(
            "Cancellation request failed. (workflow id '%s', level %d, step %d)",
            workflowId,
            workflowLevel,
            workflowStep);
        result.ResultCode = ADUC_Result_Cancel_UnableToCancel;
    }

    return result;
}

/**
 * @brief Check whether the current device state satisfies specified workflow data.
 *
 * @return ADUC_Result The result based on evaluating the workflow data
 */
ADUC_Result MCUbUpdateHandlerImpl::IsInstalled(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { ADUC_Result_IsInstalled_NotInstalled };
    ADUC_WorkflowHandle handle = workflowData->WorkflowHandle;

    /* Stage installedCriteria */
    char* installedCriteria = workflow_get_installed_criteria(handle);
    if (IsNullOrEmpty(installedCriteria))
    {
        workflow_set_result_details(handle, "Property 'installedCriteria' in handlerProperties is missing or empty.");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    char installedCriteria_persistent[INSTALLEDCRITERIA_MAXCHAR + 1];
    if (!nvImgUpgSt_persistentInstalledCriteria(installedCriteria_persistent,
                                                INSTALLEDCRITERIA_MAXCHAR + 1)) {
        Log_Warn("No installed criteria settled down. Maybe it is the first time for ADU.");
        goto done;
    }

    /* IsInstalled meaning
     *
     * Per review on agent_workflow.c, when IsInstalled is true, the update
     * with installedCriteria has completed, that is, download, install, apply,
     * and reboot all done.
     *
     * https://github.com/Azure/iot-hub-device-update/blob/2d1f03671d45be1e55b89c940514ffe91b3227e0/src/adu_workflow/src/agent_workflow.c#L391-L401
     */
    if (strcmp(installedCriteria, installedCriteria_persistent) != 0)
    {
        Log_Info("Installed criteria %s was not installed, the current installed criteria is %s", installedCriteria, installedCriteria_persistent);
        goto done;
    }

    Log_Info("Installed criteria %s was installed", installedCriteria);
    result = { ADUC_Result_IsInstalled_Installed };

done:
    workflow_free_string(installedCriteria);
    return result;
}

/*-----------------------------------------------------------*/

ADUC_Result MCUbUpdateHandlerImpl::CombinedDownloadInstall(const tagADUC_WorkflowData* workflowData,
                                                           const char *dl_data,
                                                           uint32_t dl_length)
{
    ADUC_Result result = { .ResultCode = ADUC_Result_Download_Success };
    ADUC_WorkflowHandle handle = workflowData->WorkflowHandle;

    /* OTA operation context */
    MBED_ASSERT(otaCtx_opaque != nullptr);
    OTA_OperationContext_t *otaCtx_inst; otaCtx_inst = static_cast<OTA_OperationContext_t *>(otaCtx_opaque);

    Log_Info("HTTP download: %d/%d", otaCtx_inst->dl_prog.offset, otaCtx_inst->dl_prog.total_exp);

    /* Catch MCUBOOT header at offset 0 and store it in buffer for later use. */
    if (otaCtx_inst->dl_prog.offset < sizeof(struct image_header)) {
        size_t image_header_offset = otaCtx_inst->dl_prog.offset;
        uint8_t *image_header_beg = (uint8_t *) &(otaCtx_inst->fwu_stage.image_header);
        uint8_t *image_header_pos = image_header_beg + image_header_offset;
        size_t image_header_todo = sizeof(struct image_header) - image_header_offset;
        if (image_header_todo > dl_length) {
            image_header_todo = dl_length;
        }

        memcpy(image_header_pos,
               dl_data,
               image_header_todo);
        image_header_pos += image_header_todo;

        /* Catch MCUBOOT header completely */
        if ((image_header_pos - image_header_beg) == sizeof(struct image_header)) {
            if (otaCtx_inst->fwu_stage.image_header.ih_magic != IMAGE_MAGIC) {
                Log_Error("Invalid MCUBOOT header magic");
                result = { .ResultCode = ADUC_Result_Failure };
                goto done;
            }

            Log_Info("Image header: padded header size=%d, image size=%" PRIu32 ", protected TLV size=%d",
                     otaCtx_inst->fwu_stage.image_header.ih_hdr_size,
                     otaCtx_inst->fwu_stage.image_header.ih_img_size,
                     otaCtx_inst->fwu_stage.image_header.ih_protect_tlv_size);

            struct image_version *stage_image_version = &(otaCtx_inst->fwu_stage.image_header.ih_ver);
            Log_Info("Stage image version: %d.%d.%d+%" PRIu32,
                     stage_image_version->iv_major,
                     stage_image_version->iv_minor,
                     stage_image_version->iv_revision,
                     stage_image_version->iv_build_num);

            /* Save stage version in NV to check installed or not on reboot */
            if (!nvImgUpgSt_setStageVersion(&otaCtx_inst->fwu_stage.image_header.ih_ver)) {
                Log_Error("nvImgUpgSt_setStageVersion() failed");
                result = { .ResultCode = ADUC_Result_Failure };
                goto done;
            }
        }
    }

    /* secondary bd ready for program? */
    if (otaCtx_inst->fwu_stage.secondary_bd == nullptr ||
        !otaCtx_inst->fwu_stage.secondary_bd_inited) {
        Log_Error("Secondary BlockDevice not ready for program");
        result = { .ResultCode = ADUC_Result_Failure };
        goto done;
    }

    /* Write through BlockDevice program() */

    MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd_progunit);
    MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd_progunit_size);

    const uint8_t *fwu_data; fwu_data = (const uint8_t *) dl_data;
    /* NOTE: fwu_offset can start from other than 0 */
    size_t fwu_offset; fwu_offset = 0 + otaCtx_inst->dl_prog.offset;
    size_t fwu_rmn; fwu_rmn = dl_length;
    size_t fwu_todo;
    size_t fwu_offset_alignup;
    int rc; rc = 0;

    /* Program first data which doesn't align on program unit boundary */
    fwu_offset_alignup = ((fwu_offset + otaCtx_inst->fwu_stage.secondary_bd_progunit_size - 1) / otaCtx_inst->fwu_stage.secondary_bd_progunit_size) * otaCtx_inst->fwu_stage.secondary_bd_progunit_size;
    fwu_todo = fwu_offset_alignup - fwu_offset;
    if (fwu_todo > fwu_rmn) {
        fwu_todo = fwu_rmn;
    }
    MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd_progunit_size > fwu_todo);

    if (fwu_todo) {
        /* Read first program unit */
        rc = bd_read_program_unit(otaCtx_inst->fwu_stage.secondary_bd,
                                  otaCtx_inst->fwu_stage.secondary_bd_progunit,
                                  otaCtx_inst->fwu_stage.secondary_bd_progunit_size,
                                  otaCtx_inst->fwu_stage.secondary_bd_readblock,
                                  otaCtx_inst->fwu_stage.secondary_bd_readblock_size,
                                  fwu_offset);
        if (rc != 0) {
            Log_Error("bd_read_program_unit(offset=%d) failed: rc=%d",
                      fwu_offset, rc);
            result = { .ResultCode = ADUC_Result_Failure };
            goto done;
        }

        /* Move unaligned data to first program unit */
        uint8_t *fwu_data_first = (uint8_t *) otaCtx_inst->fwu_stage.secondary_bd_progunit;
        size_t fwu_offset_first = (fwu_offset / otaCtx_inst->fwu_stage.secondary_bd_progunit_size) * otaCtx_inst->fwu_stage.secondary_bd_progunit_size;
        size_t fwu_todo_first = otaCtx_inst->fwu_stage.secondary_bd_progunit_size;
        memcpy(fwu_data_first + (fwu_offset - fwu_offset_first), fwu_data, fwu_todo);
        fwu_data += fwu_todo;
        fwu_offset += fwu_todo;
        fwu_rmn -= fwu_todo;

        /* Program first program unit */
        rc = otaCtx_inst->fwu_stage.secondary_bd->program(fwu_data_first,
                                                          fwu_offset_first,
                                                          fwu_todo_first);
        if (rc != 0) {
            Log_Error("Secondary BlockDevice program(addr=%d, size=%d) failed: %d",
                      fwu_offset_first, fwu_todo_first, rc);
            result = { .ResultCode = ADUC_Result_Failure };
            goto done;
        }
    }

    /* Program data which aligns on program unit boundary */
    fwu_todo = (fwu_rmn / otaCtx_inst->fwu_stage.secondary_bd_progunit_size) * otaCtx_inst->fwu_stage.secondary_bd_progunit_size;
    if (fwu_todo) {
        rc = otaCtx_inst->fwu_stage.secondary_bd->program(fwu_data,
                                                          fwu_offset,
                                                          fwu_todo);
        if (rc != 0) {
            Log_Error("Secondary BlockDevice program(addr=%d, size=%d) failed: %d",
                      fwu_offset, fwu_todo, rc);
            result = { .ResultCode = ADUC_Result_Failure };
            goto done;
        }
        fwu_data += fwu_todo;
        fwu_offset += fwu_todo;
        fwu_rmn -= fwu_todo;
    }

    /* Program last data which doesn't align on program unit boundary */
    fwu_todo = fwu_rmn;
    if (fwu_todo) {
        MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd_progunit_size > fwu_todo);

        /* Read last program unit */
        rc = bd_read_program_unit(otaCtx_inst->fwu_stage.secondary_bd,
                                  otaCtx_inst->fwu_stage.secondary_bd_progunit,
                                  otaCtx_inst->fwu_stage.secondary_bd_progunit_size,
                                  otaCtx_inst->fwu_stage.secondary_bd_readblock,
                                  otaCtx_inst->fwu_stage.secondary_bd_readblock_size,
                                  fwu_offset);
        if (rc != 0) {
            Log_Error("bd_read_program_unit(offset=%d) failed: rc=%d",
                      fwu_offset, rc);
            result = { .ResultCode = ADUC_Result_Failure };
            goto done;
        }

        /* Move unaligned data to last program unit */
        uint8_t *fwu_data_last = (uint8_t *) otaCtx_inst->fwu_stage.secondary_bd_progunit;
        size_t fwu_offset_last = fwu_offset;
        size_t fwu_todo_last = otaCtx_inst->fwu_stage.secondary_bd_progunit_size;
        memcpy(fwu_data_last, fwu_data, fwu_todo);
        fwu_data += fwu_todo;
        fwu_offset += fwu_todo;
        fwu_rmn -= fwu_todo;

        /* Program last program unit */
        rc = otaCtx_inst->fwu_stage.secondary_bd->program(fwu_data_last,
                                                          fwu_offset_last,
                                                          fwu_todo_last);
        if (rc != 0) {
            Log_Error("Secondary BlockDevice program(addr=%d, size=%d) failed: %d",
                      fwu_offset_last, fwu_todo_last, rc);
            result = { .ResultCode = ADUC_Result_Failure };
            goto done;
        }
    }

    /* Advance download offset */
    MBED_ASSERT(fwu_rmn == 0);
    otaCtx_inst->dl_prog.offset += dl_length;

done:
    return result;
}

bool MCUbUpdateHandlerImpl::VerifySignature(const tagADUC_WorkflowData* workflowData,
                                            void* fileEntity_opaque)
{
    MBED_ASSERT(fileEntity_opaque != nullptr);
    ADUC_FileEntity &fileEntity = *static_cast<ADUC_FileEntity*>(fileEntity_opaque);

    MBED_ASSERT(otaCtx_opaque != nullptr);
    OTA_OperationContext_t *otaCtx_inst = static_cast<OTA_OperationContext_t *>(otaCtx_opaque);

    USHAContext shaCtx;
    uint8_t *shaDigest = nullptr;
    STRING_HANDLE h_shaDigestBase64 = nullptr;
    bool rc_ret = true;

    if (fileEntity.HashCount) {
        SHAversion shaVersion;
        int shaDigestSize;
        char *hashType;
        char *hashValueBase64;

        hashType = ADUC_HashUtils_GetHashType(fileEntity.Hash, fileEntity.HashCount, 0);
        if (hashType == nullptr) {
            Log_Error("ADUC_HashUtils_GetHashType(index=0) failed");
            rc_ret = false;
            goto cleanup;
        }

        if (!ADUC_HashUtils_GetShaVersionForTypeString(hashType, &shaVersion)) {
            Log_Error("FileEntity for %s has unsupported hash type %s",
                      fileEntity.TargetFilename,
                      hashType);
            rc_ret = false;
            goto cleanup;
        }

        if (USHAReset(&shaCtx, shaVersion) != 0) {
            Log_Error("Error in SHA Reset, SHAversion: %d", shaVersion);
            rc_ret = false;
            goto cleanup;
        };

        /* Assert secondary bd ready for read */
        MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd != nullptr);
        MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd_inited);        
        MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd_readblock);
        MBED_ASSERT(otaCtx_inst->fwu_stage.secondary_bd_readblock_size);

        /* Read from secondary bd to calculate image digest */
        uint8_t *fwu_data;
        size_t fwu_offset;
        size_t fwu_rmn;
        size_t fwu_todo;

        fwu_data = (uint8_t *) otaCtx_inst->fwu_stage.secondary_bd_readblock;
        /* NOTE: fwu_offset can start from other than 0 */
        fwu_offset = 0;
        fwu_rmn = fileEntity.SizeInBytes;

        /* First or middle chunks which align on read block boundary */
        while (fwu_rmn) {
            fwu_todo = fwu_rmn;

            /* Limit to one read block */
            if (fwu_todo > otaCtx_inst->fwu_stage.secondary_bd_readblock_size) {
                fwu_todo = otaCtx_inst->fwu_stage.secondary_bd_readblock_size;
            }

            /* Unaligned last chunk */
            if (fwu_todo < otaCtx_inst->fwu_stage.secondary_bd_readblock_size) {
                break;
            }

            /* Note buffer size is both aligned and actual */
            int rc_bd = otaCtx_inst->fwu_stage.secondary_bd->read(fwu_data,
                                                                  fwu_offset,
                                                                  fwu_todo);
            if (rc_bd != 0) {
                LogError("Secondary BlockDevice read(addr=%d, size=%d) failed: %d",
                         fwu_offset,
                         fwu_todo,
                         rc_bd);
                rc_ret = false;
                goto cleanup;
            }

            if (USHAInput(&shaCtx, fwu_data, fwu_todo) != 0) {
                Log_Error("Error in SHA Input, SHA version: %d", shaVersion);
                rc_ret = false;
                goto cleanup;
            };

            /* Next data */
            fwu_offset += fwu_todo;
            fwu_rmn -= fwu_todo;
        }

        /* Last chunk which doesn't align on read block boundary */
        if (fwu_rmn) {
            fwu_todo = fwu_rmn;
            MBED_ASSERT(fwu_todo < otaCtx_inst->fwu_stage.secondary_bd_readblock_size);

            /* Note buffer size is aligned instead of actual */
            int rc_bd = otaCtx_inst->fwu_stage.secondary_bd->read(fwu_data,
                                                                  fwu_offset,
                                                                  otaCtx_inst->fwu_stage.secondary_bd_readblock_size);
            if (rc_bd != 0) {
                LogError("Secondary BlockDevice read(addr=%d, size=%d) failed: %d",
                         fwu_offset,
                         otaCtx_inst->fwu_stage.secondary_bd_readblock_size,
                         rc_bd);
                rc_ret = false;
                goto cleanup;
            }

            if (USHAInput(&shaCtx, fwu_data, fwu_todo) != 0) {
                Log_Error("Error in SHA Input, SHA version: %d", shaVersion);
                rc_ret = false;
                goto cleanup;
            };

            /* Next data */
            fwu_offset += fwu_todo;
            fwu_rmn -= fwu_todo;
        }

        /* SHA digest buffer */
        shaDigestSize = USHAHashSize(shaVersion);
        shaDigest = (uint8_t *) malloc(shaDigestSize);

        /* SHA digest */
        if (USHAResult(&shaCtx, shaDigest) != 0) {
            Log_Error("USHAResult() failed");
            rc_ret = false;
            goto cleanup;
        }

        /* Base64-encoded SHA digest: Calculate over secondary bd */
        h_shaDigestBase64 = Azure_Base64_Encode_Bytes((unsigned char*) shaDigest, shaDigestSize);
        if (h_shaDigestBase64 == nullptr) {
            Log_Error("Azure_Base64_Encode_Bytes() failed");
            rc_ret = false;
            goto cleanup;
        }

        /* Base64-encoded SHA digest: Expected */
        hashValueBase64 = ADUC_HashUtils_GetHashValue(fileEntity.Hash, fileEntity.HashCount, 0);
        if (hashValueBase64 == nullptr) {
            Log_Error("ADUC_HashUtils_GetHashValue(index=0) failed");
            rc_ret = false;
            goto cleanup;
        }

        /* Compare Base64-encoded SHA digest */
        if (strcmp(hashValueBase64, STRING_c_str(h_shaDigestBase64)) != 0) {
            Log_Error("Invalid Hash: SHAversion: %d: EXP %s, ACT %s",
                      shaVersion,
                      hashValueBase64,
                      STRING_c_str(h_shaDigestBase64));
            rc_ret = false;
            goto cleanup;
        }
    }

cleanup:
    free(shaDigest);
    STRING_delete(h_shaDigestBase64);

    return rc_ret;
}

bool MCUbUpdateHandlerImpl::OTACtx_Reinit(void)
{
    OTACtx_Deinit();
    MBED_ASSERT(otaCtx_opaque == nullptr);

    bool rc_ret = true;

    /* Instantiate OTA operation context */
    OTA_OperationContext_t *otaCtx_inst = new OTA_OperationContext_t;

    /* Clean, zero-initialized struct */
    memset(otaCtx_inst, 0x00, sizeof(OTA_OperationContext_t));

    /* Reset non-volatile image state */
    if (!nvImgUpgSt_reset(false)) {
        Log_Error("nvImgUpgSt_reset() failed");
        rc_ret = false;
        goto cleanup;
    }

    /* Prepare secondary bd */
    {
        /* Get secondary bd */
        otaCtx_inst->fwu_stage.secondary_bd = get_secondary_bd();
        if (otaCtx_inst->fwu_stage.secondary_bd == nullptr) {
            Log_Error("get_secondary_bd() failed");
            rc_ret = false;
            goto cleanup;
        }

        /* Initialize secondary bd */
        int rc_bd = otaCtx_inst->fwu_stage.secondary_bd->init();
        if (rc_bd != 0) {
            Log_Error("Secondary BlockDevice init() failed: -%08x", -rc_bd);
            rc_ret = false;
            goto cleanup;
        }
        otaCtx_inst->fwu_stage.secondary_bd_inited = true;

        otaCtx_inst->fwu_stage.secondary_bd_progunit_size = otaCtx_inst->fwu_stage.secondary_bd->get_program_size();
        otaCtx_inst->fwu_stage.secondary_bd_progunit = malloc(otaCtx_inst->fwu_stage.secondary_bd_progunit_size);

        size_t read_size = otaCtx_inst->fwu_stage.secondary_bd->get_read_size();
        otaCtx_inst->fwu_stage.secondary_bd_readblock_size = FWU_READ_BLOCK_DEFSIZE;
        if (FWU_READ_BLOCK_DEFSIZE < read_size) {
            otaCtx_inst->fwu_stage.secondary_bd_readblock_size = read_size;
        }
        otaCtx_inst->fwu_stage.secondary_bd_readblock = malloc(otaCtx_inst->fwu_stage.secondary_bd_readblock_size);

        size_t second_bd_size = otaCtx_inst->fwu_stage.secondary_bd->size();
        Log_Info("Secondary BlockDevice size: %d (bytes)", second_bd_size);

        /* Erase secondary bd */
        rc_bd = otaCtx_inst->fwu_stage.secondary_bd->erase(0, second_bd_size);
        if (rc_bd != 0) {
            Log_Error("Secondary BlockDevice erase() failed: -%08x", -rc_bd);
            rc_ret = false;
            goto cleanup;
        }
    }

    /* Success */
    otaCtx_opaque = otaCtx_inst;

cleanup:

    if (!rc_ret) {
        /* Failure */
        delete otaCtx_inst;
        otaCtx_inst = nullptr;
    }

    return rc_ret;
}

void MCUbUpdateHandlerImpl::OTACtx_Deinit(void)
{
    if (otaCtx_opaque == nullptr) {
        return;
    }

    OTA_OperationContext_t *otaCtx_inst = static_cast<OTA_OperationContext_t *>(otaCtx_opaque);

    /* Deinit secondary bd */
    if (otaCtx_inst->fwu_stage.secondary_bd) {
        if (otaCtx_inst->fwu_stage.secondary_bd_readblock) {
            free(otaCtx_inst->fwu_stage.secondary_bd_readblock);
            otaCtx_inst->fwu_stage.secondary_bd_readblock = nullptr;
            otaCtx_inst->fwu_stage.secondary_bd_readblock_size = 0;
        }

        if (otaCtx_inst->fwu_stage.secondary_bd_progunit) {
            free(otaCtx_inst->fwu_stage.secondary_bd_progunit);
            otaCtx_inst->fwu_stage.secondary_bd_progunit = nullptr;
            otaCtx_inst->fwu_stage.secondary_bd_progunit_size = 0;
        }

        if (otaCtx_inst->fwu_stage.secondary_bd_inited) {
            otaCtx_inst->fwu_stage.secondary_bd->deinit();
            otaCtx_inst->fwu_stage.secondary_bd_inited = false;
        }
        otaCtx_inst->fwu_stage.secondary_bd = nullptr;
    }

    delete otaCtx_inst;
    otaCtx_inst = nullptr;
    otaCtx_opaque = nullptr;
}
    
/*-----------------------------------------------------------*/

static bool nvImgUpgSt_reset(bool includeReserved)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (includeReserved || !nvImgUpgSt_getAll(&imageUpgradeState)) {
        memset(&imageUpgradeState, 0x00, sizeof(OTA_NonVolatileImageUpgradeState_t));
    } else {
        static_assert(offsetof(OTA_NonVolatileImageUpgradeState_t, reserved) < sizeof(OTA_NonVolatileImageUpgradeState_t),
                      "Invalid reserved region offset in OTA_NonVolatileImageUpgradeState_t");
        memset(&imageUpgradeState, 0x00, offsetof(OTA_NonVolatileImageUpgradeState_t, reserved));
    }

    return nvImgUpgSt_setAll(&imageUpgradeState);
}

static bool nvImgUpgSt_setStageVersion(struct image_version *stageVersion)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (!nvImgUpgSt_getAll(&imageUpgradeState)) {
        return false;
    }

    memcpy(&(imageUpgradeState.stageVersion),
           stageVersion,
           sizeof(struct image_version));
    imageUpgradeState.stageVersion_valid = true;

    return nvImgUpgSt_setAll(&imageUpgradeState);
}

static bool nvImgUpgSt_setInstallRebooted(bool installRebooted)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (!nvImgUpgSt_getAll(&imageUpgradeState)) {
        return false;
    }

    imageUpgradeState.installRebooted = installRebooted;
    imageUpgradeState.installRebooted_valid = true;

    return nvImgUpgSt_setAll(&imageUpgradeState);
}

static bool nvImgUpgSt_setStageInstalledCriteria(const char *installedCriteria)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (!nvImgUpgSt_getAll(&imageUpgradeState)) {
        return false;
    }

    size_t len = strlen(installedCriteria);
    if (len > INSTALLEDCRITERIA_MAXCHAR) {
        return false;
    }

    memcpy(imageUpgradeState.stageInstalledCriteria,
           installedCriteria,
           len + 1);
    imageUpgradeState.stageInstalledCriteria_valid = true;

    return nvImgUpgSt_setAll(&imageUpgradeState);
}

/**
 * @brief Make stage installed criteria become persistent
 *
 * @note Stage installed criteria will be cleared on success.
 */
static bool nvImgUpgSt_settleInstalledCriteria(void)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (!nvImgUpgSt_getAll(&imageUpgradeState)) {
        return false;
    }

    if (!imageUpgradeState.stageInstalledCriteria_valid) {
        return false;
    }

    size_t len = strlen(imageUpgradeState.stageInstalledCriteria);
    if (len > INSTALLEDCRITERIA_MAXCHAR) {
        return false;
    }

    /* Copy stage installed criteria to persistent one */
    memcpy(imageUpgradeState.persistentInstalledCriteria,
           imageUpgradeState.stageInstalledCriteria,
           len + 1);
    imageUpgradeState.persistentInstalledCriteria_valid = true;

    /* Clear stage installed criteria */
    imageUpgradeState.stageInstalledCriteria_valid = false;
    memset(imageUpgradeState.stageInstalledCriteria, 
           0x00,
           INSTALLEDCRITERIA_MAXCHAR + 1);

    return nvImgUpgSt_setAll(&imageUpgradeState);
}

static bool nvImgUpgSt_installed(bool *confirmed)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (!nvImgUpgSt_getAll(&imageUpgradeState)) {
        return false;
    }

    if (!imageUpgradeState.installRebooted_valid ||
        !imageUpgradeState.installRebooted) {
        return false;
    }

    if (!imageUpgradeState.stageVersion_valid) {
        return false;
    }

    const struct image_header *active_header = (const struct image_header *) MCUBOOT_PRIMARY_SLOT_START_ADDR;
    const struct image_version *active_version = &(active_header->ih_ver);

    if (0 != memcmp(&(imageUpgradeState.stageVersion),
                    active_version,
                    sizeof(struct image_version))) {
        return false;
    }

    const struct flash_area *fap = nullptr;
    uint8_t image_ok = BOOT_FLAG_UNSET;

    if ((flash_area_open(FLASH_AREA_IMAGE_PRIMARY(0), &fap)) != 0) {
        return false;
    }

    /* Get value of image-ok flag of the image to check whether application
     * itself is already confirmed. */
    if (boot_read_image_ok(fap, &image_ok) != 0) {
        goto cleanup;
    }

cleanup:

    if (fap) {
        flash_area_close(fap);
        fap = nullptr;
    }

    if (image_ok == BOOT_FLAG_SET) {
        *confirmed = true;
        return true;
    } else {
        *confirmed = false;
        return true;
    }
}

static bool nvImgUpgSt_installRebooted(bool *installRebooted)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (!nvImgUpgSt_getAll(&imageUpgradeState)) {
        return false;
    }

    if (!imageUpgradeState.installRebooted_valid) {
        return false;
    }

    *installRebooted = imageUpgradeState.installRebooted;
    return true;
}

static bool nvImgUpgSt_persistentInstalledCriteria(char *installedCriteria, size_t installedCriteria_maxlen)
{
    OTA_NonVolatileImageUpgradeState_t imageUpgradeState;
    if (!nvImgUpgSt_getAll(&imageUpgradeState)) {
        return false;
    }

    if (!imageUpgradeState.persistentInstalledCriteria_valid) {
        return false;
    }

    size_t len = strlen(imageUpgradeState.persistentInstalledCriteria);
    if ((len + 1) > installedCriteria_maxlen) {
        return false;
    }

    memcpy(installedCriteria,
           imageUpgradeState.persistentInstalledCriteria,
           len + 1);

    return true;
}

static bool nvImgUpgSt_setAll(const OTA_NonVolatileImageUpgradeState_t *imageUpgradeState)
{
    int kv_status = kv_set(KV_DEF_FQ_KEY(OTA_IMAGE_UPDATE_STATE_KEY),
                           imageUpgradeState,
                           sizeof(OTA_NonVolatileImageUpgradeState_t),
                           0);
    if (kv_status != MBED_SUCCESS) {
        return false;
    }

    return true;
}

static bool nvImgUpgSt_getAll(OTA_NonVolatileImageUpgradeState_t *imageUpgradeState)
{
    size_t actual_size = 0;

    int kv_status = kv_get(KV_DEF_FQ_KEY(OTA_IMAGE_UPDATE_STATE_KEY),
                           imageUpgradeState,
                           sizeof(OTA_NonVolatileImageUpgradeState_t),
                           &actual_size);
    if (kv_status != MBED_SUCCESS) {
        return false;
    }
    if (actual_size != sizeof(OTA_NonVolatileImageUpgradeState_t)) {
        return false;
    }

    return true;
}

/*-----------------------------------------------------------*/

static int bd_read_program_unit(mbed::BlockDevice *bd,
                                void *progunit,
                                size_t progunit_size,
                                void *readblock,
                                size_t readblock_size,
                                size_t offset)
{
    /* Support only read block size >= program unit size */
    MBED_ASSERT(readblock_size >= progunit_size);

    int rc = 0;
    size_t readblock_aligndown = (offset / readblock_size) * readblock_size;
    size_t progunit_aligndown = (offset / progunit_size) * progunit_size;
    MBED_ASSERT(progunit_aligndown >= readblock_aligndown);

    rc = bd->read(readblock, readblock_aligndown, readblock_size);
    if (rc != 0) {
        return rc;
    }

    memcpy(progunit,
           ((const uint8_t *) readblock) + progunit_aligndown - readblock_aligndown,
           progunit_size);

    return rc;
}
