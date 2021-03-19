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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"

#include "nvs_utils.h"

#define NVS_NAMESPACE            "DigitalClock"

static const char *TAG = "NVSU";

static nvs_handle nvs_flash_handle;

char * nvs_get_base_mac()
{
    uint8_t base_mac[6];
    static char * mac_str;

    if (mac_str == NULL) {        
        mac_str = malloc(2 * sizeof(base_mac) + 1);
        if (mac_str == NULL)
            return NULL;

        esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
        sprintf(mac_str, "%02x%02x%02x%02x%02x%02x",
            base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    }

    return mac_str;
}

nvs_handle nvs_get_handle()
{
    return nvs_flash_handle;
}

esp_err_t nvs_init()
{
    esp_err_t ret;

    /* Initialize NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not init NVS, ret 0x%x!", ret);
        return ret;
    }

    /* Open NVS flash */
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_flash_handle) != ESP_OK) {        
        ESP_LOGE(TAG, "Could not open NVS %s, ret 0x%x!", NVS_NAMESPACE, ret);
        return ESP_FAIL;
    }

    if (!nvs_flash_handle)
        return ESP_FAIL;

    return ESP_OK;
}
