/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Adrian Bradianu (github.com/abradianu)
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

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2c.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"

#include "lwip/apps/sntp.h"
#include "cmd_recv.h"
#include "nvs_utils.h"
#include "http_server2.h"
#include "ssd1306.h"
#include "ads111x.h"
#include "ssd1306_fonts.h"
#include "ssd1306_tests.h"

#include "ups.h"

/* Main task settings */
#define MAIN_TASK_LOOP_DELAY           400
#define MAIN_TASK_PRIORITY             10
#define MAIN_TASK_STACK_SIZE           4096

/* Led is connected to GPIO16 on NodeMcu board */
#define GPIO_BLUE_LED                  16

/* I2C pins */
#define GPIO_I2C_MASTER_SCL            4
#define GPIO_I2C_MASTER_SDA            5

/* Vbuck status GPIO input */
#define GPIO_VBUCK_STATUS              12

/* Battery control GPIO output */
#define GPIO_BATTERY_CONTROL           13

/* Fan control GPIO output */
#define GPIO_FAN_CONTROL               15

#define BATTERY_DISCONNECT             0
#define BATTERY_CONNECT                1

#define FAN_LOW                        0
#define FAN_HIGH                       1

/* v_in good voltage in mV */
#define V_IN_GOOD                      15000

/* 25% remaining */
#define V_BAT_DISCHARGED               11750

/* battery is partial charged */
#define V_BAT_CHARGED                  12500

/* max current in mA */
#define CURRENT_MAX                    3000

/* short circuit rezistor in mili ohmi */
#define REZISTOR_SC                    100

#define V_BAT_FAN_LOW                  13400
#define I_OUT_FAN_LOW                  1000
#define FAN_MIN_PERIOD                 30

#define ADC_BUSY_RETRIES               10

/*
 * std offset dst [offset],start[/time],end[/time]
 * There are no spaces in the specification. The initial std and offset specify
 * the standard timezone. The dst string and offset specify the name and offset
 * for the corresponding daylight saving timezone. If the offset is omitted,
 * it default to one hour ahead of standard time. The start field specifies
 * when daylight saving time goes into effect and the end field specifies when
 * the change is made back to standard time. These fields may have the following
 * formats: Mm.w.d
 * This specifies day d (0 <= d <= 6) of week w (1 <= w <= 5) of month m
 * (1 <= m <= 12). Week 1 is the first week in which day d occurs and week 5 is
 * the last week in which day d occurs. Day 0 is a Sunday. 
 */

#define TIMEZONE                       "EET-2EEST-3,M3.5.0,M10.5.0"

#define FATAL_ERROR(fmt, args...)                     \
do {                                                  \
    ESP_LOGE(TAG, fmt,## args);                       \
    ESP_LOGE(TAG, "Rebooting in 2 seconds");          \
    vTaskDelay(2000 / portTICK_RATE_MS);              \
    esp_restart();                                    \
} while (0)

typedef enum {
    WIFI_AP_MODE,
    WIFI_STA_DISCONNECTED,
    WIFI_STA_CONNECTED,
} wifi_state_t;

static const char *TAG = "UPS";
static wifi_state_t wifi_state;
static i2c_dev_t adc_dev;
static ups_data_t ups_data;
static SemaphoreHandle_t ups_mutex = NULL;

static void sntp_start(void)
{
    ESP_LOGI(TAG, "SNTP start");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case SYSTEM_EVENT_STA_GOT_IP:
            wifi_state = WIFI_STA_CONNECTED;
            ESP_LOGI(TAG, "WiFi connected");
            break;

        case SYSTEM_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "station:"MACSTR" join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
            break;

        case SYSTEM_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "station:"MACSTR"leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
            break;
            
        case SYSTEM_EVENT_STA_DISCONNECTED:
            wifi_state = WIFI_STA_DISCONNECTED;
            ESP_LOGI(TAG, "WiFi disconnected");

            /* This is a workaround as ESP8266 WiFi libs don't currently
               auto-reassociate. */
            esp_wifi_connect();
            break;

        default:
            break;
    }

    return ESP_OK;
}

static void wifi_init_softap(void)
{
    char* base_mac = nvs_get_base_mac();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    tcpip_adapter_init();
    if (esp_event_loop_init(event_handler, NULL) != ESP_OK ||
        esp_wifi_init(&cfg) != ESP_OK) {
        FATAL_ERROR("Could not init WiFi!");
    }

    /* Base mac is both the SSID and password */
    strcpy((char *)wifi_config.ap.ssid, base_mac);
    strcpy((char *)wifi_config.ap.password, base_mac);
    wifi_config.ap.ssid_len = strlen(base_mac);

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK                   ||
        esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        FATAL_ERROR("Could not start AP mode!");
    }

    ESP_LOGI(TAG, "Init softap with SSID %s pass %s",
             wifi_config.ap.ssid, wifi_config.ap.password);
}

