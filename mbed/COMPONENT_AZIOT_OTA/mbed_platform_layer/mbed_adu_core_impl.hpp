/**
 * @file mbed_adu_core_impl.hpp
 * @brief Implements an ADUC "simulator" mode.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#ifndef MBED_ADU_CORE_IMPL_HPP
#define MBED_ADU_CORE_IMPL_HPP

#include <atomic>
//#include <exception>
#include <memory>
#include <string>
//#include <thread>
#include <unordered_map>

#include <aduc/adu_core_exports.h>
#include <aduc/content_handler.hpp>
//#include <aduc/exception_utils.hpp>
#include <aduc/logging.h>
#include <aduc/result.h>
#include "aduc/types/workflow.h"
#include "aduc/workflow_utils.h"

/* Mbed includes */
#include "mbed.h"
#include "rtos/Thread.h"

/* Address 'STD C/C++ library libspace not available' on Mbed OS for TOOLCHAIN_ARM
 *
 * There are two levels to address the issue:
 * 
 * 1. In user application mbed_app.json, OS_THREAD_LIBSPACE_NUM must be large
 *    enough to meet max threads that need libspace.
 *
 * 2. According to __user_perthread_libspace() below, libspace resource is not
 *    released after bindin (libspace leak):
 *    https://github.com/ARMmbed/mbed-os/blob/17dc3dc2e6e2817a8bd3df62f38583319f0e4fed/cmsis/device/rtos/TOOLCHAIN_ARM_STD/mbed_boot_arm_std.c#L122-L146
 *    To address above, we make thread control block memory (thread Id) for new
 *    worker thread of the same task type unchanged, so that libspace having bound
 *    to it can be reused without rebinding.
 *
 * Arm C/C++ Compiler libspace:
 * https://developer.arm.com/documentation/dui0475/m/the-arm-c-and-c---libraries/multithreaded-support-in-arm-c-libraries/use-of-the---user-libspace-static-data-area-by-the-c-libraries
 * https://developer.arm.com/documentation/dui0475/m/the-arm-c-and-c---libraries/multithreaded-support-in-arm-c-libraries/c-library-functions-to-access-subsections-of-the---user-libspace-static-data-area
 *
 */
#define NU_WORKAROUND_THREAD_LIBSPACE_UNBIND

namespace ADUC
{
/**
 * @brief Implementation class for UpdateAction handlers.
 */
class MbedPlatformLayer
{
public:
    static std::unique_ptr<MbedPlatformLayer> Create();

    ADUC_Result SetUpdateActionCallbacks(ADUC_UpdateActionCallbacks* data);

private:
    //
    // Static callbacks.
    //

    /**
     * @brief Implements Idle callback.
     *
     * @param token Opaque token.
     * @param workflowId Current workflow identifier.
     * @return ADUC_Result
     */
    static void IdleCallback(ADUC_Token token, const char* workflowId) noexcept
    {
        static_cast<MbedPlatformLayer*>(token)->Idle(workflowId);
    }

    /**
     * @brief Implements Download callback.
     *
     * @param token Opaque token.
     * @param workCompletionData Contains information on what to do when task is completed.
     * @param info ADUC_WorkflowDataToken with information on how to download.
     * @return ADUC_Result
     */
    static ADUC_Result DownloadCallback(
        ADUC_Token token, const ADUC_WorkCompletionData* workCompletionData, ADUC_WorkflowDataToken info) noexcept
    {
        return AsyncTaskCallback(token,
                                 workCompletionData,
                                 info,
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
                                 downloadWorker,
                                 downloadWorkerBlock);
#else
                                 downloadWorker);
#endif
    }

