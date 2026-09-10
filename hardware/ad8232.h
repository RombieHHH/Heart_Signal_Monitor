#ifndef APPLICATION_AD8232_H
#define APPLICATION_AD8232_H

#include "stm32f1xx_hal.h"

#include <stdint.h>

#define AD8232_SAMPLE_RATE_HZ 500U

HAL_StatusTypeDef AD8232_Init(void);
HAL_StatusTypeDef AD8232_Restart(void);
uint8_t AD8232_ReadSample(uint16_t *sample);
uint8_t AD8232_ReadIndexedSample(uint16_t *sample, uint32_t *index);
uint16_t AD8232_GetLatestSample(void);
uint8_t AD8232_AreLeadsOff(void);
uint32_t AD8232_GetDroppedSampleCount(void);
void AD8232_DiscardPending(void);
HAL_StatusTypeDef AD8232_TransmitVofa(UART_HandleTypeDef *uart,
                                      uint16_t sample);

#endif
