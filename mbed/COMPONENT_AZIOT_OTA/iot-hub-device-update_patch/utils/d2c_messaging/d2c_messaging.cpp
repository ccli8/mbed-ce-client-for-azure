/**
 * @file d2c_messaging.c
 * @brief Implements utilities for the Device Update Agent Device-to-Cloud messaging.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include "aduc/d2c_messaging.h"
#include "aduc/client_handle_helper.h"
#include "aduc/retry_utils.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
// NUVOTON: For no POSIX API
#if 0
#include <sys/param.h> // MIN/MAX
#else
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#include <time.h> // clock_gettime
#include <unistd.h>

// NUVOTON: Use Mbed OS mutex instead of pthread mutex
#include "mbed.h"
#include "rtos/Mutex.h"
// Make sure memory buffer is enough for placement new rtos::Mutex
static_assert(sizeof(((ADUC_D2C_Message_Processing_Context *) 0)->mutexBlock) >= sizeof(rtos::Mutex),
              "Size of rtos::Mutex underestimated");

#define DEFAULT_INITIAL_DELAY_MS 1000 // 1 second
#define DEFAULT_MAX_BACKOFF_TIME_MS (60 * 1000) // 60 seconds
#define DEFAULT_MAX_JITTER_PERCENT 5
#define MAX_RETRY_EXPONENT 9
#define FATAL_ERROR_WAIT_TIME_SEC 10 // 10 seconds
#define ONE_DAY_IN_SECONDS (1 * 24 * 60 * 60)

#if 0
static pthread_mutex_t s_pendingMessageStoreMutex = PTHREAD_MUTEX_INITIALIZER;
#else
static rtos::Mutex s_pendingMessageStoreMutex;
#endif
static bool s_core_initialized = false;

static ADUC_D2C_Message s_pendingMessageStore[ADUC_D2C_Message_Type_Max] = {};
static ADUC_D2C_Message_Processing_Context s_messageProcessingContext[ADUC_D2C_Message_Type_Max] = {};

static void ProcessMessage(ADUC_D2C_Message_Processing_Context* context);

static time_t GetTimeSinceEpochInSeconds()
{
    // NUVOTON: For no POSIX API. Use time() instead of clock_gettime(CLOCK_REALTIME) for only second accuracy.
#if 0
    struct timespec timeSinceEpoch;
    clock_gettime(CLOCK_REALTIME, &timeSinceEpoch);
    return timeSinceEpoch.tv_sec;
#else
    return time(NULL);
#endif
}

/**
 * @brief The retry strategy for all each http response status code from the Azure IoT Hub.
 */
static ADUC_D2C_HttpStatus_Retry_Info g_defaultHttpStatusRetryInfo[] = {
    /* Success responses, no retries needed */
    { .httpStatusMin = 200,
      .httpStatusMax = 299,
      .additionalDelaySecs = 0,
      .retryTimestampCalcFunc = NULL,
      .maxRetry = 0 },

    /* Bad Request, no retries needed */
    { .httpStatusMin = 400,
      .httpStatusMax = 400,
      .additionalDelaySecs = 0,
      .retryTimestampCalcFunc = NULL,
      .maxRetry = 0 },

    /* 'Too many requests / Throttled', additional wait 30 secs on top of regular backoff time */
    { .httpStatusMin = 429,
      .httpStatusMax = 429,
      .additionalDelaySecs = 30,
      .retryTimestampCalcFunc = ADUC_Retry_Delay_Calculator,
      .maxRetry = INT_MAX },

    /* Message sent to the IoT Hub exceeds the maximum allowable size for IoT Hub messages. (Not retry) */
    { .httpStatusMin = 413,
      .httpStatusMax = 413,
      .additionalDelaySecs = 30,
      .retryTimestampCalcFunc = ADUC_Retry_Delay_Calculator,
      .maxRetry = 0 },

    /* Catch all for client error responses*/
    { .httpStatusMin = 400,
      .httpStatusMax = 499,
      .additionalDelaySecs = 5,
      .retryTimestampCalcFunc = ADUC_Retry_Delay_Calculator,
      .maxRetry = INT_MAX },

    /* Could be related to throttled, additional wait 30 secs on top of regular backoff time */
    { .httpStatusMin = 503,
      .httpStatusMax = 503,
      .additionalDelaySecs = 30,
      .retryTimestampCalcFunc = ADUC_Retry_Delay_Calculator,
      .maxRetry = INT_MAX },

    /* Catch all for server error responses */
    { .httpStatusMin = 500,
      .httpStatusMax = 599,
      .additionalDelaySecs = 30,
      .retryTimestampCalcFunc = ADUC_Retry_Delay_Calculator,
      .maxRetry = INT_MAX },

    /* Catch all */
    { .httpStatusMin = 0,
      .httpStatusMax = INT_MAX,
      .additionalDelaySecs = 0,
      .retryTimestampCalcFunc = ADUC_Retry_Delay_Calculator,
      .maxRetry = INT_MAX },
};

