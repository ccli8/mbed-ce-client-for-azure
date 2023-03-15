/**
 * @file mcubupdate_handler.hpp
 * @brief Defines MCUbUpdateHandlerImpl.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#ifndef ADUC_MCUBUPDATE_HANDLER_HPP
#define ADUC_MCUBUPDATE_HANDLER_HPP

#include "aduc/content_handler.hpp"

/**
 * @class MCUbUpdateHandlerImpl
 * @brief The mcubupdate specific implementation of ContentHandler interface.
 */
class MCUbUpdateHandlerImpl : public ContentHandler
{
public:
    static ContentHandler* CreateContentHandler();

    // Delete copy ctor, copy assignment, move ctor and move assignment operators.
    MCUbUpdateHandlerImpl(const MCUbUpdateHandlerImpl&) = delete;
    MCUbUpdateHandlerImpl& operator=(const MCUbUpdateHandlerImpl&) = delete;
    MCUbUpdateHandlerImpl(MCUbUpdateHandlerImpl&&) = delete;
    MCUbUpdateHandlerImpl& operator=(MCUbUpdateHandlerImpl&&) = delete;

    ~MCUbUpdateHandlerImpl() override;

    ADUC_Result Download(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Backup(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Install(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Apply(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Restore(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Cancel(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result IsInstalled(const tagADUC_WorkflowData* workflowData) override;

protected:
    // Protected constructor, must call CreateContentHandler factory method or from derived class
    MCUbUpdateHandlerImpl();

    // Callback for receiving mbed-http response body
    // For small memory device, download and install by chunk
    ADUC_Result CombinedDownloadInstall(const tagADUC_WorkflowData* workflowData,
                                        const char *dl_data,
                                        uint32_t dl_length);

    // Verify signature
    bool VerifySignature(const tagADUC_WorkflowData* workflowData,
                         void* fileEntity_opaque);

    // Internal OTA operation context
    bool OTACtx_Reinit(void);
    void OTACtx_Deinit(void);
    void *otaCtx_opaque;
};

#endif // ADUC_MCUBUPDATE_HANDLER_HPP
