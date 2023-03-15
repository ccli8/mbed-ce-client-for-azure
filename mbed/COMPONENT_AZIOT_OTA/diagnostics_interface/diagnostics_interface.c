/**
 * @file diagnostics_interface.c
 * @brief Methods to communicate with "dtmi:azure:iot:deviceUpdateDiagnosticModel;1" interface.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include <diagnostics_interface.h>

#include "aduc/c_utils.h"
#include "aduc/client_handle_helper.h"
#include "aduc/d2c_messaging.h"
#include "aduc/logging.h"
#include "aduc/string_c_utils.h" // atoint64t
#include <ctype.h> // isalnum
#include <diagnostics_async_helper.h> // for DiagnosticsWorkflow_DiscoverAndUploadLogsAsync
// NUVOTON: For no file system implementation
#if 0
#include <diagnostics_config_utils.h> // for DiagnosticsWorkflowData, DiagnosticsWorkflow_InitFromFile
#endif
#include <pnp_protocol.h>
#include <stdlib.h>

// Name of the DiagnosticsInformation component that this device implements.
static const char g_diagnosticsPnPComponentName[] = "diagnosticInformation";

// this is the device-to-cloud property for the diagnostics interface
// the diagnostic client sends up the status of the upload to the service for
// it to interpret.
static const char g_diagnosticsPnPComponentAgentPropertyName[] = "agent";

// this is the cloud-to-device property for the diagnostics interface
// the diagnostics manager sends down properties necessary for the log upload
static const char g_diagnosticsPnPComponentOrchestratorPropertyName[] = "service";

/**
 * @brief Handle for Diagnostics component to communicate to service.
 */
ADUC_ClientHandle g_iotHubClientHandleForDiagnosticsComponent = NULL;

//
// DiagnosticsInterface methods
//

/**
 * @brief Create a DeviceInfoInterface object.
 *
 * @param componentContext Context object to use for related calls.
 * @param argc Count of arguments in @p argv
 * @param argv Command line parameters.
 * @return bool True on success.
 */
bool DiagnosticsInterface_Create(void** componentContext, int argc, char** argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    *componentContext = NULL;

    Log_Debug("Dummy DiagnosticsInterface_Create()");
    return true;
}

/**
 * @brief Called after connected to IoTHub (device client handler is valid).
 *
 * @param componentContext Context object from Create.
 */
void DiagnosticsInterface_Connected(void* componentContext)
{
    UNREFERENCED_PARAMETER(componentContext);

    Log_Info("DiagnosticsInterface is connected");
}

void DiagnosticsInterface_Destroy(void** componentContext)
{
    UNREFERENCED_PARAMETER(componentContext);

    Log_Debug("Dummy DiagnosticsInterface_Destroy()");
    return;
}

/**
 * @brief This function is called when the message is no longer being process.
 *
 * @param context The ADUC_D2C_Message object
 * @param status The message status.
 */
static void OnDiagnosticsD2CMessageCompleted(void* context, ADUC_D2C_Message_Status status)
{
    UNREFERENCED_PARAMETER(context);
    Log_Debug("Send message completed (status:%d)", status);
}

/**
 * @brief Function for sending a PnP message to the IotHub
 * @param clientHandle handle for the IotHub client handle to send the message to
 * @param jsonString message to send to the iothub
 * @returns True if success.
 */
static bool SendPnPMessageToIotHub(ADUC_ClientHandle clientHandle, const char* jsonString)
{
    UNREFERENCED_PARAMETER(clientHandle);
    UNREFERENCED_PARAMETER(jsonString);

    Log_Debug("Dummy SendPnPMessageToIotHub()");
    return true;
}

/**
 * @brief Function for sending a PnP message to the IotHub with status
 * @param clientHandle handle for the IotHub client handle to send the message to
 * @param jsonString message to send to the iothub
 * @param status value to set as the status to send up to the iothub
 * @param propertyVersion value for the version to send up to the iothub
 * @returns True if success.
 */
static bool SendPnPMessageToIotHubWithStatus(
    ADUC_ClientHandle clientHandle, const char* jsonString, int status, int propertyVersion)
{
    UNREFERENCED_PARAMETER(clientHandle);
    UNREFERENCED_PARAMETER(jsonString);
    UNREFERENCED_PARAMETER(status);
    UNREFERENCED_PARAMETER(propertyVersion);

    Log_Debug("Dummy SendPnPMessageToIotHubWithStatus()");
    return true;
}

void DiagnosticsOrchestratorUpdateCallback(
    ADUC_ClientHandle clientHandle, JSON_Value* propertyValue, int propertyVersion, void* context)
{
    UNREFERENCED_PARAMETER(clientHandle);
    UNREFERENCED_PARAMETER(propertyValue);
    UNREFERENCED_PARAMETER(propertyVersion);
    UNREFERENCED_PARAMETER(context);

    Log_Debug("Dummy DiagnosticsOrchestratorUpdateCallback()");
    return;
}

/**
 * @brief A callback for the diagnostic component's property update events.
 */
void DiagnosticsInterface_PropertyUpdateCallback(
    ADUC_ClientHandle clientHandle,
    const char* propertyName,
    JSON_Value* propertyValue,
    int version,
    ADUC_PnPComponentClient_PropertyUpdate_Context* sourceContext,
    void* context)
{
    UNREFERENCED_PARAMETER(sourceContext);
    if (strcmp(propertyName, g_diagnosticsPnPComponentOrchestratorPropertyName) == 0)
    {
        DiagnosticsOrchestratorUpdateCallback(clientHandle, propertyValue, version, context);
    }
    else
    {
        Log_Info("DiagnosticsInterface received unsupported property. (%s)", propertyName);
    }
}

/**
 * @brief Report a new state to the server.
 * @param result the result to be reported to the service
 * @param operationId the operation id associated with the result being reported
 */
void DiagnosticsInterface_ReportStateAndResultAsync(const Diagnostics_Result result, const char* operationId)
{
    UNREFERENCED_PARAMETER(result);
    UNREFERENCED_PARAMETER(operationId);

    Log_Debug("Dummy DiagnosticsInterface_ReportStateAndResultAsync()");
    return;
}