/**
 * @brief The default retry strategy for all Device-to-Cloud message requests to the Azure IoT Hub.
 */
static ADUC_D2C_RetryStrategy g_defaultRetryStrategy = {
    .httpStatusRetryInfo = g_defaultHttpStatusRetryInfo,
    .httpStatusRetryInfoSize = sizeof(g_defaultHttpStatusRetryInfo) / sizeof(*g_defaultHttpStatusRetryInfo),

    /* By default, all D2C message are important and DU Agent should never give up. */
    .maxRetries = INT_MAX,

    /* Though, we shouldn't wait longer than a day to retry. */
    .maxDelaySecs = ONE_DAY_IN_SECONDS,

    // NUVOTON: For designator order doesn't match declaration order
#if 0
    /* backoff factor, 1000 milliseconds */
    .initialDelayUnitMilliSecs = DEFAULT_INITIAL_DELAY_MS,
#endif

    /* fallback value when regular calculation failed, 30 seconds */
    .fallbackWaitTimeSec = 30,

    // NUVOTON: For designator order doesn't match declaration order
#if 1
    /* backoff factor, 1000 milliseconds */
    .initialDelayUnitMilliSecs = DEFAULT_INITIAL_DELAY_MS,
#endif

    /* maximum jitter percentage used for calculating the jitter, (5 percent)*/
    .maxJitterPercent = DEFAULT_MAX_JITTER_PERCENT,
};

/**
 * Release resources allocated for the @p message and reset all message fields.
*/
static void DestroyMessageData(ADUC_D2C_Message* message)
{
    if (message == NULL)
    {
        return;
    }
    free(message->content);
    memset(message, 0, sizeof(ADUC_D2C_Message));
}

/**
 * @brief Set the message status, then call the message.statusChangedCallback (if supplied).
 *
 * @param message The message object.
 * @param status  Final message status
 */
void SetMessageStatus(ADUC_D2C_Message* message, ADUC_D2C_Message_Status status)
{
    if (message == NULL)
    {
        return;
    }
    message->status = status;
    if (message->statusChangedCallback)
    {
        message->statusChangedCallback(message, status);
    }
}

/**
 * @brief A helper function that is called when the message has reach it terminal state.
 *  This function calls SetMessageStatus() then DestroyMessage().
 *
 * @param message The message object.
 * @param status  The message status
 */
static void OnMessageProcessingCompleted(ADUC_D2C_Message* message, ADUC_D2C_Message_Status status)
{
    if (message == NULL || message->content == NULL)
    {
        return;
    }
    SetMessageStatus(message, status);
    if (message->completedCallback != NULL)
    {
        message->completedCallback(message, status);
    }
    DestroyMessageData(message);
}