    /**
     * @brief Implements Backup callback.
     *
     * @param token Opaque token.
     * @param workCompletionData Contains information on what to do when task is completed.
     * @param info #ADUC_WorkflowData with information on how to backup.
     * @return ADUC_Result
     */
    static ADUC_Result BackupCallback(
        ADUC_Token token, const ADUC_WorkCompletionData* workCompletionData, ADUC_WorkflowDataToken info) noexcept
    {
        return AsyncTaskCallback(token,
                                 workCompletionData,
                                 info,
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
                                 backupWorker,
                                 backupWorkerBlock);
#else
                                 backupWorker);
#endif
    }

    /**
     * @brief Implements Install callback.
     *
     * @param token Opaque token.
     * @param workCompletionData Contains information on what to do when task is completed.
     * @param info #ADUC_WorkflowData with information on how to install.
     * @return ADUC_Result
     */
    static ADUC_Result InstallCallback(
        ADUC_Token token, const ADUC_WorkCompletionData* workCompletionData, ADUC_WorkflowDataToken info) noexcept
    {
        return AsyncTaskCallback(token,
                                 workCompletionData,
                                 info,
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
                                 installWorker,
                                 installWorkerBlock);
#else
                                 installWorker);
#endif
    }

    /**
     * @brief Implements Apply callback.
     *
     * @param token Opaque token.
     * @param workCompletionData Contains information on what to do when task is completed.
     * @param info #ADUC_WorkflowData with information on how to apply.
     * @return ADUC_Result
     */
    static ADUC_Result ApplyCallback(
        ADUC_Token token, const ADUC_WorkCompletionData* workCompletionData, ADUC_WorkflowDataToken info) noexcept
    {
        return AsyncTaskCallback(token,
                                 workCompletionData,
                                 info,
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
                                 applyWorker,
                                 applyWorkerBlock);
#else
                                 applyWorker);
#endif
    }

    /**
     * @brief Implements Restore callback.
     *
     * @param token Opaque token.
     * @param workCompletionData Contains information on what to do when task is completed.
     * @param info #ADUC_WorkflowData with information on how to restore.
     * @return ADUC_Result
     */
    static ADUC_Result RestoreCallback(
        ADUC_Token token, const ADUC_WorkCompletionData* workCompletionData, ADUC_WorkflowDataToken info) noexcept
    {
        return AsyncTaskCallback(token,
                                 workCompletionData,
                                 info,
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
                                 restoreWorker,
                                 restoreWorkerBlock);
#else
                                 restoreWorker);
#endif
    }

    /**
     * @brief Implements Cancel callback.
     *
     * @param token Opaque token.
     * @param info #ADUC_WorkflowData to cancel.
     */
    static void CancelCallback(ADUC_Token token, ADUC_WorkflowDataToken info) noexcept
    {
        Log_Info("CancelCallback called");
        const ADUC_WorkflowData* workflowData = static_cast<const ADUC_WorkflowData*>(info);

        static_cast<MbedPlatformLayer*>(token)->Cancel(workflowData);
    }

    static ADUC_Result IsInstalledCallback(ADUC_Token token, ADUC_WorkflowDataToken info) noexcept
    {
        Log_Info("IsInstalledCallback called");
        const ADUC_WorkflowData* workflowData = static_cast<const ADUC_WorkflowData*>(info);

        return static_cast<MbedPlatformLayer*>(token)->IsInstalled(workflowData);
    }

    /**
     * @brief Implements SandboxCreate callback.
     *
     * @param token Contains pointer to our class instance.
     * @param workflowId Unique workflow identifier.
     * @param workFolder Location of sandbox, or NULL if no sandbox is required, e.g. fileless OS.
     * Must be allocated using malloc.
     *
     * @return ADUC_Result
     */
    static ADUC_Result SandboxCreateCallback(ADUC_Token token, const char* workflowId, char* workFolder) noexcept
    {
        return static_cast<MbedPlatformLayer*>(token)->SandboxCreate(workflowId, workFolder);
    }

    /**
     * @brief Implements SandboxDestroy callback.
     *
     * @param token Contains pointer to our class instance.
     * @param workflowId Unique workflow identifier.
     * @param workFolder[in] Sandbox path.
     */
    static void SandboxDestroyCallback(ADUC_Token token, const char* workflowId, const char* workFolder) noexcept
    {
        static_cast<MbedPlatformLayer*>(token)->SandboxDestroy(workflowId, workFolder);
    }

    /**
     * @brief Implements DoWork callback.
     *
     * @param token Opaque token.
     * @param workflowData Current workflow data object.
     */
    static void DoWorkCallback(ADUC_Token token, ADUC_WorkflowDataToken workflowData) noexcept
    {
        // Not used in this example.
        // Not used in this code.
        UNREFERENCED_PARAMETER(token);
        UNREFERENCED_PARAMETER(workflowData);
    }

    /**
     * @brief Implements asynchronous task callback.
     *
     * @param token Opaque token.
     * @param workCompletionData Contains information on what to do when task is completed.
     * @param info ADUC_WorkflowDataToken with information on how to download.
     * @return ADUC_Result
     */
    static ADUC_Result AsyncTaskCallback(
        ADUC_Token token,
        const ADUC_WorkCompletionData* workCompletionData,
        ADUC_WorkflowDataToken info,
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
        rtos::Thread *&worker,
        void *workerBlock)
