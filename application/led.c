#include "led.h"

#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "main.h"
#include "task.h"
#include "tim.h"

#define KEY_EVENT_MODE       (1UL << 0)
#define KEY_EVENT_BRIGHTNESS (1UL << 1)
#define KEY_EVENT_KEY3       (1UL << 2)
#define KEY_EVENT_KEY4       (1UL << 3)

#define KEY_DEBOUNCE_MS 150U
#define BEEP_DURATION_MS 80U
#define BLINK_PERIOD_MS  500U
#define BREATHE_STEP_MS  20U

/* Start visibly at full brightness so the user LED is easy to verify. */
static volatile LED_Mode s_mode = LED_MODE_ON;
static volatile uint8_t s_brightness_level = 4U;
static volatile uint32_t s_key_events;

static const uint8_t s_brightness_percent[4] = {25U, 50U, 75U, 100U};

LED_Mode LED_GetMode(void)
{
    return s_mode;
}

uint8_t LED_GetBrightnessLevel(void)
{
    return s_brightness_level;
}

uint8_t LED_GetBrightnessPercent(void)
{
    return s_brightness_percent[s_brightness_level - 1U];
}

const char *LED_GetModeName(LED_Mode mode)
{
    static const char *const names[LED_MODE_COUNT] = {
        "OFF", "ON", "BLINK", "BREATHE"};

    return (mode < LED_MODE_COUNT) ? names[mode] : "OFF";
}

static void LED_SetDuty(uint8_t percent)
{
    uint32_t period = __HAL_TIM_GET_AUTORELOAD(&htim1) + 1U;
    uint32_t compare;

    if (percent > 100U)
    {
        percent = 100U;
    }

    compare = (period * percent) / 100U;
    if (compare > __HAL_TIM_GET_AUTORELOAD(&htim1))
    {
        compare = __HAL_TIM_GET_AUTORELOAD(&htim1);
    }
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, compare);
}

static void Buzzer_Set(uint8_t enabled)
{
    uint32_t compare = enabled
                           ? ((__HAL_TIM_GET_AUTORELOAD(&htim1) + 1U) / 2U)
                           : 0U;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, compare);
}

static uint32_t LED_TakeKeyEvents(void)
{
    uint32_t events;

    taskENTER_CRITICAL();
    events = s_key_events;
    s_key_events = 0U;
    taskEXIT_CRITICAL();
    return events;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    static uint32_t last_key1_tick;
    static uint32_t last_key2_tick;
    static uint32_t last_key3_tick;
    static uint32_t last_key4_tick;
    uint32_t now = HAL_GetTick();
    uint32_t *last_tick = NULL;
    uint32_t event = 0U;

    if (GPIO_Pin == KEY1_Pin)
    {
        last_tick = &last_key1_tick;
        event = KEY_EVENT_MODE;
    }
    else if (GPIO_Pin == KEY2_Pin)
    {
        last_tick = &last_key2_tick;
        event = KEY_EVENT_BRIGHTNESS;
    }
    else if (GPIO_Pin == KEY3_Pin)
    {
        last_tick = &last_key3_tick;
        event = KEY_EVENT_KEY3;
    }
    else if (GPIO_Pin == KEY4_Pin)
    {
        last_tick = &last_key4_tick;
        event = KEY_EVENT_KEY4;
    }

    if ((last_tick != NULL) &&
        ((*last_tick == 0U) || ((now - *last_tick) >= KEY_DEBOUNCE_MS)))
    {
        *last_tick = now;
        s_key_events |= event;
    }
}

void LEDTaskFunc(void *argument)
{
    LED_Mode previous_mode = LED_MODE_COUNT;
    uint32_t mode_start_tick = 0U;
    uint32_t last_breathe_tick = 0U;
    uint32_t beep_end_tick = 0U;
    uint8_t breathe_value = 0U;
    uint8_t breathe_up = 1U;
    uint8_t buzzer_active = 0U;
    uint32_t events;
    uint32_t now;
    uint8_t target_brightness;

    (void)argument;

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0U);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, 0U);
    if (HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4) != HAL_OK)
    {
        Error_Handler();
    }

    for (;;)
    {
        now = HAL_GetTick();
        events = LED_TakeKeyEvents();

        if (events != 0U)
        {
            Buzzer_Set(1U);
            buzzer_active = 1U;
            beep_end_tick = now + BEEP_DURATION_MS;
        }
        if ((events & KEY_EVENT_MODE) != 0U)
        {
            s_mode = (LED_Mode)(((uint32_t)s_mode + 1U) % LED_MODE_COUNT);
        }
        if ((events & KEY_EVENT_BRIGHTNESS) != 0U)
        {
            s_brightness_level = (uint8_t)((s_brightness_level % 4U) + 1U);
        }

        if ((buzzer_active != 0U) && ((int32_t)(now - beep_end_tick) >= 0))
        {
            Buzzer_Set(0U);
            buzzer_active = 0U;
        }

        if (s_mode != previous_mode)
        {
            previous_mode = s_mode;
            mode_start_tick = now;
            last_breathe_tick = now;
            breathe_value = 0U;
            breathe_up = 1U;
        }

        target_brightness = LED_GetBrightnessPercent();
        switch (s_mode)
        {
            case LED_MODE_ON:
                LED_SetDuty(target_brightness);
                break;

            case LED_MODE_BLINK:
                LED_SetDuty((((now - mode_start_tick) / BLINK_PERIOD_MS) & 1U)
                                ? 0U
                                : target_brightness);
                break;

            case LED_MODE_BREATHE:
                if ((now - last_breathe_tick) >= BREATHE_STEP_MS)
                {
                    last_breathe_tick = now;
                    if (breathe_up != 0U)
                    {
                        if (breathe_value < target_brightness)
                        {
                            ++breathe_value;
                        }
                        else
                        {
                            breathe_up = 0U;
                        }
                    }
                    else if (breathe_value > 0U)
                    {
                        --breathe_value;
                    }
                    else
                    {
                        breathe_up = 1U;
                    }
                }
                LED_SetDuty(breathe_value);
                break;

            case LED_MODE_OFF:
            default:
                LED_SetDuty(0U);
                break;
        }

        osDelay(10U);
    }
}
