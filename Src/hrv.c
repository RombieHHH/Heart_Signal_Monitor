#include "hrv.h"

#include <stddef.h>

#define HRV_DEFAULT_ALARM_CV 0.10F
#define HRV_DEFAULT_ALARM_D 0.12F
#define HRV_DEFAULT_CLEAR_CV 0.08F
#define HRV_DEFAULT_CLEAR_D 0.10F
#define HRV_DEFAULT_ALARM_UPDATES 3U
#define HRV_DEFAULT_CLEAR_UPDATES 5U

static float HRV_Sqrt(float value)
{
    float estimate;
    uint8_t i;

    if (value <= 0.0F)
    {
        return 0.0F;
    }

    estimate = (value >= 1.0F) ? value : 1.0F;
    /* Sixteen Newton iterations cover the complete 333...1500 ms RR range
     * without adding a libm dependency to the small MCU target. */
    for (i = 0U; i < 16U; ++i)
    {
        estimate = 0.5F * (estimate + (value / estimate));
    }
    return estimate;
}

static void HRV_NotifyAlarm(HRVContext *context, bool active)
{
    if (context->config.alarm_callback != NULL)
    {
        context->config.alarm_callback(active,
                                       context->config.callback_user_data);
    }
}

static void HRV_ClearWindow(HRVContext *context)
{
    uint8_t i;

    context->result.alarm_event = HRV_ALARM_EVENT_NONE;
    context->result.metrics_valid = false;
    context->result.rr_count = 0U;
    context->result.mean_rr_ms = 0.0F;
    context->result.sdnn_ms = 0.0F;
    context->result.rmssd_ms = 0.0F;
    context->result.cv = 0.0F;
    context->result.d = 0.0F;
    context->rr_write_index = 0U;
    context->abnormal_updates = 0U;
    context->normal_updates = 0U;

    for (i = 0U; i < HRV_RR_WINDOW_SIZE; ++i)
    {
        context->rr_window[i] = 0U;
    }
}

static void HRV_CalculateMetrics(HRVContext *context)
{
    float sum = 0.0F;
    float squared_deviation_sum = 0.0F;
    float squared_difference_sum = 0.0F;
    float mean;
    uint8_t oldest;
    uint8_t i;

    for (i = 0U; i < HRV_RR_WINDOW_SIZE; ++i)
    {
        sum += (float)context->rr_window[i];
    }
    mean = sum / (float)HRV_RR_WINDOW_SIZE;

    for (i = 0U; i < HRV_RR_WINDOW_SIZE; ++i)
    {
        float deviation = (float)context->rr_window[i] - mean;
        squared_deviation_sum += deviation * deviation;
    }

    /* rr_write_index points to the oldest interval once the ring is full. */
    oldest = context->rr_write_index;
    for (i = 1U; i < HRV_RR_WINDOW_SIZE; ++i)
    {
        uint8_t previous =
            (uint8_t)((oldest + i - 1U) % HRV_RR_WINDOW_SIZE);
        uint8_t current =
            (uint8_t)((oldest + i) % HRV_RR_WINDOW_SIZE);
        float difference = (float)context->rr_window[current] -
                           (float)context->rr_window[previous];
        squared_difference_sum += difference * difference;
    }

    context->result.mean_rr_ms = mean;
    context->result.sdnn_ms =
        HRV_Sqrt(squared_deviation_sum / (float)(HRV_RR_WINDOW_SIZE - 1U));
    context->result.rmssd_ms =
        HRV_Sqrt(squared_difference_sum / (float)(HRV_RR_WINDOW_SIZE - 1U));
    context->result.cv = context->result.sdnn_ms / mean;
    context->result.d = context->result.rmssd_ms / mean;
    context->result.metrics_valid = true;
}

static HRVAlarmEvent HRV_UpdateAlarm(HRVContext *context)
{
    bool exceeds_alarm =
        (context->result.cv > context->config.alarm_cv_threshold) ||
        (context->result.d > context->config.alarm_d_threshold);
    bool below_clear =
        (context->result.cv < context->config.clear_cv_threshold) &&
        (context->result.d < context->config.clear_d_threshold);

    context->result.alarm_event = HRV_ALARM_EVENT_NONE;

    if (!context->result.alarm_active)
    {
        context->normal_updates = 0U;
        if (exceeds_alarm)
        {
            if (context->abnormal_updates < UINT8_MAX)
            {
                ++context->abnormal_updates;
            }
            if (context->abnormal_updates >=
                context->config.alarm_confirm_updates)
            {
                context->result.alarm_active = true;
                context->result.status = HRV_STATUS_SUSPECTED_ARRHYTHMIA;
                context->result.alarm_event = HRV_ALARM_EVENT_TRIGGERED;
                context->abnormal_updates = 0U;
                HRV_NotifyAlarm(context, true);
            }
        }
        else
        {
            context->abnormal_updates = 0U;
        }
    }
    else
    {
        context->abnormal_updates = 0U;
        if (below_clear)
        {
            if (context->normal_updates < UINT8_MAX)
            {
                ++context->normal_updates;
            }
            if (context->normal_updates >= context->config.clear_confirm_updates)
            {
                context->result.alarm_active = false;
                context->result.status = HRV_STATUS_NORMAL;
                context->result.alarm_event = HRV_ALARM_EVENT_CLEARED;
                context->normal_updates = 0U;
                HRV_NotifyAlarm(context, false);
            }
        }
        else
        {
            context->normal_updates = 0U;
        }
    }

    return context->result.alarm_event;
}

