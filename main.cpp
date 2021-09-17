/*
 * Copyright (c) 2020 Arm Limited
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mbed.h"
#include "rtos/ThisThread.h"
#include "NTPClient.h"

#include "certs.h"
#include "iothub.h"
#include "iothub_client_options.h"
#include "iothub_device_client.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/tickcounter.h"
#include "azure_c_shared_utility/xlogging.h"

#include "iothubtransportmqtt.h"
#include "azure_cloud_credentials.h"

#include "../LSM6DSL/LSM6DSL_acc_gyro_driver.h"
#include "../LSM6DSL/LSM6DSLSensor.h"
#include "ei_run_classifier.h"
#include "model-parameters/model_metadata.h"
#include <cstring>

// Blinking rate in milliseconds
#define BLINKING_RATE    500ms
#define BUSY_LOOP_DURATON 9ms

// Set the sampling frequency in Hz
static int16_t sampling_freq = 101;
static int64_t time_between_samples_us = (1000000 / (sampling_freq - 1));
static int64_t time_between_messages_us = 60000000; //60000000 = 1 minute, 300000000 = 5min
static int64_t timeout_unused_ms = 30000;           // amount of ms unused until sleep mode (ML disabled)
bool ML_enabled = true;

/* IMPORTANT: message buffer now does not hold space for more than 1 batch of messages now (for 1 min interval) */
// Initialize variables for messaging
IOTHUB_MESSAGE_HANDLE message_handle;
const int message_buf_size = 1200;      // 1200 based on 1 message per 0.5s for 5min x 2 cycles (reality is ~0.8s)
char message[message_buf_size][80];              
int i = 0;                          // iterator for messages within time_between_messages_us cycle
time_t message_timestamp;
char ts_string[40];
int DeviceID = 3;                 // Device Identifier
char NN_State[20] = "LOL";          // User state (Stoop, Walk, Stand, Squat, Unused)

char NN_State_old[20] = "streak";   // to keep track of streaks of same state; saves on messages sent
int streak_duration = 0;
char streak_ts_string[40];

int Duration = 0;                   // Duration of state [ms] 
int loopOld = 0;
int loopNew = 0;
int t_previous_send = 0;

static float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];

/**
 * This example sends and receives messages to and from Azure IoT Hub.
 * The API usages are based on Azure SDK's official iothub_convenience_sample.
 */

 // Check if this checks out for B-L4S5I board
static DevI2C devI2C(PB_11, PB_10);
// Acc and Gyro Class Object
static LSM6DSLSensor acc_gyro(&devI2C, LSM6DSL_ACC_GYRO_I2C_ADDRESS_LOW);


// Global symbol referenced by the Azure SDK's port for Mbed OS, via "extern"
NetworkInterface *_defaultSystemNetwork;

static bool message_received = false;

static void on_connection_status(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* user_context)
{
    if (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED) {
        LogInfo("Connected to IoT Hub");
    } else {
        LogError("Connection failed, reason: %s", MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, reason));
    }
}

static IOTHUBMESSAGE_DISPOSITION_RESULT on_message_received(IOTHUB_MESSAGE_HANDLE message, void* user_context)
{
    LogInfo("Message received from IoT Hub");

    const unsigned char *data_ptr;
    size_t len;
    if (IoTHubMessage_GetByteArray(message, &data_ptr, &len) != IOTHUB_MESSAGE_OK) {
        LogError("Failed to extract message data, please try again on IoT Hub");
        return IOTHUBMESSAGE_ABANDONED;
    }

    message_received = true;
    LogInfo("Message body: %.*s", len, data_ptr);
    return IOTHUBMESSAGE_ACCEPTED;
}