/**
 * @brief The function that is called when a 'reported property' patch response is received from the IoT Hub.
 *
 * @param statusCode A HTTP Status Code
 * @param context A pointer to the ADUC_D2C_Message_Processing_Context object.
 *
 * @remark This function calls the ADUC_D2C_Message's responseCallback function to determine whether
 *  we need to retry sending the message.
 *
 *      If responseCallback returns true (a retry is needed), the default 'back off' algorithm will be used to
 *  determine the time for the next retry attempt.
 *
 *      Otherwise, the context.processed will be set to true to indicates that the message has been processed,
 *  thus no further action is required.
 */
static void DefaultIoTHubSendReportedStateCompletedCallback(int http_status_code, void* context)
{
    Log_Debug("context:0x%x", context);
    ADUC_D2C_Message_Processing_Context* message_processing_context = (ADUC_D2C_Message_Processing_Context*)context;
    int computed = false;
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_lock(&message_processing_context->mutex);
#else
    static_cast<rtos::Mutex *>(message_processing_context->mutex)->lock();
#endif
    message_processing_context->message.lastHttpStatus = http_status_code;

    // It's possible that the message has been destroy by ADUC_D2C_Messaging_Uninit().
    // In this case, we just abort here.
    if (message_processing_context->message.content == NULL)
    {
        Log_Debug("Message already been destroy. No op.");
        goto done;
    }

    // Note, stop processing the message if the responseCallback() returned false,
    // or http_status_code is >= 200 and < 300.
    // NUVOTON: goto cannot cross variable definition
#if 0
    bool success =
        (message_processing_context->message.responseCallback != NULL
         && !message_processing_context->message.responseCallback(http_status_code, message_processing_context))
        || ((http_status_code >= 200 && http_status_code < 300));

    time_t previousRetryTimeStamp = message_processing_context->nextRetryTimeStampEpoch;
#else
    bool success; success =
        (message_processing_context->message.responseCallback != NULL
         && !message_processing_context->message.responseCallback(http_status_code, message_processing_context))
        || ((http_status_code >= 200 && http_status_code < 300));

    time_t previousRetryTimeStamp; previousRetryTimeStamp = message_processing_context->nextRetryTimeStampEpoch;
#endif
    // Call the responseCallback to allow the message owner to make a decision whether
    // to continue trying, and specified the 'nextRetryTimestamp' if needed.
    if (success)
    {
        // The callback indicates that no retries needed.
        // We're done with this message.
        Log_Debug(
            "D2C message processed successfully (t:%d, r:%d, content:0x%x )",
            message_processing_context->type,
            message_processing_context->retries,
            message_processing_context->message.content);
        OnMessageProcessingCompleted(&message_processing_context->message, ADUC_D2C_Message_Status_Success);
        goto done;
    }

    if (message_processing_context->nextRetryTimeStampEpoch != previousRetryTimeStamp)
    {
        // It's possible that the next retry time has been set by the responseCallback(),
        // we don't need to do anything here.
        SetMessageStatus(&message_processing_context->message, ADUC_D2C_Message_Status_In_Progress);
        goto done;
    }

    if (message_processing_context->retries >= message_processing_context->retryStrategy->maxRetries)
    {
        Log_Warn(
            "Maximum attempt reached (t:%d, r:%d)",
            message_processing_context->type,
            message_processing_context->retries);
        OnMessageProcessingCompleted(
            &message_processing_context->message, ADUC_D2C_Message_Status_Max_Retries_Reached);
        goto done;
    }

    for (int i = 0; i < message_processing_context->retryStrategy->httpStatusRetryInfoSize; i++)
    {
        ADUC_D2C_HttpStatus_Retry_Info* info = &message_processing_context->retryStrategy->httpStatusRetryInfo[i];

        if (http_status_code >= info->httpStatusMin && http_status_code <= info->httpStatusMax)
        {
            if (message_processing_context->retries >= info->maxRetry)
            {
                Log_Warn("Max retries reached (httpStatus:%d)", http_status_code);
                OnMessageProcessingCompleted(
                    &message_processing_context->message, ADUC_D2C_Message_Status_Max_Retries_Reached);
                goto done;
            }

            if (info->retryTimestampCalcFunc == NULL)
            {
                Log_Debug("Retry timestamp calculator func is not specified. Skipped. (info #%d)", i);
                continue;
            }

            message_processing_context->retries++;
            time_t newTime = info->retryTimestampCalcFunc(
                info->additionalDelaySecs,
                message_processing_context->retries,
                message_processing_context->retryStrategy->initialDelayUnitMilliSecs,
                message_processing_context->retryStrategy->maxDelaySecs,
                message_processing_context->retryStrategy->maxJitterPercent);

            Log_Debug(
                "Will resend the message in %d second(s) (epoch:%d, t:%d, r:%d, c:0x%x)",
                newTime - message_processing_context->nextRetryTimeStampEpoch,
                newTime,
                message_processing_context->type,
                message_processing_context->retries,
                message_processing_context->message.content);
            message_processing_context->nextRetryTimeStampEpoch = newTime;
            SetMessageStatus(&message_processing_context->message, ADUC_D2C_Message_Status_In_Progress);
            goto done;
        }
    }

    if (!computed)
    {
        message_processing_context->nextRetryTimeStampEpoch +=
            message_processing_context->retryStrategy->fallbackWaitTimeSec;
        Log_Warn(
            "Failed to calculate the next retry timestamp. Next retry in %d seconds.",
            message_processing_context->retryStrategy->fallbackWaitTimeSec);
        SetMessageStatus(&message_processing_context->message, ADUC_D2C_Message_Status_In_Progress);
    }

done:
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_unlock(&message_processing_context->mutex);
#else
    static_cast<rtos::Mutex *>(message_processing_context->mutex)->unlock();
#endif
}