#else
        std::unique_ptr<rtos::Thread> &worker)
#endif
    {
        const char *taskName;
        ADUC_Result InProgressResult;
        if (&worker == &downloadWorker) {
            taskName = "Download worker";
            InProgressResult = ADUC_Result{ ADUC_Result_Download_InProgress };
        } else if (&worker == &backupWorker) {
            taskName = "Backup worker";
            InProgressResult = ADUC_Result{ ADUC_Result_Backup_InProgress };
        } else if (&worker == &installWorker) {
            taskName = "Install worker";
            InProgressResult = ADUC_Result{ ADUC_Result_Install_InProgress };
        } else if (&worker == &applyWorker) {
            taskName = "Apply worker";
            InProgressResult = ADUC_Result{ ADUC_Result_Apply_InProgress };
        } else if (&worker == &restoreWorker) {
            taskName = "Restore worker";
            InProgressResult = ADUC_Result{ ADUC_Result_Restore_InProgress };
        } else {
            Log_Error("%s() failed: Uncaught asynchronous task", __func__);
            return ADUC_Result{ ADUC_Result_Failure };
        }

        Log_Info("%s started", taskName);

        const ADUC_WorkflowData* workflowData = static_cast<const ADUC_WorkflowData*>(info);

        /* NOTE: Dangling references in closure
         *
         * ADU SDK's (1.0.1) implementation (linux_adu_core_impl.hpp or simulator_adu_core_impl.hpp)
         * captures local variables by reference, causing angling reference?
         * https://en.cppreference.com/w/cpp/language/lambda#Lambda_capture
         */

        /* Download task */
        auto downloadTask = [token, workCompletionData, workflowData] {
            const ADUC_Result result{ static_cast<MbedPlatformLayer*>(token)->Download(workflowData) };

            // Report result to main thread.
            workCompletionData->WorkCompletionCallback(workCompletionData->WorkCompletionToken, result, true /* isAsync */);

            Log_Info("Download worker thread finished");
        };

        /* Backup task */
        auto backupTask = [token, workCompletionData, workflowData] {
            const ADUC_Result result{ static_cast<MbedPlatformLayer*>(token)->Backup(workflowData) };

            // Report result to main thread.
            workCompletionData->WorkCompletionCallback(workCompletionData->WorkCompletionToken, result, true /* isAsync */);

            Log_Info("Backup worker thread finished");
        };

        /* Install task */
        auto installTask = [token, workCompletionData, workflowData] {
            const ADUC_Result result{ static_cast<MbedPlatformLayer*>(token)->Install(workflowData) };

            // Report result to main thread.
            workCompletionData->WorkCompletionCallback(workCompletionData->WorkCompletionToken, result, true /* isAsync */);

            Log_Info("Install worker thread finished");
        };

        /* Apply task */
        auto applyTask = [token, workCompletionData, workflowData] {
            const ADUC_Result result{ static_cast<MbedPlatformLayer*>(token)->Apply(workflowData) };

            // Report result to main thread.
            workCompletionData->WorkCompletionCallback(workCompletionData->WorkCompletionToken, result, true /* isAsync */);

            Log_Info("Apply worker thread finished");
        };

        /* Restore task */
        auto restoreTask = [token, workCompletionData, workflowData] {
            const ADUC_Result result{ static_cast<MbedPlatformLayer*>(token)->Restore(workflowData) };

            // Report result to main thread.
            workCompletionData->WorkCompletionCallback(workCompletionData->WorkCompletionToken, result, true /* isAsync */);

            Log_Info("Restore worker thread finished");
        };

#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
        if (worker) {
            worker->~Thread();
            worker = nullptr;
        }
        worker = new (workerBlock) rtos::Thread(osPriorityNormal,   // priority
                                                OS_STACK_SIZE,      // stack_size
                                                nullptr,            // stack_mem
                                                taskName);          // name
#else
        worker.reset(new rtos::Thread(osPriorityNormal, // priority
                                      OS_STACK_SIZE,    // stack_size
                                      nullptr,          // stack_mem
                                      taskName));       // name
#endif

        osStatus os_rc = osOK;
        if (&worker == &downloadWorker) {
            os_rc = worker->start(downloadTask);
        } else if (&worker == &backupWorker) {
            os_rc = worker->start(backupTask);
        } else if (&worker == &installWorker) {
            os_rc = worker->start(installTask);
        } else if (&worker == &applyWorker) {
            os_rc = worker->start(applyTask);
        } else if (&worker == &restoreWorker) {
            os_rc = worker->start(restoreTask);
        } else {
            Log_Error("%s() failed: Uncaught asynchronous task", __func__);
            return ADUC_Result{ ADUC_Result_Failure };
        }
        if (os_rc != osOK) {
            Log_Error("%s thread failed: Thread.start(): -0x%08x", taskName, -os_rc);
            return ADUC_Result{ ADUC_Result_Failure };
        }

        // Indicate that we've spun off a thread to do the actual work.
        return InProgressResult;
    }

    /**
     * @brief Download thread control block
     */
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
    static rtos::Thread *                   downloadWorker;
    static uint64_t                         downloadWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];
