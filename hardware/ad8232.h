#ifndef APPLICATION_AD8232_H
#define APPLICATION_AD8232_H

#include "stm32f1xx_hal.h"

#include <stdint.h>

#define AD8232_SAMPLE_RATE_HZ 1000U

HAL_StatusTypeDef AD8232_Init(void);
void AD8232_SampleTick1kHz(void);
uint8_t AD8232_ReadSample(uint16_t *sample);
uint16_t AD8232_GetLatestSample(void);
uint8_t AD8232_AreLeadsOff(void);
uint32_t AD8232_GetDroppedSampleCount(void);
HAL_StatusTypeDef AD8232_TransmitVofa(UART_HandleTypeDef *uart,
                                      uint16_t sample);

#endif