/**
 * @brief Performs messages processing tasks.
 *
 * Note: must call this function every 100ms - 200ms to ensure that the Device to Cloud messages
 *       are processed in timely manner.
 *
 **/
void ADUC_D2C_Messaging_DoWork()
{
    for (int i = 0; i < ADUC_D2C_Message_Type_Max; i++)
    {
        ProcessMessage(&s_messageProcessingContext[i]);
    }
}

static void ProcessMessage(ADUC_D2C_Message_Processing_Context* message_processing_context)
{
    bool shouldSend = false;
    time_t now = GetTimeSinceEpochInSeconds();
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_lock(&s_pendingMessageStoreMutex);
    pthread_mutex_lock(&message_processing_context->mutex);
#else
    s_pendingMessageStoreMutex.lock();
    static_cast<rtos::Mutex *>(message_processing_context->mutex)->lock();
#endif

    if (s_pendingMessageStore[message_processing_context->type].content != NULL)
    {
        if (message_processing_context->message.content != NULL)
        {
            if (message_processing_context->message.status == ADUC_D2C_Message_Status_Waiting_For_Response)
            {
                // Let's wait to see what the response is.
                goto done;
            }

            // Discard old message.
            Log_Info(
                "New D2C message content (t:%d, content:0x%x).",
                message_processing_context->type,
                s_pendingMessageStore[message_processing_context->type].content);
            OnMessageProcessingCompleted(&message_processing_context->message, ADUC_D2C_Message_Status_Replaced);
        }

        // Use new message
        memset(&message_processing_context->message, 0, sizeof(message_processing_context->message));
        message_processing_context->message = s_pendingMessageStore[message_processing_context->type];
        message_processing_context->message.attempts = 0;
        message_processing_context->retries = 0;
        message_processing_context->nextRetryTimeStampEpoch = now;

        // Empty pending message store.
        memset(&s_pendingMessageStore[message_processing_context->type], 0, sizeof(ADUC_D2C_Message));
        shouldSend = message_processing_context->message.content != NULL;

        SetMessageStatus(&message_processing_context->message, ADUC_D2C_Message_Status_In_Progress);
    }
    else if (
        (message_processing_context->message.content != NULL)
        && (message_processing_context->message.status == ADUC_D2C_Message_Status_In_Progress)
        && (now >= message_processing_context->nextRetryTimeStampEpoch))
    {
        shouldSend = true;
    }

    if (shouldSend)
    {
        if (message_processing_context->transportFunc == NULL)
        {
            Log_Error(
                "Cannot send message. Transport function is NULL. Will retry in the next %d seconds. (t:%d)",
                FATAL_ERROR_WAIT_TIME_SEC,
                message_processing_context->type);
            message_processing_context->nextRetryTimeStampEpoch += FATAL_ERROR_WAIT_TIME_SEC;
        }
        else
        {
            message_processing_context->message.attempts++;
            Log_Debug(
                "Sending D2C message (t:%d, retries:%d).",
                message_processing_context->type,
                message_processing_context->retries);
            if (message_processing_context->transportFunc(
                    message_processing_context->message.cloudServiceHandle,
                    message_processing_context,
                    DefaultIoTHubSendReportedStateCompletedCallback)
                != 0)
            {
                message_processing_context->nextRetryTimeStampEpoch += FATAL_ERROR_WAIT_TIME_SEC;
                Log_Error(
                    "Failed to send message. Will retry in the next %d seconds. (t:%d)",
                    FATAL_ERROR_WAIT_TIME_SEC,
                    message_processing_context->type);
            }
        }
    }

done:
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_unlock(&message_processing_context->mutex);
    pthread_mutex_unlock(&s_pendingMessageStoreMutex);
#else
    static_cast<rtos::Mutex *>(message_processing_context->mutex)->unlock();
    s_pendingMessageStoreMutex.unlock();
#endif
}

