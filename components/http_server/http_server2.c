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
#include "lwip/api.h"

#include "esp_log.h"
#include "esp_system.h"

#include "nvs_utils.h"

#define HTTP_SERVER_TASK_NAME   "httpd"
#define HTTP_SERVER_TASK_STACK  2048
#define HTTP_SERVER_TASK_PRIO   5

#define HTTP_PORT               80
#define HTTP_SEND_BUF_SIZE      400

#define WIFI_SSID_LEN_MAX       24
#define WIFI_PASS_LEN_MAX       24

#define HTTP_SSID_INPUT_NAME    "wifi_ssid"
#define HTTP_PASS_INPUT_NAME    "wifi_pass"

static const char *TAG = "HTTPD";

void http_server_task(void *pvParameters)
{
    struct netconn *conn, *newconn;
    struct netbuf *nb;
    char *send_buf;
    void *rcv_buf;
    uint16_t rcv_len;
    char ssid[WIFI_SSID_LEN_MAX + 1];
    char pass[WIFI_PASS_LEN_MAX + 1];
    bool got_ssid = false;
    bool got_pass = false;

    /* Create a new connection */
    conn = netconn_new(NETCONN_TCP);
    if (conn == NULL) {
        ESP_LOGE(TAG, "Failed to create new TCP connection!");
        return;
    }

    send_buf = malloc(HTTP_SEND_BUF_SIZE);
    if (send_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate send buffer!");
        return;
    }

    if (netconn_bind(conn, IP_ADDR_ANY, HTTP_PORT) != ERR_OK ||
        netconn_listen(conn) != ERR_OK) {
        ESP_LOGE(TAG, "Failed to bind/listen on the TCP socket!");
        free(send_buf);
        return;
    }

    ESP_LOGI(TAG, "HTTP server started");
    while (1) {
        if (netconn_accept(conn, &newconn) == ERR_OK) {
            if (netconn_recv(newconn, &nb) == ERR_OK) {
                netbuf_data(nb, &rcv_buf, &rcv_len);                

                /* We only handle HTTP GET or POST */
                if (strncmp(rcv_buf, "GET ", 4) != 0 &&
                    strncmp(rcv_buf, "POST ", 5) != 0) {
                    netbuf_delete(nb);
                    ESP_LOGI(TAG, "Received unsupported HTTP method");
                    continue;
                }

                if (!strncmp(rcv_buf, "POST ", 5)) {
                    char *ssid_start;
                    char *pass_start;
                    uint32_t str_len;

                    ESP_LOGI(TAG, "Received HTTP POST request");

                    /* Check for SSID and password*/
                    if ((ssid_start = strstr(rcv_buf, HTTP_SSID_INPUT_NAME)) != NULL &&
                        (pass_start = strstr(rcv_buf, HTTP_PASS_INPUT_NAME)) != NULL) {

                        got_ssid = false;
                        got_pass = false;

                        /* Response looks like this: wifi_ssid=my_ssid&wifi_pass=my_pass */
                        ssid_start += strlen(HTTP_SSID_INPUT_NAME) + 1;
                        str_len = pass_start - ssid_start - 1;
                        if (str_len + 1 < sizeof(ssid)) {
                            memcpy(ssid, ssid_start, str_len);
                            ssid[str_len] = 0;
                            got_ssid = true;
                            ESP_LOGI(TAG, "Got SSID: %s", ssid);
                        }
                  
                        pass_start += strlen(HTTP_PASS_INPUT_NAME) + 1;
                        str_len = rcv_len - ((uint32_t)pass_start - (uint32_t)rcv_buf);
                        if (str_len + 1 < sizeof(pass)) {
                            memcpy(pass, pass_start, str_len);
                            pass[str_len] = 0;
                            got_pass = true;
                            ESP_LOGI(TAG, "Got password: %s", pass);
                        }
                    }
                } else {
                    ESP_LOGI(TAG, "Received HTTP GET request");
                }

                if (!got_ssid || !got_pass) {
                    snprintf(send_buf, HTTP_SEND_BUF_SIZE,
                        "HTTP/1.1 200 OK\r\n"
                        "Content-type: text/html\r\n\r\n"
                        "<!DOCTYPE html>"
                        "<html>"
                        "<body>"
                        "<font size=\"+1\">"
                        "<form action=\"/action_page.php\" method=\"post\">"
                        "SSID    : <input type=\"text\" name=\""HTTP_SSID_INPUT_NAME"\"><br>"
                        "Password: <input type=\"password\" name=\""HTTP_PASS_INPUT_NAME"\"><br>"
                        "<input type=\"submit\" value=\"Submit\">"
                        "</form>"
                        "</font>"
                        "</body>"
                         "</html>"
                        );
                } else {
                    /* Got SSID and password */
                    snprintf(send_buf, HTTP_SEND_BUF_SIZE,
                        "HTTP/1.1 200 OK\r\n"
                        "Content-type: text/html\r\n\r\n"
                        "<!DOCTYPE html>"
                        "<html>"
                        "<body>"
                        "<font size=\"+2\">"
                        "SSID and password saved! The device will reboot in station mode!"
                        "</font>"
                        "</body>"
                        "</html>"
                        );
                }
                if (netconn_write(newconn, send_buf, strlen(send_buf), NETCONN_COPY) == ERR_OK) {
                    ESP_LOGI(TAG, "Sent response to the client, len %d", strlen(send_buf));
                } else {
                    ESP_LOGI(TAG, "Failed to sent response to the client");
                }

                netbuf_delete(nb);
            }
            netconn_close(newconn);
            netconn_delete(newconn);
            
            if (got_ssid && got_pass) {
                 nvs_handle nvs = nvs_get_handle();
                
                /* Save SSID and password, set station mode and reboot */
                if (nvs_set_str(nvs, NVS_WIFI_SSID, ssid) != ESP_OK ||
                    nvs_set_str(nvs, NVS_WIFI_PASS, pass) != ESP_OK ||
                    nvs_set_u8(nvs, NVS_WIFI_AP_MODE, 0) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save the new credentials! Rebooting ...!");
                } else {
                    ESP_LOGI(TAG, "Rebooting in station mode...");
                }
                
                vTaskDelay(1000 / portTICK_RATE_MS);
                esp_restart();
            }
        }
    }
}

void http_server_init()
{
    xTaskCreate(&http_server_task,
                HTTP_SERVER_TASK_NAME,
                HTTP_SERVER_TASK_STACK,
                NULL,
                HTTP_SERVER_TASK_PRIO, 
                NULL);
}
