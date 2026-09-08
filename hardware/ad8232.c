#include "ad8232.h"

#include "adc.h"
#include "main.h"

#include <stddef.h>

#define AD8232_SAMPLE_BUFFER_SIZE 256U
#define AD8232_SAMPLE_BUFFER_MASK (AD8232_SAMPLE_BUFFER_SIZE - 1U)
#define AD8232_UART_TIMEOUT_MS    5U

static volatile uint16_t sample_buffer[AD8232_SAMPLE_BUFFER_SIZE];
static volatile uint16_t sample_head;
static volatile uint16_t sample_tail;
static volatile uint16_t latest_sample;
static volatile uint32_t dropped_sample_count;
static volatile uint8_t sampling_enabled;

HAL_StatusTypeDef AD8232_Init(void)
{
    HAL_StatusTypeDef status;

    sampling_enabled = 0U;
    sample_head = 0U;
    sample_tail = 0U;
    latest_sample = 0U;
    dropped_sample_count = 0U;

    HAL_GPIO_WritePin(AD8232_SDN_GPIO_Port, AD8232_SDN_Pin, GPIO_PIN_SET);

    status = HAL_ADCEx_Calibration_Start(&hadc2);
    if (status != HAL_OK)
    {
        return status;
    }

    status = HAL_ADC_Start(&hadc2);
    if (status == HAL_OK)
    {
        sampling_enabled = 1U;
    }

    return status;
}

void AD8232_SampleTick1kHz(void)
{
    uint16_t sample;
    uint16_t next_head;

    if (sampling_enabled == 0U)
    {
        return;
    }

    if (__HAL_ADC_GET_FLAG(&hadc2, ADC_FLAG_EOC) == RESET)
    {
        return;
    }

    if (HAL_ADC_PollForConversion(&hadc2, 0U) != HAL_OK)
    {
        return;
    }

    sample = (uint16_t)HAL_ADC_GetValue(&hadc2);
    latest_sample = sample;

    next_head = (uint16_t)((sample_head + 1U) & AD8232_SAMPLE_BUFFER_MASK);
    if (next_head != sample_tail)
    {
        sample_buffer[sample_head] = sample;
        sample_head = next_head;
    }
    else
    {
        ++dropped_sample_count;
    }

    if (HAL_ADC_Start(&hadc2) != HAL_OK)
    {
        sampling_enabled = 0U;
    }
}

uint8_t AD8232_ReadSample(uint16_t *sample)
{
    uint16_t tail;

    if (sample == NULL)
    {
        return 0U;
    }

    tail = sample_tail;
    if (tail == sample_head)
    {
        return 0U;
    }

    *sample = sample_buffer[tail];
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
