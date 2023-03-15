/**
 * @file mbed_adu_core_exports.cpp
 * @brief Implements exported methods for platform-specific ADUC agent code.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include "aduc/adu_core_exports.h"
#include "aduc/c_utils.h"
#include "aduc/logging.h"
//#include "aduc/process_utils.hpp"
#include "mbed_adu_core_impl.hpp"
#include <memory>
//#include <signal.h> // raise()
#include <string>
//#include <unistd.h> // sync()
#include <vector>

// Mbed includes
#include "mbed.h"

EXTERN_C_BEGIN

/**
 * @brief Register this platform layer and approriate callbacks for all update actions.
 *
 * @param data Information about this module (e.g. callback methods)
 * @return ADUC_Result Result code.
 */
ADUC_Result ADUC_RegisterPlatformLayer(ADUC_UpdateActionCallbacks* data, unsigned int argc, const char** argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    {
        std::unique_ptr<ADUC::MbedPlatformLayer> pImpl{ ADUC::MbedPlatformLayer::Create() };
        ADUC_Result result{ pImpl->SetUpdateActionCallbacks(data) };
        // The platform layer object is now owned by the UpdateActionCallbacks object.
        pImpl.release();
        return result;
    }
}

/**
 * @brief Unregister this module.
 *
 * @param token Token that was returned from #ADUC_RegisterPlatformLayer call.
 */
void ADUC_Unregister(ADUC_Token token)
{
    ADUC::MbedPlatformLayer* pImpl{ static_cast<ADUC::MbedPlatformLayer*>(token) };
    delete pImpl; // NOLINT(cppcoreguidelines-owning-memory)
}

/**
 * @brief Reboot the system.
 *
 * @returns int errno, 0 if success.
 */
int ADUC_RebootSystem()
{
    Log_Info("ADUC_RebootSystem called. Rebooting system.");

    mbed_event_queue()->call_in(std::chrono::seconds(3), NVIC_SystemReset);

    return 0;
}

/**
 * @brief Restart the ADU Agent.
 *
 * @returns int errno, 0 if success.
 */
int ADUC_RestartAgent()
{
    Log_Info("Restarting ADU Agent.");

    mbed_event_queue()->call_in(std::chrono::seconds(3), NVIC_SystemReset);

    return 0;
}

EXTERN_C_END
