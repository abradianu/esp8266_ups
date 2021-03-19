/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2021 Adrian Bradianu (github.com/abradianu)
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"

#include "ota.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "nvs_utils.h"
 #include "ups.h"

#include "cmd_recv.h"

#define CMD_RECV_TASK_NAME        "cmd_rcv"
#define CMD_RECV_TASK_STACK       2048
#define CMD_RECV_TASK_PRIO        10

#define MQTT_BROKER               "192.168.1.135"
#define MQTT_SUB_TOPIC_PREFIX     "sensors/cmd/"
#define MQTT_SUB_QOS              1
#define MQTT_PUB_TOPIC_PREFIX     "sensors/data/"
#define MQTT_PUB_QOS              1

/* Max number of commands to queue */
#define CMD_PARSE_QUEUE_LEN       5

/* JSON command fields */
#define CMD_JSON_CMD             "cmd"
#define CMD_JSON_TIME            "time"
#define CMD_JSON_CHIP_MAC        "mac"
#define CMD_JSON_CLIENT_ID       "id"
#define CMD_JSON_CLIENT_NAME     "name"
#define CMD_JSON_BROKER_IP       "ip"
#define CMD_JSON_RESULT          "res"
#define CMD_JSON_VOUT            "v_out"
#define CMD_JSON_IOUT            "i_out"
#define CMD_JSON_VBAT            "v_bat"
#define CMD_JSON_VIN             "v_in "
#define CMD_JSON_POFF            "p_off"
#define CMD_JSON_BATD            "bat_discharged"
#define CMD_JSON_BATC            "bat_connected"
#define CMD_JSON_FAN             "fan_high"
#define CMD_JSON_ADC_ERR         "adc_err"
#define CMD_JSON_UPTIME          "up"
#define CMD_JSON_FW_VER          "fw_v"
#define CMD_JSON_HEAP            "heap"
#define CMD_JSON_SERVER          "server"
#define CMD_JSON_PORT            "port"
#define CMD_JSON_FILENAME        "file"
#define CMD_JSON_AP_MODE         "ap"
#define CMD_JSON_BRIGHTNESS      "b"
#define CMD_JSON_CCS811_BASELINE "baseline"


/* Delay between MQTT publish attempts */
#define CMD_MQTT_DELAY_MIN       500
#define CMD_MQTT_DELAY_MAX       (600 * 1000)

typedef struct {
    uint8_t * data;
    uint16_t  len;
    bool      last;
} cmd_data_t;

static const char *TAG = "CMD";

static xQueueHandle cmd_recv_queue = NULL;
static mqtt_client_info_t mqtt_client_info;

static char * mqtt_pub_topic;
static char * mqtt_client_id;

static void do_reboot()
{
    ESP_LOGI(TAG, "Reboot requested by command...!");
    vTaskDelay(500 / portTICK_RATE_MS);
    esp_restart();
}

/* Callback to be called by MQTT when incoming data received */
static void cmd_recv_cb(const uint8_t * data, uint16_t len, bool last)
{
    uint8_t * data_buf;
    cmd_data_t * cmd;

    /* Allocate the data buffer */
    data_buf = malloc(len + 1);
    if (!data_buf) {
        ESP_LOGE(TAG, "Could not allocate data buffer!");
        return;
    }

    /* Allocate the cmd */
    cmd = malloc(sizeof(cmd_data_t));
    if (!cmd) {
        ESP_LOGE(TAG, "Could not allocate command buffer!");
        free(data_buf);
        return;
    }

    /* Copy data and make sure it ends with '\0' */
    memcpy(data_buf, data, len);
    data_buf[len] = 0;

    cmd->data = data_buf;
    cmd->len = len + 1;
    cmd->last = last;

    ESP_LOGI(TAG, "Cmd received, len %d, last %d", len, last);
    if (xQueueSend(cmd_recv_queue, (void *)&cmd, 0) != pdPASS) {
        ESP_LOGE(TAG, "Command discarded, queue is full!");

        free(cmd->data);
        free(cmd);
    }
}