void HRV_Init(HRVContext *context, const HRVConfig *config)
{
    if (context == NULL)
    {
        return;
    }

    context->config.alarm_cv_threshold = HRV_DEFAULT_ALARM_CV;
    context->config.alarm_d_threshold = HRV_DEFAULT_ALARM_D;
    context->config.clear_cv_threshold = HRV_DEFAULT_CLEAR_CV;
    context->config.clear_d_threshold = HRV_DEFAULT_CLEAR_D;
    context->config.alarm_confirm_updates = HRV_DEFAULT_ALARM_UPDATES;
    context->config.clear_confirm_updates = HRV_DEFAULT_CLEAR_UPDATES;
    context->config.alarm_callback = NULL;
    context->config.callback_user_data = NULL;

    if (config != NULL)
    {
        context->config = *config;
        if (context->config.alarm_cv_threshold <= 0.0F)
        {
            context->config.alarm_cv_threshold = HRV_DEFAULT_ALARM_CV;
        }
        if (context->config.alarm_d_threshold <= 0.0F)
        {
            context->config.alarm_d_threshold = HRV_DEFAULT_ALARM_D;
        }
        if (context->config.clear_cv_threshold <= 0.0F)
        {
            context->config.clear_cv_threshold = HRV_DEFAULT_CLEAR_CV;
        }
        if (context->config.clear_d_threshold <= 0.0F)
        {
            context->config.clear_d_threshold = HRV_DEFAULT_CLEAR_D;
        }
        if (context->config.alarm_confirm_updates == 0U)
        {
            context->config.alarm_confirm_updates =
                HRV_DEFAULT_ALARM_UPDATES;
        }
        if (context->config.clear_confirm_updates == 0U)
        {
            context->config.clear_confirm_updates =
                HRV_DEFAULT_CLEAR_UPDATES;
        }
    }

    context->result.alarm_active = false;
    context->result.status = HRV_STATUS_COLLECTING;
    HRV_ClearWindow(context);
}

HRVAlarmEvent HRV_PushRR(HRVContext *context, uint16_t rr_ms)
{
    if ((context == NULL) || (rr_ms == 0U) ||
        (context->result.status == HRV_STATUS_SIGNAL_INVALID))
    {
        return HRV_ALARM_EVENT_NONE;
    }

    context->result.alarm_event = HRV_ALARM_EVENT_NONE;
    context->rr_window[context->rr_write_index] = rr_ms;
    context->rr_write_index =
        (uint8_t)((context->rr_write_index + 1U) % HRV_RR_WINDOW_SIZE);
    if (context->result.rr_count < HRV_RR_WINDOW_SIZE)
    {
        ++context->result.rr_count;
    }

    if (context->result.rr_count < HRV_RR_WINDOW_SIZE)
    {
        context->result.status = HRV_STATUS_COLLECTING;
        return HRV_ALARM_EVENT_NONE;
    }

    HRV_CalculateMetrics(context);
    context->result.status = context->result.alarm_active
                                 ? HRV_STATUS_SUSPECTED_ARRHYTHMIA
                                 : HRV_STATUS_NORMAL;
    return HRV_UpdateAlarm(context);
}

void HRV_Invalidate(HRVContext *context)
{
    bool alarm_was_active;

    if (context == NULL)
    {
        return;
    }

    alarm_was_active = context->result.alarm_active;
    context->result.alarm_active = false;
    HRV_ClearWindow(context);
    context->result.status = HRV_STATUS_SIGNAL_INVALID;

    if (alarm_was_active)
    {
        context->result.alarm_event = HRV_ALARM_EVENT_CLEARED;
        HRV_NotifyAlarm(context, false);
    }
}

void HRV_SetSignalValid(HRVContext *context)
{
    if (context == NULL)
    {
        return;
    }

    HRV_ClearWindow(context);
    context->result.alarm_active = false;
    context->result.status = HRV_STATUS_COLLECTING;
}

const HRVResult *HRV_GetResult(const HRVContext *context)
{
    return (context != NULL) ? &context->result : NULL;
}
