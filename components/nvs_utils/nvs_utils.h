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

#ifndef __FLASH_UTILS_H__
#define __FLASH_UTILS_H__

#include "nvs.h"
#include "nvs_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Settings saved in the NVS, max key length is currently 15 characters */
#define NVS_MQTT_CLIENT_ID       "MqttClienId"
#define NVS_MQTT_BROKER_IP       "MqttBrokerIP"
#define NVS_DISPLAY_BRIGHTNESS   "Brightness"
#define NVS_POWER_OFF            "PowerOff"
#define NVS_BATTERY_DISCHARGED   "BatDischarged"

#define NVS_WIFI_AP_MODE         "WiFiApMode"
#define NVS_WIFI_SSID            "WiFiSSID"
#define NVS_WIFI_PASS            "WiFiPass"

#define NVS_CCS811_BASELINE      "baseline"

nvs_handle nvs_get_handle();
char *     nvs_get_base_mac();
esp_err_t  nvs_init();

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H__ */
