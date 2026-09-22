#include "ad8232.h"

#include "adc.h"
#include "main.h"
#include "tim.h"

#include <stddef.h>

#define AD8232_SAMPLE_BUFFER_SIZE 512U
#define AD8232_SAMPLE_BUFFER_MASK (AD8232_SAMPLE_BUFFER_SIZE - 1U)
#define AD8232_DMA_BUFFER_SIZE    10U
#define AD8232_DMA_HALF_SIZE      (AD8232_DMA_BUFFER_SIZE / 2U)
#define AD8232_UART_TIMEOUT_MS    5U

static uint16_t adc_dma_buffer[AD8232_DMA_BUFFER_SIZE];
static volatile uint16_t sample_buffer[AD8232_SAMPLE_BUFFER_SIZE];
static volatile uint32_t sample_indices[AD8232_SAMPLE_BUFFER_SIZE];
static uint32_t acquisition_index;
static volatile uint16_t sample_head;
static volatile uint16_t sample_tail;
static volatile uint16_t latest_sample;
static volatile uint32_t dropped_sample_count;

static void AD8232_PushDmaSamples(const uint16_t *samples, uint16_t count)
{
    uint16_t index;
    uint16_t next_head;
    uint16_t sample;

    for (index = 0U; index < count; ++index)
    {
        sample = samples[index];
        latest_sample = sample;

        next_head = (uint16_t)((sample_head + 1U) &
                               AD8232_SAMPLE_BUFFER_MASK);
        if (next_head != sample_tail)
        {
            sample_buffer[sample_head] = sample;
            sample_indices[sample_head] = acquisition_index;
            sample_head = next_head;
        }
        else
        {
            ++dropped_sample_count;
        }
        ++acquisition_index;
    }
}

HAL_StatusTypeDef AD8232_Init(void)
{
    HAL_StatusTypeDef status;

    sample_head = 0U;
    sample_tail = 0U;
    latest_sample = 0U;
    dropped_sample_count = 0U;
    acquisition_index = 0U;

    HAL_GPIO_WritePin(AD8232_SDN_GPIO_Port, AD8232_SDN_Pin, GPIO_PIN_SET);

    status = HAL_ADCEx_Calibration_Start(&hadc1);
    if (status != HAL_OK)
    {
        return status;
    }

    status = HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_buffer,
                               AD8232_DMA_BUFFER_SIZE);
    if (status != HAL_OK)
    {
        return status;
    }

    __HAL_TIM_SET_COUNTER(&htim3, 0U);
    status = HAL_TIM_Base_Start(&htim3);
    if (status != HAL_OK)
    {
        (void)HAL_ADC_Stop_DMA(&hadc1);
    }

    return status;
}

HAL_StatusTypeDef AD8232_Restart(void)
{
    /* Recover from a stopped trigger, ADC overrun/error, or DMA channel fault.
       Stop every producer before resetting the shared queue in AD8232_Init. */
    (void)HAL_TIM_Base_Stop(&htim3);
    (void)HAL_ADC_Stop_DMA(&hadc1);
    return AD8232_Init();
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        AD8232_PushDmaSamples(&adc_dma_buffer[0], AD8232_DMA_HALF_SIZE);
    }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        AD8232_PushDmaSamples(&adc_dma_buffer[AD8232_DMA_HALF_SIZE],
                              AD8232_DMA_HALF_SIZE);
    }
}

uint8_t AD8232_ReadSample(uint16_t *sample)
{
    uint32_t index;
    return AD8232_ReadIndexedSample(sample, &index);
}

uint8_t AD8232_ReadIndexedSample(uint16_t *sample, uint32_t *index)
{
    uint16_t tail;

    if (sample == NULL || index == NULL)
    {
        return 0U;
    }

    tail = sample_tail;
    if (tail == sample_head)
    {
        return 0U;
    }

    *sample = sample_buffer[tail];
    *index = sample_indices[tail];
    sample_tail = (uint16_t)((tail + 1U) & AD8232_SAMPLE_BUFFER_MASK);
    return 1U;
}

uint16_t AD8232_GetLatestSample(void)
{
    return latest_sample;
}

uint8_t AD8232_AreLeadsOff(void)
{
    GPIO_PinState lead_positive;
    GPIO_PinState lead_negative;

    lead_positive = HAL_GPIO_ReadPin(AD8232_L0__GPIO_Port, AD8232_L0__Pin);
    lead_negative = HAL_GPIO_ReadPin(AD8232_L0_C7_GPIO_Port,
                                    AD8232_L0_C7_Pin);

    return ((lead_positive == GPIO_PIN_SET) ||
            (lead_negative == GPIO_PIN_SET)) ? 1U : 0U;
}

uint32_t AD8232_GetDroppedSampleCount(void)
{
    return dropped_sample_count;
}

void AD8232_DiscardPending(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    sample_tail = sample_head;
    __set_PRIMASK(primask);
}

HAL_StatusTypeDef AD8232_TransmitVofa(UART_HandleTypeDef *uart,
                                      uint16_t sample)
{
    uint8_t output[7];
    uint8_t reversed[5];
    uint8_t digit_count = 0U;
    uint8_t output_length = 0U;

    if (uart == NULL)
    {
        return HAL_ERROR;
    }

    do
    {
        reversed[digit_count] = (uint8_t)('0' + (sample % 10U));
        sample = (uint16_t)(sample / 10U);
        ++digit_count;
    } while ((sample != 0U) && (digit_count < sizeof(reversed)));

    while (digit_count > 0U)
    {
        --digit_count;
        output[output_length] = reversed[digit_count];
        ++output_length;
    }

    output[output_length++] = '\r';
    output[output_length++] = '\n';

    return HAL_UART_Transmit(uart, output, output_length,
                             AD8232_UART_TIMEOUT_MS);
}
