#include "heart_rate.h"

#include <stddef.h>

#define HEART_RATE_DEFAULT_SAMPLE_RATE_HZ 500U
#define MILLIHZ_PER_HZ 1000U
#define HEART_RATE_DEFAULT_MIN_BPM 40U
#define HEART_RATE_DEFAULT_MAX_BPM 180U
#define SECONDS_PER_MINUTE 60U
#define MILLISECONDS_PER_SECOND 1000U

static void HeartRate_ClearMeasurements(HeartRateContext *context)
{
    uint8_t i;

    context->result.valid = false;
    context->result.stable = false;
    context->result.rr_ms = 0U;
    context->result.instantaneous_bpm = 0.0F;
    context->result.display_bpm = 0.0F;
    context->result.rr_count = 0U;
    context->rr_write_index = 0U;

    for (i = 0U; i < HEART_RATE_RR_WINDOW_SIZE; ++i)
    {
        context->rr_window[i] = 0U;
    }
}

static uint16_t HeartRate_MedianRR(const HeartRateContext *context)
{
    uint16_t sorted[HEART_RATE_RR_WINDOW_SIZE];
    uint8_t count = context->result.rr_count;
    uint8_t i;
    uint8_t j;

    for (i = 0U; i < count; ++i)
    {
        sorted[i] = context->rr_window[i];
    }

    for (i = 1U; i < count; ++i)
    {
        uint16_t value = sorted[i];
        j = i;
        while ((j > 0U) && (sorted[j - 1U] > value))
        {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = value;
    }

    if ((count & 1U) != 0U)
    {
        return sorted[count / 2U];
    }

    return (uint16_t)(((uint32_t)sorted[(count / 2U) - 1U] +
                       (uint32_t)sorted[count / 2U]) /
                      2U);
}

void HeartRate_Init(HeartRateContext *context, const HeartRateConfig *config)
{
    if (context == NULL)
    {
        return;
    }

    context->config.sample_rate_hz = HEART_RATE_DEFAULT_SAMPLE_RATE_HZ;
    context->sample_rate_millihz =
        HEART_RATE_DEFAULT_SAMPLE_RATE_HZ * MILLIHZ_PER_HZ;
    context->config.min_bpm = HEART_RATE_DEFAULT_MIN_BPM;
    context->config.max_bpm = HEART_RATE_DEFAULT_MAX_BPM;

    if (config != NULL)
    {
        if (config->sample_rate_hz != 0U)
        {
            context->config.sample_rate_hz = config->sample_rate_hz;
            context->sample_rate_millihz =
                config->sample_rate_hz * MILLIHZ_PER_HZ;
        }
        if (config->min_bpm != 0U)
        {
            context->config.min_bpm = config->min_bpm;
        }
        if (config->max_bpm != 0U)
        {
            context->config.max_bpm = config->max_bpm;
        }
    }

    if (context->config.min_bpm >= context->config.max_bpm)
    {
        context->config.min_bpm = HEART_RATE_DEFAULT_MIN_BPM;
        context->config.max_bpm = HEART_RATE_DEFAULT_MAX_BPM;
    }

    context->last_r_sample = 0U;
    context->has_last_r_peak = false;
    HeartRate_ClearMeasurements(context);
}

HeartRateEvent HeartRate_PushRPeak(HeartRateContext *context,
                                  uint32_t r_sample_index)
{
    uint32_t delta_samples;
    uint32_t min_samples;
    uint32_t max_samples;
    uint32_t rr_ms;
    uint16_t median_rr_ms;

    if ((context == NULL) || (context->sample_rate_millihz == 0U))
    {
        return HEART_RATE_EVENT_REJECTED;
    }

    if (!context->has_last_r_peak)
    {
        context->last_r_sample = r_sample_index;
        context->has_last_r_peak = true;
        return HEART_RATE_EVENT_FIRST_PEAK;
    }

    delta_samples = r_sample_index - context->last_r_sample;
    min_samples = (uint32_t)
        ((((uint64_t)context->sample_rate_millihz *
           SECONDS_PER_MINUTE) +
          ((uint64_t)context->config.max_bpm * MILLIHZ_PER_HZ) - 1U) /
         ((uint64_t)context->config.max_bpm * MILLIHZ_PER_HZ));
    max_samples = (uint32_t)
        (((uint64_t)context->sample_rate_millihz *
          SECONDS_PER_MINUTE) /
         ((uint64_t)context->config.min_bpm * MILLIHZ_PER_HZ));

    if (delta_samples < min_samples)
    {
        /* Keep the last accepted peak so a premature false QRS does not
         * corrupt the following valid RR interval. */
        return HEART_RATE_EVENT_REJECTED;
    }

    if (delta_samples > max_samples)
    {
        /* Re-anchor after a missed peak or an out-of-range pause, otherwise
         * every following peak would remain too far from the old anchor. */
        context->last_r_sample = r_sample_index;
        return HEART_RATE_EVENT_GAP;
    }

    context->last_r_sample = r_sample_index;
    rr_ms = (uint32_t)
        ((((uint64_t)delta_samples * MILLISECONDS_PER_SECOND *
           MILLIHZ_PER_HZ) +
          (context->sample_rate_millihz / 2U)) /
         context->sample_rate_millihz);

    context->rr_window[context->rr_write_index] = (uint16_t)rr_ms;
    context->rr_write_index =
        (uint8_t)((context->rr_write_index + 1U) % HEART_RATE_RR_WINDOW_SIZE);
    if (context->result.rr_count < HEART_RATE_RR_WINDOW_SIZE)
    {
        ++context->result.rr_count;
    }

    median_rr_ms = HeartRate_MedianRR(context);
    context->result.rr_ms = (uint16_t)rr_ms;
    context->result.instantaneous_bpm = 60000.0F / (float)rr_ms;
    context->result.display_bpm = 60000.0F / (float)median_rr_ms;
    context->result.valid = true;
    context->result.stable =
        (context->result.rr_count == HEART_RATE_RR_WINDOW_SIZE);

    return HEART_RATE_EVENT_UPDATED;
}

void HeartRate_SetSampleRateMilliHz(HeartRateContext *context,
                                    uint32_t sample_rate_millihz)
{
    if ((context == NULL) || (sample_rate_millihz == 0U))
    {
        return;
    }

    context->sample_rate_millihz = sample_rate_millihz;
    context->config.sample_rate_hz =
        (sample_rate_millihz + 500U) / MILLIHZ_PER_HZ;
}

void HeartRate_Invalidate(HeartRateContext *context)
{
    if (context == NULL)
    {
        return;
    }

    context->last_r_sample = 0U;
    context->has_last_r_peak = false;
    HeartRate_ClearMeasurements(context);
}

const HeartRateResult *HeartRate_GetResult(const HeartRateContext *context)
{
    return (context != NULL) ? &context->result : NULL;
}