static esp_err_t mqtt_start()
{
    char * sub_topic = NULL;
    char * broker_ip = MQTT_BROKER;
    nvs_handle nvs = nvs_get_handle();

    ESP_LOGI(TAG, "MQTT client start, chip id %s", nvs_get_base_mac());

    if (nvs) {
        size_t len;

        /* Read MQTT client name from flash */
        if (nvs_get_str(nvs, NVS_MQTT_CLIENT_ID, NULL, &len) == ESP_OK) {
            mqtt_client_id = calloc(1, len + 1);
            if (mqtt_client_id == NULL) {
                goto error;
            }

            nvs_get_str(nvs, NVS_MQTT_CLIENT_ID, mqtt_client_id, &len);

            ESP_LOGI(TAG, "NVS MQTT client id: %s", mqtt_client_id);
        }

        /* Read server IP from flash */
        if (nvs_get_str(nvs, NVS_MQTT_BROKER_IP, NULL, &len) == ESP_OK) {
            broker_ip = calloc(1, len + 1);
            if (broker_ip == NULL) {
                goto error;
            }

            nvs_get_str(nvs, NVS_MQTT_BROKER_IP, broker_ip, &len);

            ESP_LOGI(TAG, "NVS MQTT server IP: %s", broker_ip);
        }
    }

    /* No client name found in flash, use chip mac */
    if (mqtt_client_id == NULL)
        mqtt_client_id = nvs_get_base_mac();

    sub_topic = malloc(strlen(MQTT_SUB_TOPIC_PREFIX) + strlen(mqtt_client_id) + 1);
    if (sub_topic == NULL) {
        goto error;
    }
    sprintf(sub_topic, "%s%s", MQTT_SUB_TOPIC_PREFIX, mqtt_client_id);

    mqtt_pub_topic = malloc(strlen(MQTT_PUB_TOPIC_PREFIX) + strlen(mqtt_client_id) + 1);
    if (mqtt_pub_topic == NULL) {
        goto error;
    }
    sprintf(mqtt_pub_topic, "%s%s", MQTT_PUB_TOPIC_PREFIX, mqtt_client_id);

    mqtt_client_info.broker = broker_ip;
    mqtt_client_info.sub_topic = sub_topic;
    mqtt_client_info.sub_qos = MQTT_SUB_QOS;
    mqtt_client_info.message_received_cb = cmd_recv_cb;
    mqtt_client_info.client_id = mqtt_client_id;

    if (mqtt_client_start(&mqtt_client_info) != ESP_OK) {
        goto error;
    }

    return ESP_OK;
error:
    ESP_LOGI(TAG, "MQTT client start failed!");

    if (mqtt_client_id != nvs_get_base_mac())
        free(mqtt_client_id);
    free(broker_ip);
    free(sub_topic);
    free(mqtt_pub_topic);

    return ESP_FAIL;
}

static esp_err_t cmd_do_reboot(cJSON *root)
{
    cJSON * ap_mode = NULL;

    ap_mode = cJSON_GetObjectItemCaseSensitive(root, CMD_JSON_AP_MODE);
    if (ap_mode != NULL) {
        if (!cJSON_IsNumber(ap_mode)) {
            ESP_LOGE(TAG, "Wrong DO_REBOOT format!");
            return ESP_FAIL;
        }

        /* Save the AP mod in flash*/
        if (nvs_get_handle()) {        
            if (nvs_set_u8(nvs_get_handle(), NVS_WIFI_AP_MODE, ap_mode->valueint) != ESP_OK) {
                ESP_LOGI(TAG, "Failed to write the AP mode!");
                return ESP_FAIL;
            }
        }
    }

    do_reboot();

    return ESP_FAIL;
}

/*
 * CMD result JSON format:
 * {
 *        "cmd":  1,
 *        "id":   "84f3eb23bcd5",
 *        "time": 1550306592,
 *        "res":  1
 *}
 */