#else
    static std::unique_ptr<rtos::Thread>    downloadWorker;
#endif

    /**
     * @brief Backup thread control block
     */
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
    static rtos::Thread *                   backupWorker;
    static uint64_t                         backupWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];
#else
    static std::unique_ptr<rtos::Thread>    backupWorker;
#endif

    /**
     * @brief Install thread control block
     */
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
    static rtos::Thread *                   installWorker;
    static uint64_t                         installWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];
#else
    static std::unique_ptr<rtos::Thread>    installWorker;
#endif

    /**
     * @brief Apply thread control block
     */
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
    static rtos::Thread *                   applyWorker;
    static uint64_t                         applyWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];
#else
    static std::unique_ptr<rtos::Thread>    applyWorker;
#endif

    /**
     * @brief Restore thread control block
     */
#if defined(NU_WORKAROUND_THREAD_LIBSPACE_UNBIND)
    static rtos::Thread *                   restoreWorker;
    static uint64_t                         restoreWorkerBlock[(sizeof(rtos::Thread) + 7) / 8];
#else
    static std::unique_ptr<rtos::Thread>    restoreWorker;
#endif

    //
    // Implementation.
    //

    // Private constructor, must call Create factory method.
    MbedPlatformLayer() = default;

    void Idle(const char* workflowId);
    ADUC_Result Download(const ADUC_WorkflowData* workflowData);
    ADUC_Result Backup(const ADUC_WorkflowData* workflowData);
    ADUC_Result Install(const ADUC_WorkflowData* workflowData);
    ADUC_Result Apply(const ADUC_WorkflowData* workflowData);
    ADUC_Result Restore(const ADUC_WorkflowData* workflowData);
    void Cancel(const ADUC_WorkflowData* workflowData);

    ADUC_Result IsInstalled(const ADUC_WorkflowData* workflowData);

    /**
     * @brief Class implementation of SandboxCreate method.
     *
     * @param workflowId Unique workflow identifier.
     * @param workFolder Location of sandbox, or NULL if no sandbox is required, e.g. fileless OS.
     * Must be allocated using malloc.
     * @return ADUC_Result
     */
    ADUC_Result SandboxCreate(const char* workflowId, char* workFolder);

    /**
     * @brief Class implementation of SandboxDestroy method.
     *
     * @param workflowId Unique workflow identifier.
     * @param workFolder Sandbox path.
     */
    void SandboxDestroy(const char* workflowId, const char* workFolder);

    /**
     * @brief Was Cancel called?
     */
    std::atomic_bool _IsCancellationRequested{ false };
};
} // namespace ADUC

#endif // MBED_ADU_CORE_IMPL_HPP