/**
 * @brief Initializes messaging utility.
 *
 * @return Returns true if success.
 */
bool ADUC_D2C_Messaging_Init()
{
    bool success = false;
    int i = 0;
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_lock(&s_pendingMessageStoreMutex);
#else
    s_pendingMessageStoreMutex.lock();
#endif
    if (!s_core_initialized)
    {
        memset(&s_messageProcessingContext, 0, sizeof(s_messageProcessingContext));
        memset(&s_pendingMessageStore, 0, sizeof(s_pendingMessageStore));
        for (i = 0; i < ADUC_D2C_Message_Type_Max; i++)
        {
            // NUVOTON: For C++ strong typing
#if 0
            s_messageProcessingContext[i].type = i;
#else
            s_messageProcessingContext[i].type = (ADUC_D2C_Message_Type) i;
#endif
            s_messageProcessingContext[i].transportFunc = ADUC_D2C_Default_Message_Transport_Function;
            s_messageProcessingContext[i].retryStrategy = &g_defaultRetryStrategy;
            // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
            int res = pthread_mutex_init(&s_messageProcessingContext[i].mutex, NULL);
            if (res != 0)
            {
                Log_Error("Can't init mutex for type %d. (err:%d)", i, res);
                goto done;
            }
#else
            s_messageProcessingContext[i].mutex = new (s_messageProcessingContext[i].mutexBlock) rtos::Mutex;
#endif
        }
        s_core_initialized = true;
    }
    success = true;
done:
    if (!success)
    {
        ADUC_D2C_Messaging_Uninit();
    }

    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_unlock(&s_pendingMessageStoreMutex);
#else
    s_pendingMessageStoreMutex.unlock();
#endif
    return success;
}