static esp_err_t send_cmd_result(int cmd, esp_err_t res)
{
    cJSON *root = NULL;
    char * string;
    esp_err_t ret;

    root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Could not create JSON object!");
        return ESP_FAIL;
    }

    if (!cJSON_AddNumberToObject(root, CMD_JSON_CMD, cmd)                  ||
        !cJSON_AddStringToObject(root, CMD_JSON_CLIENT_ID, mqtt_client_id) ||
        !cJSON_AddNumberToObject(root, CMD_JSON_TIME, time(NULL))          ||
        !cJSON_AddStringToObject(root, CMD_JSON_RESULT,
                                 res == ESP_OK ? "OK" : "ERROR")) {
        ESP_LOGE(TAG, "Could not add info to the response JSON!");

        cJSON_Delete(root);
        return ESP_FAIL;
    }

    string = cJSON_Print(root);

    ret = mqtt_client_publish(mqtt_client_info.ctrl_handle,
            mqtt_pub_topic, (const uint8_t*)string, strlen(string),
            MQTT_PUB_QOS, 0);

    /* Free allocated items */
    cJSON_Delete(root);
    free(string);

    return ret;
}

static esp_err_t cmd_do_ota(cJSON *root)
{
    cJSON * server = NULL;
    cJSON * port = NULL;
    cJSON * file = NULL;

    server = cJSON_GetObjectItemCaseSensitive(root, CMD_JSON_SERVER);
    if (server == NULL || !cJSON_IsString(server)) {
        ESP_LOGE(TAG, "Wrong OTA server format!");
        return ESP_FAIL;
    }

    port = cJSON_GetObjectItemCaseSensitive(root, CMD_JSON_PORT);
    if (port == NULL || !cJSON_IsNumber(port)) {
        ESP_LOGE(TAG, "Wrong OTA port format!");
        return ESP_FAIL;
    }

    file = cJSON_GetObjectItemCaseSensitive(root, CMD_JSON_FILENAME);
    if (file == NULL || !cJSON_IsString(file)) {
        ESP_LOGE(TAG, "Wrong OTA file format!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CMD OTA  server %s, port %d, file %s",
             server->valuestring, port->valueint, file->valuestring);

    if (ota_start(server->valuestring, port->valueint, file->valuestring, NULL) == ESP_OK) {
        send_cmd_result(CMD_DO_OTA, ESP_OK);

        do_reboot();
    }

    return ESP_FAIL;
}

static esp_err_t cmd_set_mqtt_client_name(cJSON *root)
{
    cJSON * client_name = NULL;

    client_name = cJSON_GetObjectItemCaseSensitive(root, CMD_JSON_CLIENT_NAME);
    if (client_name == NULL || !cJSON_IsString(client_name)) {
        ESP_LOGE(TAG, "Wrong MQTT client name!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CMD SET MQTT client name: %s", client_name->valuestring);

    /* Save in flash, will be taken into consideration at the next reboot */
    if (nvs_get_handle()) {
        if (nvs_set_str(nvs_get_handle(), NVS_MQTT_CLIENT_ID, client_name->valuestring) != ESP_OK) {
            ESP_LOGI(TAG, "Failed to write the MQTT client name!");
            return ESP_FAIL;
        }

        send_cmd_result(CMD_SET_MQTT_CLIENT_NAME, ESP_OK);

        do_reboot();
    }

    return ESP_FAIL;
}

static esp_err_t cmd_set_mqtt_broker_ip(cJSON *root)
{
    cJSON * broker_ip = NULL;

    broker_ip = cJSON_GetObjectItemCaseSensitive(root, CMD_JSON_BROKER_IP);
    if (broker_ip == NULL || !cJSON_IsString(broker_ip)) {
        ESP_LOGE(TAG, "Wrong server IP!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CMD SET MQTT server IP: %s", broker_ip->valuestring);

    /* Save in flash, will be taken into consideration at the next reboot */
    if (nvs_get_handle()) {
        if (nvs_set_str(nvs_get_handle(), NVS_MQTT_BROKER_IP, broker_ip->valuestring) != ESP_OK) {
            ESP_LOGI(TAG, "Failed to write the MQTT server IP!");
            return ESP_FAIL;
        }
        send_cmd_result(CMD_SET_MQTT_SERVER_IP, ESP_OK);

        do_reboot();
    }

    return ESP_FAIL;
}

/*
 * Sensors info JSON format:
 * {
 *        "cmd":  3,
 *        "id":   "84f3eb23bcd5",
 *        "time": 1550306285,
 *        "co2":  123
 *        "temp": 2345,
 *        "humidity":     5123,
 * }
 */

esp_err_t send_ups_info(ups_data_t *ups_data)
{
    cJSON *root = NULL;
    char * string;
    esp_err_t ret;

    root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Could not create JSON object!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Send ups info: Vout %d, Iout %d, Vbat %d, Vin %d",
             ups_data->v_out, ups_data->i_out, ups_data->v_bat, ups_data->v_in);

    if (!cJSON_AddNumberToObject(root, CMD_JSON_CMD, CMD_GET_UPS_INFO)          ||
        !cJSON_AddStringToObject(root, CMD_JSON_CLIENT_ID, mqtt_client_id)      ||
        !cJSON_AddNumberToObject(root, CMD_JSON_TIME, time(NULL))               ||
        !cJSON_AddNumberToObject(root, CMD_JSON_VOUT, ups_data->v_out)          ||
        !cJSON_AddNumberToObject(root, CMD_JSON_IOUT, ups_data->i_out)          ||
        !cJSON_AddNumberToObject(root, CMD_JSON_VBAT, ups_data->v_bat)          ||
        !cJSON_AddNumberToObject(root, CMD_JSON_VIN, ups_data->v_in)            ||
        !cJSON_AddNumberToObject(root, CMD_JSON_POFF, ups_data->power_off )     ||
        !cJSON_AddBoolToObject(root, CMD_JSON_FAN, ups_data->fan_high)          ||
        !cJSON_AddNumberToObject(root, CMD_JSON_ADC_ERR, ups_data->adc_errors)  ||
        !cJSON_AddNumberToObject(root, CMD_JSON_BATD, ups_data->bat_discharged) ||
        !cJSON_AddBoolToObject(root, CMD_JSON_BATC, ups_data->bat_connected)) {
        ESP_LOGE(TAG, "Could not add ups info to JSON!");

        cJSON_Delete(root);
        return ESP_FAIL;
    }

    string = cJSON_Print(root);

    ret = mqtt_client_publish(mqtt_client_info.ctrl_handle,
            mqtt_pub_topic, (const uint8_t*)string, strlen(string),
            MQTT_PUB_QOS, 0);

    /* Free allocated items */
    cJSON_Delete(root);
    free(string);

    return ret;
}

static esp_err_t cmd_set_display_brightness(cJSON *root)
{

    return ESP_FAIL;
}


static void cmd_recv(cmd_data_t * cmd)
{
    cJSON *root = NULL;
    cJSON *cmd_nr = NULL;
    esp_err_t ret = ESP_OK;

    root = cJSON_Parse((char *)cmd->data);
    if (root == NULL) {
        ESP_LOGE(TAG, "Could not create JSON object!");
        return;
    }

    cmd_nr = cJSON_GetObjectItemCaseSensitive(root, CMD_JSON_CMD);
    if (cmd_nr == NULL ||
        !cJSON_IsNumber(cmd_nr)) {
        ESP_LOGE(TAG, "Wrong command format!");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Command number: %d", cmd_nr->valueint);

    switch(cmd_nr->valueint) {
        case CMD_DO_REBOOT:
            ret = cmd_do_reboot(root);

            /* Reboot failed if we are here */
            send_cmd_result(CMD_DO_REBOOT, ESP_FAIL);

            break;

        case CMD_DO_OTA:
            ret = cmd_do_ota(root);

            /* OTA failed if we are here */
            send_cmd_result(CMD_DO_OTA, ESP_FAIL);

            break;

        case CMD_GET_SYS_INFO:
            ret = send_sys_info();
            break;

        case CMD_GET_UPS_INFO:
        {
            ups_data_t ups_data;

            ret = ESP_FAIL;
            if (ups_get_data(&ups_data) == ESP_OK) {
                ret = send_ups_info(&ups_data);
            }

            break;
        }

        case CMD_SET_MQTT_CLIENT_NAME:
            ret = cmd_set_mqtt_client_name(root);

            /* CMD failed if we are here */
            send_cmd_result(CMD_SET_MQTT_CLIENT_NAME, ESP_FAIL);

            break;

        case CMD_SET_MQTT_SERVER_IP:
            ret = cmd_set_mqtt_broker_ip(root);

            /* CMD failed if we are here */
            send_cmd_result(CMD_SET_MQTT_SERVER_IP, ESP_FAIL);

            break;

        case CMD_SET_DISPLAY_BRIGHTNESS:
            ret = cmd_set_display_brightness(root);

            send_cmd_result(CMD_SET_DISPLAY_BRIGHTNESS, ret);

            break;

        default:
            ESP_LOGE(TAG, "Command %d not implemented!", cmd_nr->valueint);
            break;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Command %d failed!", cmd_nr->valueint);
    }

    cJSON_Delete(root);
}

static void cmd_recv_task(void *arg)
{
    cmd_data_t * cmd;
    int delay = CMD_MQTT_DELAY_MIN;
    
    /* Command receiving is ready, but first send a sys info message to the MQTT broker */
    while (send_sys_info() != ESP_OK) {
        vTaskDelay(delay / portTICK_RATE_MS);
        
        /* Exponential delay*/
        delay <<= 1;
        if (delay > CMD_MQTT_DELAY_MAX)
            delay = CMD_MQTT_DELAY_MAX;
    }

    ESP_LOGI(TAG, "Sent SYS_INFO, ready to receive commands");

    while (1) {
        if (xQueueReceive(cmd_recv_queue, &cmd, portMAX_DELAY)) {
            ESP_LOGI(TAG, "Cmd received: \"%s\"", cmd->data);
            
            if (cmd->last) {
                cmd_recv(cmd);
            } else {
                ESP_LOGE(TAG, "Fragmented command. Not supported yet!");
            }

            free(cmd->data);
            free(cmd);
        }
    }
}

/*
 * Sys info JSON format:
 * {
 *        "cmd":  2,
 *        "id":   "84f3eb23bcd5",
 *        "mac"   "84f3eb23bcd5",
 *        "time": 1550306275,
 *        fw_v:   "0.0.5",
 *        heap:   60784,
 *        up:     38
 *}
 */

esp_err_t send_sys_info()
{
    cJSON *root = NULL;
    char * string;
    uint32_t uptime;
    esp_err_t ret;
    uint16_t baseline = 0;

    /* uptime in seconds. 100Hz tick rate => 497 days */
    uptime = xTaskGetTickCount() / xPortGetTickRateHz();

    root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Could not create JSON object!");
        return ESP_FAIL;
    }

    nvs_get_u16(nvs_get_handle(), NVS_CCS811_BASELINE, &baseline);

    if (!cJSON_AddNumberToObject(root, CMD_JSON_CMD, CMD_GET_SYS_INFO)          ||
        !cJSON_AddStringToObject(root, CMD_JSON_CLIENT_ID, mqtt_client_id)      ||
        !cJSON_AddStringToObject(root, CMD_JSON_CHIP_MAC, nvs_get_base_mac())   ||
        !cJSON_AddNumberToObject(root, CMD_JSON_CCS811_BASELINE, baseline)      ||
        !cJSON_AddNumberToObject(root, CMD_JSON_TIME, time(NULL))               ||
        !cJSON_AddStringToObject(root, CMD_JSON_FW_VER, FW_VERSION)             ||
        !cJSON_AddNumberToObject(root, CMD_JSON_HEAP, esp_get_free_heap_size()) ||
        !cJSON_AddNumberToObject(root, CMD_JSON_UPTIME, uptime)) {
        ESP_LOGE(TAG, "Could not add sys info to JSON!");
    
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    string = cJSON_Print(root);

    ret = mqtt_client_publish(mqtt_client_info.ctrl_handle,
            mqtt_pub_topic, (const uint8_t*)string, strlen(string),
            MQTT_PUB_QOS, 0);

    /* Free allocated items */
    cJSON_Delete(root);
    free(string);
    
    return ret;
}

esp_err_t cmd_recv_init(void)
{
    cmd_recv_queue = xQueueCreate(CMD_PARSE_QUEUE_LEN, sizeof(uint32_t));
    if (cmd_recv_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create cmd queue!");
        return ESP_FAIL;
    }

    if (mqtt_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client!");
        return ESP_FAIL;
    }

    if (xTaskCreate(cmd_recv_task,
                    CMD_RECV_TASK_NAME,
                    CMD_RECV_TASK_STACK,
                    NULL,
                    CMD_RECV_TASK_PRIO,
                    NULL) != pdPASS)  {
        /* FATAL error, do not care about stopping MQTT client */
        return ESP_FAIL;
    }

    return ESP_OK;
}
