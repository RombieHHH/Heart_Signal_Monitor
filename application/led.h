#ifndef APPLICATION_LED_H
#define APPLICATION_LED_H

#include <stdint.h>

typedef enum
{
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK,
    LED_MODE_BREATHE,
    LED_MODE_COUNT
} LED_Mode;

LED_Mode LED_GetMode(void);
uint8_t LED_GetBrightnessLevel(void);
uint8_t LED_GetBrightnessPercent(void);
const char *LED_GetModeName(LED_Mode mode);
void LEDTaskFunc(void *argument);

#endif