void ADUC_D2C_Messaging_Uninit()
{
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_lock(&s_pendingMessageStoreMutex);
#else
    s_pendingMessageStoreMutex.lock();
#endif
    if (s_core_initialized)
    {
        // Cancel pending messages
        for (int i = 0; i < ADUC_D2C_Message_Type_Max; i++)
        {
            // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
            pthread_mutex_lock(&s_messageProcessingContext[i].mutex);
#else
            static_cast<rtos::Mutex *>(s_messageProcessingContext[i].mutex)->lock();
#endif
            if (s_pendingMessageStore[i].content != NULL)
            {
                OnMessageProcessingCompleted(&s_pendingMessageStore[i], ADUC_D2C_Message_Status_Canceled);
            }

            if (s_messageProcessingContext[i].message.content != NULL)
            {
                OnMessageProcessingCompleted(&s_messageProcessingContext[i].message, ADUC_D2C_Message_Status_Canceled);
            }
            // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
            pthread_mutex_unlock(&s_messageProcessingContext[i].mutex);
            pthread_mutex_destroy(&s_messageProcessingContext[i].mutex);
#else
            static_cast<rtos::Mutex *>(s_messageProcessingContext[i].mutex)->unlock();
            static_cast<rtos::Mutex *>(s_messageProcessingContext[i].mutex)->~Mutex();
#endif
            s_messageProcessingContext[i].initialized = false;
        }
        s_core_initialized = false;
    }
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_unlock(&s_pendingMessageStoreMutex);
#else
    s_pendingMessageStoreMutex.unlock();
#endif
}

/**
 * @brief Submits the message to pending messages store. If the message for specified @p type already exist, it will be replaced by the new message.
 *
 * @param type The message type.
 * @param cloudServiceHandle An opaque pointer to the underlying cloud service handle.
 *                           By default, this is the handle to an Azure Iot C PnP device client.
 * @param message The required message content. For example, a JSON string containing 'reported' property of the IoT Hub Device Twin.
 * @param responseCallback A optional callback to be called when the device received a http response.
 * @param completedCallback An optional callback to be called when the messages processor stopped processing the message.
 * @param statusChangedCallback A optional callback to be called when the messages status has changed.
 * @param userData An additional user data.
 *
 * @return Returns true if message successfully added to the pending-messages queue.
 */
bool ADUC_D2C_Message_SendAsync(
    ADUC_D2C_Message_Type type,
    void* cloudServiceHandle,
    const char* message,
    ADUC_D2C_MESSAGE_HTTP_RESPONSE_CALLBACK responseCallback,
    ADUC_D2C_MESSAGE_COMPLETED_CALLBACK completedCallback,
    ADUC_D2C_MESSAGE_STATUS_CHANGED_CALLBACK statusChangedCallback,
    void* userData)
{
    if (message == NULL)
    {
        Log_Error("message is NULL");
        return false;
    }

    char* messageToSend = NULL;
    if (mallocAndStrcpy_s(&messageToSend, message) != 0)
    {
        return false;
    }
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_lock(&s_pendingMessageStoreMutex);
#else
    s_pendingMessageStoreMutex.lock();
#endif

    // Replace pending message if exist.
    if (s_pendingMessageStore[type].content != NULL)
    {
        if (s_pendingMessageStore[type].completedCallback != NULL)
        {
            Log_Debug("Replacing existing pending message. (t:%d, s:%s)", type, s_pendingMessageStore[type].content);
            OnMessageProcessingCompleted(&s_pendingMessageStore[type], ADUC_D2C_Message_Status_Replaced);
        }
    }

    Log_Debug("Queueing message (t:%d, c:0x%x, m:%s)", type, message, message);
    memset(&s_pendingMessageStore[type], 0, sizeof(s_pendingMessageStore[0]));
    s_pendingMessageStore[type].cloudServiceHandle = cloudServiceHandle;
    s_pendingMessageStore[type].originalContent = message;
    s_pendingMessageStore[type].content = messageToSend;
    s_pendingMessageStore[type].responseCallback = responseCallback;
    s_pendingMessageStore[type].completedCallback = completedCallback;
    s_pendingMessageStore[type].statusChangedCallback = statusChangedCallback;
    s_pendingMessageStore[type].contentSubmitTime = GetTimeSinceEpochInSeconds();
    s_pendingMessageStore[type].userData = userData;
    SetMessageStatus(&s_pendingMessageStore[type], ADUC_D2C_Message_Status_Pending);
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_unlock(&s_pendingMessageStoreMutex);
#else
    s_pendingMessageStoreMutex.unlock();
#endif
    return true;
}

