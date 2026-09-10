#include "ecg_app.h"
#include "ecg_monitor.h"
#include "ecg_plot.h"
#include "ad8232.h"
#include "lcd.h"
#include "main.h"
#include "tim.h"
#include "usart.h"
#include "ecg_bt_protocol.h"
#include <stdio.h>
#include <string.h>

/* Optional raw serial stream, compatible with the existing collection tools.
   Disabled for acceptance display to keep UART stalls out of acquisition. */
#ifndef ECG_RAW_SERIAL
#define ECG_RAW_SERIAL 0
#endif
#ifndef ECG_UART2_DEBUG
#define ECG_UART2_DEBUG 1
#endif
_Static_assert(AD8232_SAMPLE_RATE_HZ == ECG_MONITOR_RATE, "Filter rate mismatch");
static ECGMonitor monitor;
static ECGBTProtocol protocol;
/* Separate storage remains untouched until the UART completes transmission. */
static uint8_t uart3_frame[ECG_BT_FRAME_SIZE];
static uint16_t uart3_offset = ECG_BT_FRAME_SIZE;
static uint32_t uart3_next_tick;

#define UART3_CHUNK_BYTES 1U
#define UART3_CHUNK_INTERVAL_MS 2U

/* Pace the compact frame instead of sending one large UART burst.
   A 128-byte frame takes about 256 ms and is produced every 400 ms. */
static void PollUart3(void)
{
    uint32_t now = HAL_GetTick();
    if (uart3_offset == ECG_BT_FRAME_SIZE || huart3.gState != HAL_UART_STATE_READY ||
        (int32_t)(now - uart3_next_tick) < 0) return;
    uint16_t count = ECG_BT_FRAME_SIZE - uart3_offset;
    if (count > UART3_CHUNK_BYTES) count = UART3_CHUNK_BYTES;
    if (HAL_UART_Transmit_IT(&huart3, uart3_frame + uart3_offset, count) == HAL_OK) {
        uart3_offset += count;
        uart3_next_tick = now + UART3_CHUNK_INTERVAL_MS;
    }
}
static ECGPlot plot;
static uint32_t dropped, last_data_tick, last_restart_tick, info_tick;
static bool stalled, gap_notice;
static volatile bool scale_requested;

#if ECG_UART2_DEBUG && !ECG_RAW_SERIAL
static uint8_t uart2_debug[128];

static int32_t Rounded(float value)
{
    return (int32_t)(value >= 0.0F ? value + 0.5F : value - 0.5F);
}

static void DebugUart2(uint32_t sample_index, uint16_t raw)
{
    if ((sample_index % 5U) != 0U ||
        huart2.gState != HAL_UART_STATE_READY) return;
    const ECGPreprocessResult *p = &monitor.preprocess.result;
    int length = snprintf((char *)uart2_debug, sizeof(uart2_debug),
        "ECGDBG,%lu,%u,%ld,%ld,%lu,%u,%u,%u,%u,%u,%ld,%lu\r\n",
        (unsigned long)sample_index, (unsigned)raw,
        (long)Rounded(p->display_sample), (long)Rounded(p->qrs_sample),
        (unsigned long)p->quality_flags,
        monitor.leads_off ? 1U : 0U,
        monitor.result.signal_valid ? 1U : 0U,
        monitor.detector.learning_complete ? 1U : 0U,
        monitor.heart_rate.result.valid ? 1U : 0U,
        (unsigned)monitor.heart_rate.result.rr_count,
        (long)Rounded(monitor.detector.noise_level +
            monitor.detector.config.threshold_weight *
            (monitor.detector.signal_level - monitor.detector.noise_level)),
        (unsigned long)AD8232_GetDroppedSampleCount());
    if (length > 0 && length < (int)sizeof(uart2_debug))
        (void)HAL_UART_Transmit_IT(&huart2, uart2_debug, (uint16_t)length);
}
#endif

static void RenderColumn(uint16_t x)
{
    uint16_t pixels[ECG_PLOT_HEIGHT];
    ECGPlot_RenderColumn(&plot, x, pixels);
    LCD_DrawColorColumn(x, ECG_PLOT_TOP, pixels, ECG_PLOT_HEIGHT);
}

