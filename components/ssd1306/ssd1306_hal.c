

#include "driver/i2c.h"
#include "freertos/task.h"
#include "ssd1306_hal.h"

#define ACK_CHECK_EN 1
 
void HAL_Delay(int ms)
{
    int ticks = ms / portTICK_RATE_MS;
    if (!ticks)
        ticks = 1;
    vTaskDelay(ticks);
}

uint32_t HAL_GetTick()
{
    return xTaskGetTickCount();
}

uint32_t HAL_GetTickRate()
{
    return xPortGetTickRateHz();
}

#if defined(SSD1306_USE_I2C)
void HAL_I2C_Mem_Write(int i2c_num, uint8_t addr, uint8_t reg, uint8_t res,
                       uint8_t * data, int data_len, int delay)
{
    int ret;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, reg, ACK_CHECK_EN);
    i2c_master_write(cmd, data, data_len, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(i2c_num, cmd, delay / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK)
        printf("I2C transfer filed!\n");
}
#endif