/**
 * @brief Sets the messaging transport. By default, the messaging utility will send messages to IoT Hub.
 *
 * @param type The message type.
 * @param transportFunc The message transport function.
 */
void ADUC_D2C_Messaging_Set_Transport(ADUC_D2C_Message_Type type, ADUC_D2C_MESSAGE_TRANSPORT_FUNCTION transportFunc)
{
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_lock(&s_messageProcessingContext[type].mutex);
    s_messageProcessingContext[type].transportFunc = transportFunc;
    pthread_mutex_unlock(&s_messageProcessingContext[type].mutex);
#else
    static_cast<rtos::Mutex *>(s_messageProcessingContext[type].mutex)->lock();
    s_messageProcessingContext[type].transportFunc = transportFunc;
    static_cast<rtos::Mutex *>(s_messageProcessingContext[type].mutex)->unlock();
#endif
}

/**
 * @brief The default function used for sending message content to the IoT Hub.
 *
 * @param cloudServiceHandle A pointer to ADUC_ClientHandle
 * @param context A pointer to the ADUC_D2C_Message_Processing_Context.
 * @param handleResponseMessageFunc A callback function to be called when the device received a response from the IoT Hub.
 * @return int Returns 0 if success. Otherwise, return implementation specific error code.
 *         For default fuction, this is equivalent to IOTHUB_CLIENT_RESULT.
 */
int ADUC_D2C_Default_Message_Transport_Function(
    void* cloudServiceHandle, void* context, ADUC_C2D_RESPONSE_HANDLER_FUNCTION c2dResponseHandlerFunc)
{
    ADUC_D2C_Message_Processing_Context* message_processing_context = (ADUC_D2C_Message_Processing_Context*)context;
    if (message_processing_context->message.cloudServiceHandle == NULL
        || *((ADUC_ClientHandle*)message_processing_context->message.cloudServiceHandle) == NULL)
    {
        Log_Warn(
            "Try to send D2C message but cloudServiceHandle is NULL. Skipped. (content:0x%x)",
            message_processing_context->message.content);
        return 1;
    }
    else
    {
        // Send content.
        Log_Debug("Sending D2C message:\n%s", (const char*)message_processing_context->message.content);

        IOTHUB_CLIENT_RESULT iotHubClientResult = (IOTHUB_CLIENT_RESULT)ClientHandle_SendReportedState(
            *((ADUC_ClientHandle*)message_processing_context->message.cloudServiceHandle),
            (const unsigned char*)message_processing_context->message.content,
            strlen(message_processing_context->message.content),
            c2dResponseHandlerFunc,
            message_processing_context);

        if (iotHubClientResult == IOTHUB_CLIENT_OK)
        {
            SetMessageStatus(&message_processing_context->message, ADUC_D2C_Message_Status_Waiting_For_Response);
        }
        else
        {
            Log_Error("ClientHandle_SendReportedState return %d. Stop processing the message.", iotHubClientResult);
            OnMessageProcessingCompleted(&message_processing_context->message, ADUC_D2C_Message_Status_Failed);
        }

        return iotHubClientResult;
    }
}

/**
 * @brief Sets the retry strategy for the specified @p type
 *
 * @param type The message type.
 * @param strategy The retry strategy information.
 */
void ADUC_D2C_Messaging_Set_Retry_Strategy(ADUC_D2C_Message_Type type, ADUC_D2C_RetryStrategy* strategy)
{
    // NUVOTON: Use Mbed OS mutex instead of pthread mutex
#if 0
    pthread_mutex_lock(&s_messageProcessingContext[type].mutex);
    s_messageProcessingContext[type].retryStrategy = strategy;
    pthread_mutex_unlock(&s_messageProcessingContext[type].mutex);
#else
    static_cast<rtos::Mutex *>(s_messageProcessingContext[type].mutex)->lock();
    s_messageProcessingContext[type].retryStrategy = strategy;
    static_cast<rtos::Mutex *>(s_messageProcessingContext[type].mutex)->unlock();
#endif
}