static void Plot(const ECGMonitorResult *r)
{
    if (!r->waveform_valid) { plot.samples = 0U; plot.previous_valid = false; return; }
    bool had_marker = plot.marker[plot.x] != UINT16_MAX;
    int x = ECGPlot_Push(&plot, r->waveform, r->point);
    if (x < 0) return;
    RenderColumn((uint16_t)x);
    /* Move the detection line and refresh columns as they cross the three
       old-trace brightness thresholds. */
    static const uint16_t fade_ages[] = {
        ECG_PLOT_FADE_START_AGE,
        ECG_PLOT_FADE_MID_AGE,
        ECG_PLOT_FADE_DARK_AGE
    };
    for (unsigned i = 0U; i < sizeof(fade_ages) / sizeof(fade_ages[0]); ++i) {
        uint16_t fade_x = (uint16_t)((plot.x + ECG_PLOT_WIDTH - 1U -
                                      fade_ages[i]) % ECG_PLOT_WIDTH);
        if (fade_x != (uint16_t)x) RenderColumn(fade_x);
    }
    RenderColumn(plot.x);
    /* Refresh neighbors immediately when an old/new annotation changes.
       The compositor retains the full square and glyph on future refreshes. */
    if (had_marker || plot.marker[x] != UINT16_MAX)
        for (int neighbor = x - 4; neighbor <= x + 4; ++neighbor)
            if (neighbor >= 0 && neighbor < (int)ECG_PLOT_WIDTH && neighbor != x)
                RenderColumn((uint16_t)neighbor);
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    static uint32_t last_key_tick;
    uint32_t now = HAL_GetTick();
    if (pin == KEY2_Pin && now - last_key_tick >= 200U) {
        last_key_tick = now;
        scale_requested = true;
    }
}

static LCDTextField bpm_field, rr_field, sdnn_field, rmssd_field;
static LCDTextField status_field, count_field, scale_field;