static void wifi_init_sta(char* ssid, char* pass)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config;

    tcpip_adapter_init();
    if (esp_event_loop_init(event_handler, NULL) != ESP_OK ||
        esp_wifi_init(&cfg) != ESP_OK                      ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        FATAL_ERROR("Could not init WiFi!");
    }

    memset(&wifi_config, 0, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ssid);
    strcpy((char *)wifi_config.sta.password, pass);
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK                   ||
        esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        FATAL_ERROR("Could not stat station mode!");
    }
}

static esp_err_t uart_init(void)
{
    ESP_LOGI(TAG, "UART init");

    /* Configure UART to 115200, 8N1*/   
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    return (uart_param_config(0, &uart_config));
}

static esp_err_t adc_init(void)
{
    ESP_LOGI(TAG, "ADC init");
    
    if (ads111x_init_desc(&adc_dev, ADS111X_ADDR_GND, I2C_NUM_0, 
                      GPIO_I2C_MASTER_SDA, GPIO_I2C_MASTER_SCL))
    {
        ESP_LOGE(TAG, "ADC init failed!");
        return ESP_FAIL;
    }
    
    if (ads111x_set_data_rate(&adc_dev, ADS111X_DATA_RATE_64) != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC set data rate failed!");
        return ESP_FAIL;
    }

    if (ads111x_set_mode(&adc_dev, ADS111X_MODE_SINGLE_SHOT) != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC set mode failed!");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t adc_read(int chan, int * voltage)
{
    bool not_busy = false;
    int16_t raw = 0;
    int retries = 0;
    /* gain * 1000 */
    static int gain[] = {0, 0, 0, 0, 18840, 22270, 48000, 2048};

    if (ads111x_set_input_mux(&adc_dev, chan) != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC failed to set input on chan %d!", chan);
        return ESP_FAIL;
    }
    
    if (ads111x_start_conversion(&adc_dev) != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC failed to start conversion on chan %d!", chan);
        return ESP_FAIL;
    }

    /* Wait for conversion to finish, for 64 SPS conversion time is ~15ms */
    vTaskDelay(20 / portTICK_RATE_MS);
    do
    {
        ads111x_is_busy(&adc_dev, &not_busy);
        if (not_busy)
            break;
        retries++;
        vTaskDelay(1);
    } while (retries < ADC_BUSY_RETRIES);

    if (retries == ADC_BUSY_RETRIES)
    {
        ESP_LOGE(TAG, "ADC conversion timeout on chan %d!", chan);
        return ESP_FAIL;
    }
    
    if (ads111x_get_value(&adc_dev, &raw) == ESP_OK)
    {
        *voltage = ((int)raw * gain[chan]) / ADS111X_MAX_VALUE;
        ESP_LOGV(TAG, "ADC chan %d raw value %d, voltage: %d mV\n",
                 chan, raw, *voltage);
    }
    else
    {
        ESP_LOGE(TAG, "ADC get value failed on chan %d!", chan);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static esp_err_t gpio_init(void)
{
    gpio_config_t io_conf;

    /* Config LED GPIO */
    ESP_LOGI(TAG, "GPIO init");

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1 << GPIO_BLUE_LED) |
                           (1 << GPIO_BATTERY_CONTROL) |
                           (1 << GPIO_FAN_CONTROL);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    if (gpio_config(&io_conf) != ESP_OK)
        return ESP_FAIL;

    /* Config inputs */
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1 << GPIO_VBUCK_STATUS);
    io_conf.pull_up_en = 1;
    return gpio_config(&io_conf);

    return ESP_OK;
}

static void main_task(void *arg)
{
    uint8_t blink_level = 1;
    uint32_t power_off = 0;
    uint32_t bat_discharged = 0;
    uint32_t adc_errors = 0;
    TickType_t fan_tick_count = 0;
    bool init_done = false;
    int v_out, i_out, v_bat, v_in, v_sc, v_bat_prev, i_out_prev;
    char text[16];
    bool first_time = true;
    bool bat_connected = false;
    bool power_is_on = false;
    bool fan_high = false;

    /* Display banner */
    ssd1306_SetCursor(2, 4);
    ssd1306_WriteString("12V UPS", Font_16x26, White);
    ssd1306_SetCursor(2, 40);
    ssd1306_WriteString("FW: "FW_VERSION, Font_11x18, White);
    ssd1306_UpdateScreen();

    nvs_get_u32(nvs_get_handle(), NVS_POWER_OFF, &power_off);
    nvs_get_u32(nvs_get_handle(), NVS_BATTERY_DISCHARGED, &bat_discharged);

    /* Wait 2 seconds for voltages to be stable and display banner */
    vTaskDelay(2000 / portTICK_RATE_MS);

    ssd1306_Fill(Black);
    ssd1306_UpdateScreen();

    while (1) {
        if (wifi_state == WIFI_STA_CONNECTED && !init_done) {

            /* We are connected to WiFi now */
            sntp_start();

            /* Init the MQTT command receiving logic */
            if (cmd_recv_init() != ESP_OK) {
                FATAL_ERROR("CMD not started!");
            }
            
            init_done = true;
        }

        blink_level ^= 1;
        gpio_set_level(GPIO_BLUE_LED, blink_level);

        if (adc_read(ADS111X_MUX_0_GND, &v_bat) != ESP_OK ||
            adc_read(ADS111X_MUX_1_GND, &v_out) != ESP_OK ||
            adc_read(ADS111X_MUX_2_GND, &v_in)  != ESP_OK ||
            adc_read(ADS111X_MUX_3_GND, &v_sc)  != ESP_OK)
        {
            adc_errors++;
            vTaskDelay(1);
            continue;
        }

        if (v_in < 0) v_in = 0;
        if (v_sc < 0) v_sc = 0;
        i_out = (v_sc * 1000) / REZISTOR_SC;
        if (first_time)
        {
            v_bat_prev = v_bat;
            i_out_prev = i_out;
        }

        /* low pass filter, v_bat is noisy when battery is fully charged */
        v_bat = v_bat_prev + (v_bat - v_bat_prev) / 5;
        v_bat_prev = v_bat;

        i_out = i_out_prev + (i_out - i_out_prev) / 5;
        i_out_prev = i_out;

        /* Check battery state */
        if (bat_connected)
        {
            if (v_bat < V_BAT_DISCHARGED)
            {
                /* Battery is discharged, disconnect the battery */
                gpio_set_level(GPIO_BATTERY_CONTROL, BATTERY_DISCONNECT);
                
                bat_connected = false;
                bat_discharged++;
                nvs_set_u32(nvs_get_handle(), NVS_BATTERY_DISCHARGED, bat_discharged);
                
                ESP_LOGI(TAG, "Battery discharged and disconnected!");
            }
        }
        else            
        {
            /* Battery is disconnected until power is back on */
            if (v_in > V_IN_GOOD || first_time)
            {
                /* Power is on */
                
                if (v_bat > V_BAT_CHARGED)
                {
                    /* Battery is partial charged, connect the battery */
                    gpio_set_level(GPIO_BATTERY_CONTROL, BATTERY_CONNECT);

                    bat_connected = true;
                    
                    ESP_LOGI(TAG, "Battery charged and connected!");
                }
            }
        }
        first_time = false;

        /* Check power on state */
        if (v_in < V_IN_GOOD && power_is_on)
        {
            power_is_on = false;

            power_off++;
            nvs_set_u32(nvs_get_handle(), NVS_POWER_OFF, power_off);
            
            ESP_LOGI(TAG, "Power off!");
        }
        else if (v_in >= V_IN_GOOD && !power_is_on)
        {
            power_is_on = true;
            
            ESP_LOGI(TAG, "Power on!");
        }


        /* Check fan state, minimum period once state changed */
        if (xTaskGetTickCount() - fan_tick_count > FAN_MIN_PERIOD * xPortGetTickRateHz())
        {
            if (fan_high)
            {
                if (v_bat > V_BAT_FAN_LOW && i_out < I_OUT_FAN_LOW)
                {
                    /* Set fan to low speed */
                    gpio_set_level(GPIO_FAN_CONTROL, FAN_LOW);
                    fan_tick_count = xTaskGetTickCount();
                    fan_high = false;
                }
            }
            else
            {
                if (v_bat < V_BAT_FAN_LOW || i_out > I_OUT_FAN_LOW)
                {
                    /* Set fan to high speed */
                    gpio_set_level(GPIO_FAN_CONTROL, FAN_HIGH);
                    fan_tick_count = xTaskGetTickCount();
                    fan_high = true;
                }
            }
        }

        xSemaphoreTake(ups_mutex, portMAX_DELAY);
        ups_data.v_out = v_out;
        ups_data.i_out = i_out;
        ups_data.v_bat = v_bat;
        ups_data.v_in = v_in;
        ups_data.power_off = power_off;
        ups_data.bat_discharged = bat_discharged;
        ups_data.bat_connected = bat_connected;
        ups_data.fan_high = fan_high;
        ups_data.adc_errors = adc_errors;
        xSemaphoreGive(ups_mutex);

        /* Display first text row, Vout and Iout */
        v_out = v_out + 50;
        i_out = ((v_sc * 1000) / REZISTOR_SC) + 5;
        if (i_out > CURRENT_MAX && blink_level == 0)
        {
            /* Out current over limit, blink text */
            snprintf(text, sizeof(text), "             ");
        }
        else
        {
            snprintf(text, sizeof(text), "%02d.%dV %d.%02dA",
                     v_out / 1000, (v_out % 1000) / 100,
                     i_out / 1000, (i_out % 1000) / 10);
        }
        ssd1306_SetCursor(2, 0);
        ssd1306_WriteString(text, Font_11x18, White);

        /* Display second text row, Vbat */
        v_bat = v_bat + 50;
        if (!bat_connected && blink_level == 0)
        {
            /* Battery not connected, blink text */
            snprintf(text, sizeof(text), "             ");
        }
        else
        {
            snprintf(text, sizeof(text), "Vbat%c %d.%dV ", blink_level ? ':':' ',
                v_bat / 1000, (v_bat % 1000) / 100);
        }
        ssd1306_SetCursor(2, 22);
        ssd1306_WriteString(text, Font_11x18, White);

        /* Display third text row, Poff status */
        v_in = v_in + 50;
        if (!power_is_on && blink_level == 0)
        {
            /* Power is off, blink text */
            snprintf(text, sizeof(text), "             ");
        }
        else
        {
            snprintf(text, sizeof(text), "Poff%c %d    ",  blink_level ? ':':' ',
                                         power_off);
        }
        ssd1306_SetCursor(2, 42);
        ssd1306_WriteString(text, Font_11x18, White);

        ssd1306_UpdateScreen();


        vTaskDelay(MAIN_TASK_LOOP_DELAY / portTICK_RATE_MS);
    }
}

esp_err_t ups_get_data(ups_data_t *data)
{
    xSemaphoreTake(ups_mutex, portMAX_DELAY);
    memcpy(data, &ups_data, sizeof(ups_data_t));
    xSemaphoreGive(ups_mutex);
    
    return ESP_OK;
}

void app_main()
{
    uint8_t ap_mode = 0;
    nvs_handle nvs;
    char wifi_ssid[24];
    char wifi_pass[24];

    /* Init drivers */
    if (uart_init() != ESP_OK   ||
        gpio_init() != ESP_OK   ||
        i2cdev_init() != ESP_OK ||
        nvs_init() != ESP_OK    ||
        adc_init() != ESP_OK) {
        FATAL_ERROR("Could not init drivers!");
    }

    /* Set fan to low speed */
    gpio_set_level(GPIO_FAN_CONTROL, FAN_LOW);
    
    /* Start with battery disconnected */
    gpio_set_level(GPIO_BATTERY_CONTROL, BATTERY_DISCONNECT);
    
    ESP_LOGI(TAG, "FW VERSION: %s", FW_VERSION);
    ESP_LOGI(TAG, "BASE MAC  : %s", nvs_get_base_mac());
    
    nvs = nvs_get_handle();

    ssd1306_Init();

    /* Read WiFi mode from flash*/
    nvs_get_u8(nvs, NVS_WIFI_AP_MODE, &ap_mode);
    if (!ap_mode) {
        size_t ssid_len, pass_len;

        ssid_len = sizeof(wifi_ssid);
        pass_len = sizeof(wifi_pass);
        if (nvs_get_str(nvs, NVS_WIFI_SSID, wifi_ssid, &ssid_len) != ESP_OK ||
            nvs_get_str(nvs, NVS_WIFI_PASS, wifi_pass, &pass_len) != ESP_OK) {
            /* User or password not found in flash, set AP mode */
            ap_mode = 1;
        
            ESP_LOGE(TAG, "---------------------------------------------------------------------------");
            ESP_LOGE(TAG, "Station mode set but no SSID/password found in flash. Switching to AP mode!");
            ESP_LOGE(TAG, "---------------------------------------------------------------------------");
        }
    }

    if (ap_mode) {
        wifi_state = WIFI_AP_MODE;
        wifi_init_softap();
        http_server_init();
    } else {
        wifi_state = WIFI_STA_DISCONNECTED;
        wifi_init_sta(wifi_ssid, wifi_pass);
    }

    /* Set timezone */
    setenv("TZ", TIMEZONE, 1);
    tzset();
    
    ups_mutex = xSemaphoreCreateMutex();
    if (ups_mutex == NULL)
    {
        FATAL_ERROR("Could not create mutex!");
    }

    if (xTaskCreate(main_task, "main_task", MAIN_TASK_STACK_SIZE, NULL,
        MAIN_TASK_PRIORITY, NULL) != pdPASS) {
        FATAL_ERROR("Main task could not be created!");
    }
}
