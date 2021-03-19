
#include "ssd1306.h"
#include <stdint.h>

#define HAL_MAX_DELAY   100

void HAL_Delay(int ms);
uint32_t HAL_GetTick(void);
uint32_t HAL_GetTickRate(void);

#if defined(SSD1306_USE_I2C)
void HAL_I2C_Mem_Write(int port, uint8_t addr, uint8_t reg, uint8_t, uint8_t * data, int size, int delay);
#endif 