static void UpdateNumber(LCDTextField *field, uint16_t x, uint16_t y,
                          uint16_t value, bool valid, uint8_t digits,
                          uint8_t scale, uint16_t color)
{
    char text[6];
    text[digits] = '\0';
    for (uint8_t i = digits; i > 0U; --i) {
        text[i - 1U] = valid ? (char)('0' + value % 10U) : '-';
        value /= 10U;
    }
    LCD_UpdateTextField(field, x, y, text, digits, scale,
                        valid ? color : LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
}

static void DrawStaticInfo(void)
{
    LCD_DrawString(4U, 3U, "BPM", 2U, LCD_COLOR_WHITE);
    LCD_DrawString(96U, 6U, "RR", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(143U, 6U, "MS", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(172U, 6U, "500 HZ", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(4U, 201U, "SDNN", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(120U, 201U, "RMSSD", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(66U, 201U, "MS", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(186U, 201U, "MS", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(130U, 216U, "RR N", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(4U, 230U, "K2 SCALE", 1U, LCD_COLOR_WHITE);
    LCD_DrawString(100U, 230U, "0.5-40 HZ 2.4S", 1U, LCD_COLOR_WHITE);
}

static void DrawInfo(void)
{
    const HeartRateResult *hr = &monitor.heart_rate.result;
    const HRVResult *hv = &monitor.hrv.result;
    UpdateNumber(&bpm_field, 44U, 3U, (uint16_t)(hr->display_bpm + .5F), hr->valid, 3U, 2U, LCD_COLOR_GREEN);
    UpdateNumber(&rr_field, 114U, 6U, hr->rr_ms, hr->valid, 4U, 1U, LCD_COLOR_CYAN);
    UpdateNumber(&sdnn_field, 36U, 201U, (uint16_t)(hv->sdnn_ms + .5F), hv->metrics_valid, 4U, 1U, LCD_COLOR_CYAN);
    UpdateNumber(&rmssd_field, 156U, 201U, (uint16_t)(hv->rmssd_ms + .5F), hv->metrics_valid, 4U, 1U, LCD_COLOR_CYAN);
    const char *status = "LEARNING";
    if (stalled) status = "ADC STOP";
    else if (AD8232_AreLeadsOff()) status = "LEADS OFF";
    else if (gap_notice) status = "DATA GAP";
    else if (!monitor.result.signal_valid && !(monitor.result.quality_flags & ECG_QUALITY_WARMING_UP)) status = "SIGNAL BAD";
    else if (hv->alarm_active) status = "HRV ALARM";
    else if (hv->metrics_valid) status = "REGULAR";
    else if (hr->valid) status = "COLLECTING";
    LCD_UpdateTextField(&status_field, 4U, 216U, status, 10U, 1U,
                        hv->alarm_active ? LCD_COLOR_RED : LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    UpdateNumber(&count_field, 162U, 216U, hv->rr_count, true, 2U, 1U, LCD_COLOR_CYAN);
    const char *scales[] = {"AUTO", "1X", "2X", "4X", "8X"};
    LCD_UpdateTextField(&scale_field, 60U, 230U, scales[plot.scale_mode], 4U, 1U,
                        LCD_COLOR_CYAN, LCD_COLOR_BLACK);
}

void ECGApp_Init(void)
{
    ECGMonitor_Init(&monitor);
    ECGBTProtocol_Init(&protocol);
    ECGPlot_Init(&plot);
    LCD_Init();
    LCD_Fill(LCD_COLOR_BLACK);
    /* TIM1 CH4 / PA11: 2 kHz passive-buzzer carrier, silent at CCR4=0. */
    uint32_t timer_clock = HAL_RCC_GetPCLK2Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE2) != 0U) timer_clock *= 2U;
    __HAL_TIM_SET_PRESCALER(&htim1, timer_clock / 1000000U - 1U);
    __HAL_TIM_SET_AUTORELOAD(&htim1, 499U);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, 0U);
    htim1.Instance->EGR = TIM_EGR_UG;
    if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4) != HAL_OK) Error_Handler();
    DrawStaticInfo();
    DrawInfo();
    if (AD8232_Init() != HAL_OK) Error_Handler();
    info_tick = last_data_tick = last_restart_tick = HAL_GetTick();
}

void ECGApp_Poll(void)
{
    PollUart3();
    uint16_t sample;
    uint32_t sample_index;
    uint32_t now = HAL_GetTick();
    if (scale_requested) {
        scale_requested = false;
        ECGPlot_NextScale(&plot);
        LCD_FillRect(0U, ECG_PLOT_TOP, LCD_WIDTH, ECG_PLOT_HEIGHT, LCD_COLOR_BLACK);
        info_tick = now - 500U;
    }
    if (AD8232_GetDroppedSampleCount() != dropped) {
        /* Atomically discard the mixed pre/post-gap queue; never compress
           missing samples into a shorter RR interval. */
        AD8232_DiscardPending();
        dropped = AD8232_GetDroppedSampleCount();
        ECGMonitor_Init(&monitor);
        plot.samples = 0U;
        plot.previous_valid = false;
        gap_notice = true;
    }
    /* Bounded draining gives watchdog/status/alarm work a chance each pass. */
    for (uint32_t n = 0U; n < 25U && AD8232_ReadIndexedSample(&sample, &sample_index); ++n) {
        if (stalled) { ECGMonitor_Init(&monitor); stalled = false; }
        last_data_tick = HAL_GetTick();
        const ECGMonitorResult *result =
            ECGMonitor_Push(&monitor, sample, AD8232_AreLeadsOff() != 0U);
        Plot(result);
#if ECG_UART2_DEBUG && !ECG_RAW_SERIAL
        DebugUart2(sample_index, sample);
#endif
        if (ECGBTProtocol_Push(&protocol, sample_index, sample)) {
            bool accepted = false;
            if (uart3_offset == ECG_BT_FRAME_SIZE && huart3.gState == HAL_UART_STATE_READY) {
                memcpy(uart3_frame, protocol.data, sizeof(uart3_frame));
                uart3_offset = 0U;
                uart3_next_tick = HAL_GetTick();
                accepted = true;
            }
            ECGBTProtocol_Sent(&protocol, accepted);
        }
        PollUart3();
#if ECG_RAW_SERIAL
        (void)AD8232_TransmitVofa(&huart2, sample);
#endif
    }
    now = HAL_GetTick();
    if (now - last_data_tick > 100U) {
        if (!stalled) {
            ECGMonitor_Invalidate(&monitor);
            stalled = true;
        }
        /* DMA/timer faults otherwise leave the unit in ADC STOP forever.
           Retry at a bounded rate; the first new indexed sample clears it. */
        if (now - last_restart_tick >= 500U) {
            last_restart_tick = now;
            if (AD8232_Restart() == HAL_OK) {
                dropped = AD8232_GetDroppedSampleCount();
                last_data_tick = now;
            }
        }
    }
    if (monitor.heart_rate.result.valid) gap_notice = false;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4,
        monitor.hrv.result.alarm_active && (now % 1000U < 200U) ? 250U : 0U);
    if (now - info_tick >= 500U) { info_tick = now; DrawInfo(); }
}
