/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Adrian Bradianu (github.com/abradianu)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "mqtt_client.h"

#define MQTT_CLIENT_TASK_NAME       "mqtt_client"
#define MQTT_CLIENT_TASK_PRIO       2
#define MQTT_CLIENT_TASK_STACK      4096

/* Delay between connection attempts */
#define MQTT_CLIENT_DELAY_MIN       100
#define MQTT_CLIENT_DELAY_MAX       (600 * 1000)

/* Semaphore timeout in ticks */
#define MQTT_CLIENT_CB_TIMEOUT      ((60 * 1000) / portTICK_RATE_MS)
#define MQTT_CLIENT_CHECK_TIMEOUT   ((10 * 1000) / portTICK_RATE_MS)

typedef enum {
    MQTT_ERROR_OK,
    MQTT_ERROR_CONNECT,
    MQTT_ERROR_SUBSCRIBE,
    MQTT_ERROR_PUBLISH,
}error_t;

typedef struct {
    mqtt_client_info_t * client_info;
    mqtt_client_t *      mqtt_client;
    error_t              error;
    SemaphoreHandle_t    sem;
} mqtt_client_ctrl_t;

static const char *TAG =    "MQTT";

/* Called when publish is complete either with sucess or failure */
static void mqtt_pub_request_cb(void *arg, err_t result)
{
    mqtt_client_ctrl_t * mqtt_client_ctrl = arg;

    if (mqtt_client_ctrl == NULL) {
        ESP_LOGE(TAG, "%s: NULL arg", __FUNCTION__);
        return;
    }

    if (result != ERR_OK) {
        ESP_LOGI(TAG, "Publish failed, result: %d\n", result);
        
        mqtt_client_ctrl->error = MQTT_ERROR_PUBLISH;
        xSemaphoreGive(mqtt_client_ctrl->sem);
    }
}

esp_err_t mqtt_client_publish(void * ctrl_handle, const char *topic,
                              const u8_t *data, u16_t len, u8_t qos, u8_t retain)
{
    mqtt_client_ctrl_t * mqtt_client_ctrl = ctrl_handle;
    mqtt_client_t * mqtt_client;
    err_t err;

    ESP_LOGI(TAG, "Publish on topic %s, len %d, qos %d, retain %d", topic, len, qos, retain);
    
    if (mqtt_client_ctrl == NULL ||
        (mqtt_client = mqtt_client_ctrl->mqtt_client) == NULL) {
        ESP_LOGE(TAG, "NULL ctrl_handle or mqtt_client!");
        return ESP_FAIL;
    }

    if (data == NULL || !len) {
        ESP_LOGE(TAG, "Invalid data!");
        return ESP_FAIL;
    }

    if (!mqtt_client_is_connected(mqtt_client)) {
        ESP_LOGI(TAG, "Publish failed, mqtt not connected!");
        return ESP_FAIL;
    }

    err = mqtt_publish(mqtt_client, topic, data, len, qos, retain, mqtt_pub_request_cb, mqtt_client_ctrl);
    if (err != ERR_OK) {
        ESP_LOGI(TAG, "Publish failed, err %d!", err);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    mqtt_client_ctrl_t * mqtt_client_ctrl = arg;

    if (mqtt_client_ctrl == NULL) {
        ESP_LOGE(TAG, "%s: NULL arg", __FUNCTION__);
        return;
    }

    ESP_LOGI(TAG, "Incoming publish payload, len %d, flags %u",
             len, (unsigned int)flags);
    
    if (mqtt_client_ctrl->client_info->message_received_cb)
        mqtt_client_ctrl->client_info->message_received_cb(data, len, flags & MQTT_DATA_FLAG_LAST);
}

static void mqtt_sub_request_cb(void *arg, err_t result)
{
    mqtt_client_ctrl_t * mqtt_client_ctrl = arg;

    if (mqtt_client_ctrl == NULL) {
        ESP_LOGE(TAG, "%s: NULL arg!", __FUNCTION__);
        return;
    }

    if (result != ERR_OK) {
        ESP_LOGI(TAG,"Subscribe failed, ret %d!\n", result);

        mqtt_client_ctrl->error = MQTT_ERROR_SUBSCRIBE;
    }

    xSemaphoreGive(mqtt_client_ctrl->sem);
}

static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len)
{
    /* Not implemented for now */
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    mqtt_client_ctrl_t * mqtt_client_ctrl = arg;

    if (mqtt_client_ctrl == NULL) {
        ESP_LOGE(TAG, "%s: NULL arg", __FUNCTION__);
        return;
    }

    if (status == MQTT_CONNECT_ACCEPTED) {
        ESP_LOGI(TAG, "Successfully connected");
    } else {
        ESP_LOGI(TAG, "Disconnected, reason: %d", status);
        
        mqtt_client_ctrl->error = MQTT_ERROR_CONNECT;
    }
    xSemaphoreGive(mqtt_client_ctrl->sem);
}