static void on_message_sent(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    if (result == IOTHUB_CLIENT_CONFIRMATION_OK) {
        LogInfo("Message sent successfully");
    } else {
        LogInfo("Failed to send message, error: %s",
            MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
    }
}

IOTHUB_DEVICE_CLIENT_HANDLE client_handle;
IOTHUB_CLIENT_RESULT res;
tickcounter_ms_t interval = 100;
void demo() {
    // Machine Learning Setup
    void *init;

    int32_t acc_val_buf[3] = {0};
    int32_t gyro_val_buf[3] = {0};
    // init initializes the component
    acc_gyro.init(init);
    // enables the accelero
    acc_gyro.enable_x();
    // enable gyro
    acc_gyro.enable_g();

    // int y = bar(20);
    Timer t;

    bool trace_on = MBED_CONF_APP_IOTHUB_CLIENT_TRACE;
    // tickcounter_ms_t interval = 100;
    // IOTHUB_CLIENT_RESULT res;

    LogInfo("Initializing IoT Hub client");
    IoTHub_Init();

    client_handle = IoTHubDeviceClient_CreateFromConnectionString(
        azure_cloud::credentials::iothub_connection_string,
        MQTT_Protocol
    );
    if (client_handle == nullptr) {
        LogError("Failed to create IoT Hub client handle");
        goto cleanup;
    }

    // Enable SDK tracing
    res = IoTHubDeviceClient_SetOption(client_handle, OPTION_LOG_TRACE, &trace_on);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to enable IoT Hub client tracing, error: %d", res);
        goto cleanup;
    }

    // Enable static CA Certificates defined in the SDK
    res = IoTHubDeviceClient_SetOption(client_handle, OPTION_TRUSTED_CERT, certificates);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set trusted certificates, error: %d", res);
        goto cleanup;
    }

    // Process communication every 100ms
    res = IoTHubDeviceClient_SetOption(client_handle, OPTION_DO_WORK_FREQUENCY_IN_MS, &interval);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set communication process frequency, error: %d", res);
        goto cleanup;
    }

    // set incoming message callback
    res = IoTHubDeviceClient_SetMessageCallback(client_handle, on_message_received, nullptr);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set message callback, error: %d", res);
        goto cleanup;
    }

    // Set connection/disconnection callback
    res = IoTHubDeviceClient_SetConnectionStatusCallback(client_handle, on_connection_status, nullptr);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set connection status callback, error: %d", res);
        goto cleanup;
    }

    //start timer right before start of loop
    t.start();
    while(true) {
        /* Batch Message Sending (only if any messages to send) */
        if (i > 0) {                                                                // only do message sending if any messages in buffer
        if ((t.read_us() > t_previous_send + time_between_messages_us) | (i >= message_buf_size-1)) {
            // [i>=message_buf_size-1] needed to prevent overflow
            // set new previous send time to start new message collection interval
            t_previous_send = t.read_us();

            // int ret = _defaultSystemNetwork->connect();
            // if (ret != 0) {
            //     if (i < message_buf_size-1) {                                   // if space in buffer, just break message loop and keep collecting data
            //         LogError("Failed to create message");
            //         //goto cleanup;
            //         break;
            //     } else {                                                        // else goto cleanup and report connection error
            //         goto cleanup;
            //     }
            // } else {
            //     LogInfo("Connection success, MAC: %s", _defaultSystemNetwork->get_mac_address());
            // }

            /* Send all batched messages (so up to i)*/
            //for (int m=0; m<i; m++) {
            for (int m = i-1; m>=0; m--) {
                LogInfo("Sending: \"%s\"", message[m]);
        
                message_handle = IoTHubMessage_CreateFromString(message[m]);
                if (message_handle == nullptr) {
                    if (i < message_buf_size-1) {                                   // if space in buffer, just break message loop and keep collecting data
                        LogError("Failed to create message");
                        //goto cleanup;
                        break;
                    } else {                                                        // else goto cleanup and report connection error
                        goto cleanup;
                    }
                }

                res = IoTHubDeviceClient_SendEventAsync(client_handle, message_handle, on_message_sent, nullptr);
                IoTHubMessage_Destroy(message_handle); // message already copied into the SDK

                if (res != IOTHUB_CLIENT_OK) {
                    if (i < message_buf_size-1) {                                   // if space in buffer, just break message loop and keep collecting data
                        LogError("Failed to send message event, error: %d", res);
                        //goto cleanup;
                        break;
                    } else {                                                        // else goto cleanup and report connection error
                        goto cleanup;
                    }   
                }

                // move buffer iterator back when message sent. This will not happen if sending fails
                // If all messages sent, it gives i=0;
                i = m;

                // If message fails, instead of goto cleanup, just exit for-loop and continue collecting messages until next message time
            }
            //LogInfo("Sent %d messages", i);
            /* If messaging succesful, reset i (no need to empty message array; simply overwrite) */
            // i = m;
            sprintf(NN_State, "x");     // reset to stop the streak when messages are sent
            
            // disconnect from network
            // ret = _defaultSystemNetwork->disconnect();
            // if (ret != 0) {
            //     LogError("Disconnect Unsuccesful: %d", ret);
            // } else {
            //     LogInfo("Disconnected from Network.");
            // }            
            //break;
        }
        }
        
        strcpy(NN_State_old, NN_State);         // save previous NN state
        if (ML_enabled == true) {                   // take care of sleepmode
            /* Do machine learning */
            for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME) {
                int64_t next_tick = t.read_us() + time_between_samples_us;
                // printf("gyro enable val: %d\n", check_ena_g);
                acc_gyro.get_x_axes(acc_val_buf);
                acc_gyro.get_g_axes(gyro_val_buf);

                features[ix + 0] = static_cast<float>(acc_val_buf[0]) / 100.0f;
                features[ix + 1] = static_cast<float>(acc_val_buf[1]) / 100.0f;
                features[ix + 2] = static_cast<float>(acc_val_buf[2]) / 100.0f;
                features[ix + 3] = static_cast<float>(gyro_val_buf[0]) / 1000.0f;
                features[ix + 4] = static_cast<float>(gyro_val_buf[1]) / 1000.0f;
                features[ix + 5] = static_cast<float>(gyro_val_buf[2]) / 1000.0f;

                while (t.read_us() < next_tick){
                    // busy loop
                }
            }
            LogInfo("Gyroscope Values: (x, y, z): \t (%.2f, \t%.2f, \t%.2f)", features[3], features[4], features[5]);

            ei_impulse_result_t result = {0};

            signal_t signal;

            numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);

            // run classifier
            EI_IMPULSE_ERROR ei_res = run_classifier(&signal, &result, false);
            ei_printf("run_classifier returned: %d\n", ei_res);
            // if (res != 0) return 1;

            // print prediction timings
            ei_printf("Predictions (DSP: %d ms., Classification: %d ms., Anomaly: %d ms.): \n",
                result.timing.dsp, result.timing.classification, result.timing.anomaly);

            int largest_index_val = 0;
            int largest_val = 0.f;
            // print the predictions
            for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                if (static_cast<int>(result.classification[ix].value*100) > largest_val)
                {
                    largest_val = static_cast<int>(result.classification[ix].value*100);
                    largest_index_val = ix;
                }
                ei_printf("%s:\t%.5f\n", result.classification[ix].label, result.classification[ix].value);
            }
            #if EI_CLASSIFIER_HAS_ANOMALY == 1
                ei_printf("anomaly:\t%.3f\n", result.anomaly);
            #endif

            //strcpy(NN_State_old, NN_State);         // save previous NN state
            switch (largest_index_val) {
                case 0: strcpy(NN_State, "Squat");
                        break;
                case 1: strcpy(NN_State, "Stand");
                        break;
                case 2: strcpy(NN_State, "Stoop");
                        break;
                case 3: strcpy(NN_State, "Walk");
                        break;
                default: strcpy(NN_State, "Anomaly");
                        break;
            }

        } else {                                    // if ML disabled, report unused
            strcpy(NN_State, "Unused");
            
            // check if gyro values over limit, if true, enable ML again
            acc_gyro.get_g_axes(gyro_val_buf);
            if (gyro_val_buf[0] > 5000 || gyro_val_buf[1] > 5000 || gyro_val_buf[2] > 5000) {
                ML_enabled = true;
                i++;
                LogInfo("**********Left Sleepmode**********");
            } else {
                ThisThread::sleep_for(1s);                              // only check ever ~1s if movement
            }

        }

        // Find duration of current State reading
        loopOld = loopNew;
        loopNew = t.read_ms();
        Duration = loopNew-loopOld;

        /* Streak if-statement */
        if (strcmp(NN_State, NN_State_old) == 0 ) {
            streak_duration = streak_duration + Duration;                                           // increase ms length of streak
            // Update message that was first in the streak: change
            sprintf(message[i-1], "{\"TimeStamp\":\"%s\",\"DeviceID\":%d,\"State\":\"%s\",\"Duration\":%d}", ts_string, DeviceID, NN_State, streak_duration);

            LogInfo("--------------------State: \t%s--------------------", NN_State);
            LogInfo("--------------------Duration: \t%d--------------------", streak_duration);
            LogInfo("--------------------i value: \t%d--------------------", i);

            // "Unused" sleep logic: if Unused streak > 30s, disable ML to save power
            if ((strcmp(NN_State, "Stoop") == 0 ) && streak_duration > timeout_unused_ms) {
                ML_enabled = false;
                sprintf(NN_State, "x");                             // reset state to stop streak from continuing when sleepmode exit
                LogInfo("**********Entered Sleepmode**********");
            }
        } else {
            streak_duration = Duration;
        
            // get current time and extract this for timestamp string
            time(&message_timestamp);
            tm local_tm = *localtime(&message_timestamp);
            char tsV_day[2], tsV_month[2], tsV_year[4], tsV_time[8];
            strftime(tsV_day, 3, "%d", &local_tm);
            strftime(tsV_month, 3, "%m", &local_tm);
            strftime(tsV_year, 5, "%Y", &local_tm);
            strftime(tsV_time, 9, "%T", &local_tm);
            sprintf(ts_string, "%.4s%.2s%.2s %.8s", tsV_year, tsV_month, tsV_day, tsV_time);
        
            LogInfo("--------------------State: \t%s--------------------", NN_State);
            LogInfo("--------------------Duration: \t%d--------------------", Duration);
            LogInfo("--------------------i value: \t%d--------------------", i);

            // create message       
            sprintf(message[i], "{\"TimeStamp\":\"%s\",\"DeviceID\":%d,\"State\":\"%s\",\"Duration\":%d}", ts_string, DeviceID, NN_State, Duration);

            i++;
        }
        //} else {
            // check if gyro values over limit, if true, enable ML again
            // acc_gyro.get_g_axes(gyro_val_buf);
            // if (gyro_val_buf[0] > 5000 || gyro_val_buf[1] > 5000 || gyro_val_buf[2] > 5000) {
            //     ML_enabled = true;
            //     LogInfo("**********Left Sleepmode**********");
            // } else {
            //     ThisThread::sleep_for(1s);
            // }
        //}
    }

cleanup:
    IoTHubDeviceClient_Destroy(client_handle);
    IoTHub_Deinit();
    LogError("Messaging failed: Could not connect to IoT Hub and Message Buffer full.");
}

int main() {
    LogInfo("\n\n\n\n\nConnecting to the network");

    _defaultSystemNetwork = NetworkInterface::get_default_instance();
    if (_defaultSystemNetwork == nullptr) {
        LogError("No network interface found");
        return -1;
    }

    int ret = _defaultSystemNetwork->connect();
    if (ret != 0) {
        LogError("Connection error: %d", ret);
        return -1;
    }
    LogInfo("Connection success, MAC: %s", _defaultSystemNetwork->get_mac_address());

    LogInfo("Getting time from the NTP server");

    NTPClient ntp(_defaultSystemNetwork);
    ntp.set_server("2.pool.ntp.org", 123);
    time_t timestamp = ntp.get_timestamp();
    if (timestamp < 0) {
        LogError("Failed to get the current time, error: %lld", timestamp);
        return -1;
    }
    LogInfo("Time: %s", ctime(&timestamp));
    set_time(timestamp);


    LogInfo("Starting the Demo");
    demo();
    LogInfo("The demo has ended");

    return 0;
}
