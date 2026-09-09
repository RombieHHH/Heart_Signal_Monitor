/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ad8232.h"
#include "lcd.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define LCD_PLOT_TOP            52U
#define LCD_PLOT_HEIGHT         184U
#define LCD_PLOT_INTERVAL_MS    10U
#define LCD_INFO_INTERVAL_MS    100U
#define ADC_12_BIT_MAX_VALUE    4095U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static uint16_t LCD_MapAdcSampleToY(uint16_t sample)
{
  uint32_t inverted_sample;

  if (sample > ADC_12_BIT_MAX_VALUE)
  {
    sample = ADC_12_BIT_MAX_VALUE;
  }

  inverted_sample = (uint32_t)(ADC_12_BIT_MAX_VALUE - sample);
  return (uint16_t)(LCD_PLOT_TOP +
                    (inverted_sample * (LCD_PLOT_HEIGHT - 1U)) /
                    ADC_12_BIT_MAX_VALUE);
}

static void LCD_DrawAdcWaveformColumn(uint16_t x, uint16_t minimum_sample,
                                      uint16_t maximum_sample,
                                      uint16_t previous_sample)
{
  uint16_t line_top;
  uint16_t line_bottom;
  uint16_t previous_y;

  line_top = LCD_MapAdcSampleToY(maximum_sample);
  line_bottom = LCD_MapAdcSampleToY(minimum_sample);
  previous_y = LCD_MapAdcSampleToY(previous_sample);

  if (previous_y < line_top)
  {
    line_top = previous_y;
  }
  if (previous_y > line_bottom)
  {
    line_bottom = previous_y;
  }

  LCD_FillRect(x, LCD_PLOT_TOP, 1U, LCD_PLOT_HEIGHT, LCD_COLOR_BLACK);
  LCD_FillRect(x, line_top, 1U, (uint16_t)(line_bottom - line_top + 1U),
               LCD_COLOR_GREEN);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint16_t sample = 0U;
  uint16_t plot_minimum = 0U;
  uint16_t plot_maximum = 0U;
  uint16_t plot_latest = 0U;
  uint16_t plot_previous = 0U;
  uint16_t plot_x = 0U;
  uint32_t plot_update_tick;
  uint32_t info_update_tick;
  uint8_t plot_has_samples = 0U;
  uint8_t plot_has_previous = 0U;

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI2_Init();
  MX_TIM1_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
  MX_ADC1_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */

  LCD_Init();
  LCD_Fill(LCD_COLOR_BLACK);
  LCD_DrawString(84U, 4U, "AD8232", 2U, LCD_COLOR_CYAN);
  LCD_DrawString(6U, 26U, "RAW", 1U, LCD_COLOR_WHITE);
  LCD_DrawUInt16(36U, 24U, 0U, 4U, 2U, LCD_COLOR_GREEN);
  LCD_DrawString(140U, 26U, "LEADS", 1U, LCD_COLOR_WHITE);

  if (AD8232_Init() != HAL_OK)
  {
    Error_Handler();
  }
  plot_update_tick = HAL_GetTick();
  info_update_tick = plot_update_tick;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    while (AD8232_ReadSample(&sample) != 0U)
    {
      if (plot_has_samples == 0U)
      {
        plot_minimum = sample;
        plot_maximum = sample;
        plot_has_samples = 1U;
      }
      else
      {
        if (sample < plot_minimum)
        {
          plot_minimum = sample;
        }
        if (sample > plot_maximum)
        {
          plot_maximum = sample;
        }
      }
      plot_latest = sample;

      (void)AD8232_TransmitVofa(&huart2, sample);
    }

    if ((HAL_GetTick() - plot_update_tick) >= LCD_PLOT_INTERVAL_MS)
    {
      plot_update_tick = HAL_GetTick();

      if (plot_has_samples != 0U)
      {
        if (plot_has_previous == 0U)
        {
          plot_previous = plot_latest;
          plot_has_previous = 1U;
        }

        LCD_DrawAdcWaveformColumn(plot_x, plot_minimum, plot_maximum,
                                  plot_previous);
        plot_previous = plot_latest;
        plot_has_samples = 0U;

        ++plot_x;
        if (plot_x >= LCD_WIDTH)
        {
          plot_x = 0U;
        }
      }
    }

    if ((HAL_GetTick() - info_update_tick) >= LCD_INFO_INTERVAL_MS)
    {
      info_update_tick = HAL_GetTick();
      sample = AD8232_GetLatestSample();

      LCD_FillRect(36U, 22U, 56U, 18U, LCD_COLOR_BLACK);
      LCD_DrawUInt16(36U, 24U, sample, 4U, 2U, LCD_COLOR_GREEN);

      LCD_FillRect(180U, 24U, 54U, 12U, LCD_COLOR_BLACK);
      if (AD8232_AreLeadsOff() != 0U)
      {
        LCD_DrawString(180U, 26U, "OFF", 1U, LCD_COLOR_RED);
      }
      else
      {
        LCD_DrawString(180U, 26U, "OK", 1U, LCD_COLOR_GREEN);
      }
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV8;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