static void mqtt_client_task(void *arg)
{
    mqtt_client_ctrl_t * mqtt_client_ctrl;
    mqtt_client_t * mqtt_client;
    mqtt_client_info_t * client_info;
    struct mqtt_connect_client_info_t ci;
    struct hostent *hp;
    ip_addr_t ip_addr;
    uint32_t delay = MQTT_CLIENT_DELAY_MIN;
    err_t err;
    BaseType_t res;
  
    mqtt_client_ctrl = arg;
    mqtt_client = mqtt_client_ctrl->mqtt_client;
    client_info = mqtt_client_ctrl->client_info;

    while (1) {
        /* Disconnect first in case it was connected before */
        mqtt_disconnect(mqtt_client);

        /* Exponential delay before new connection for the case when broker is down
          and the code will loop trying to connect */
        vTaskDelay(delay / portTICK_RATE_MS);
        delay <<= 1;
        if (delay > MQTT_CLIENT_DELAY_MAX)
            delay = MQTT_CLIENT_DELAY_MAX;

        /* Reset error state */        
        mqtt_client_ctrl->error = MQTT_ERROR_OK;

        /* Setup an empty client info structure */
        memset(&ci, 0, sizeof(ci));
        /* LWIp MQTT does not implement user/password yet */
        ci.client_user = client_info->user;
        ci.client_pass = client_info->pwd;
        ci.client_id = client_info->client_id;
        
        ESP_LOGI(TAG, "Connect to %s, user/pwd: %s/%s, client_id %s",
                client_info->broker, ci.client_user, ci.client_pass, ci.client_id);

        if ((hp = gethostbyname(client_info->broker)) == 0) {
            ESP_LOGI(TAG, "Could not resolve %s", client_info->broker);
            continue;
        } else {
            ip_addr.addr = ((struct in_addr *)(hp->h_addr))->s_addr;
        }
        
        /* Initiate client and connect to server, if this fails immediately an error code is returned
           otherwise mqtt_connection_cb will be called with connection result after attempting 
           to establish a connection with the server. 
           For now MQTT version 3.1.1 is always used */
        err = mqtt_client_connect(mqtt_client, &ip_addr, MQTT_PORT, mqtt_connection_cb, mqtt_client_ctrl, &ci);
        if (err != ERR_OK) {
            ESP_LOGI(TAG, "Connect error, ret: %d", err);
            continue;
        }
    
        /* Wait for the connect calback result */
        res = xSemaphoreTake(mqtt_client_ctrl->sem, MQTT_CLIENT_CB_TIMEOUT);
        if (res != pdTRUE || mqtt_client_ctrl->error != MQTT_ERROR_OK) {
            /* Try again to connect */
            continue;
        }

        if (client_info->sub_topic) {
            /* Setup callback for incoming publish requests */
            mqtt_set_inpub_callback(mqtt_client, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, mqtt_client_ctrl);
    
            /* Subscribe to topic, call mqtt_sub_request_cb with result */ 
            err = mqtt_subscribe(mqtt_client, client_info->sub_topic, client_info->sub_qos,
                                mqtt_sub_request_cb, mqtt_client_ctrl);
            if (err != ERR_OK) {
                ESP_LOGI(TAG, "Subscribe error, ret: %d!", err);
                continue;
            }

            /* Wait for the subscribe calback result */
            res = xSemaphoreTake(mqtt_client_ctrl->sem, MQTT_CLIENT_CB_TIMEOUT);
            if (res != pdTRUE || mqtt_client_ctrl->error != MQTT_ERROR_OK) {
                /* Try again to connect */
                continue;
            }
        }

        /* We are connected to the MQTT broker and subscribe (if any) was OK */

        /* Reset the delay */
        delay = MQTT_CLIENT_DELAY_MIN;

        /* Wait for errors */
        while (1) {
            res = xSemaphoreTake(mqtt_client_ctrl->sem, MQTT_CLIENT_CHECK_TIMEOUT);
            if (res != pdTRUE) {
                /* Timeout */
                
                /* Check that we are connected */
                if (mqtt_client_is_connected(mqtt_client)) {
                    ESP_LOGI(TAG, "Alive and connected, heap %d", esp_get_free_heap_size());
                } else {
                    ESP_LOGI(TAG, "Alive but not connected, heap %d", esp_get_free_heap_size());
                    
                    /* Break to main loop and try to connect again */
                    break;
                }
                
                /* Continue to wait */
                continue;
            } else {
                /* Error received, break to main loop and try to connect again */
                break;
            }
        }
    }
}

esp_err_t mqtt_client_start(mqtt_client_info_t * client_info)
{
    if (client_info == NULL) {
        return ESP_FAIL;
    }

    mqtt_client_ctrl_t * mqtt_client_ctrl = malloc(sizeof(mqtt_client_ctrl_t));
    if (mqtt_client_ctrl == NULL) {
        return ESP_FAIL;
    }

    mqtt_client_ctrl->client_info = client_info;
    mqtt_client_ctrl->sem = xSemaphoreCreateBinary();
    if (!mqtt_client_ctrl->sem || !mqtt_client_ctrl->client_info) {
        free(mqtt_client_ctrl);
        return ESP_FAIL;
    }

    mqtt_client_ctrl->mqtt_client = mqtt_client_new();
    if (mqtt_client_ctrl->mqtt_client == NULL) {
        free(mqtt_client_ctrl);
        return ESP_FAIL;
    }

    if (xTaskCreate(mqtt_client_task,
                MQTT_CLIENT_TASK_NAME,
                MQTT_CLIENT_TASK_STACK,
                mqtt_client_ctrl,
                MQTT_CLIENT_TASK_PRIO,
                NULL) != pdPASS)  {
        free(mqtt_client_ctrl);
        return ESP_FAIL;
    }

    client_info->ctrl_handle = mqtt_client_ctrl;
    return ESP_OK;
}
