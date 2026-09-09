#include "qrs_detector.h"

#include <stddef.h>

#define DEFAULT_SAMPLE_RATE_MILLIHZ 500000U
#define DEFAULT_LEARNING_MS 2000U
#define DEFAULT_REFRACTORY_MS 250U
#define DEFAULT_THRESHOLD_WEIGHT 0.68F
#define DEFAULT_ENVELOPE_SHIFT 3U
#define MICRO_UNITS_PER_UNIT 1000000ULL

static uint32_t MillisecondsToSamples(uint32_t rate_millihz,
                                      uint16_t milliseconds)
{
    return (uint32_t)
        ((((uint64_t)rate_millihz * milliseconds) +
          MICRO_UNITS_PER_UNIT - 1ULL) /
         MICRO_UNITS_PER_UNIT);
}

void QRSDetector_DefaultConfig(QRSDetectorConfig *config)
{
    if (config == NULL)
    {
        return;
    }
    config->sample_rate_millihz = DEFAULT_SAMPLE_RATE_MILLIHZ;
    config->learning_ms = DEFAULT_LEARNING_MS;
    config->refractory_ms = DEFAULT_REFRACTORY_MS;
    config->threshold_weight = DEFAULT_THRESHOLD_WEIGHT;
    config->envelope_shift = DEFAULT_ENVELOPE_SHIFT;
}

void QRSDetector_Init(QRSDetectorContext *context,
                      const QRSDetectorConfig *config)
{
    QRSDetectorConfig defaults;

    if (context == NULL)
    {
        return;
    }
    QRSDetector_DefaultConfig(&defaults);
    context->config = (config != NULL) ? *config : defaults;
    if ((context->config.sample_rate_millihz == 0U) ||
        (context->config.learning_ms == 0U) ||
        (context->config.refractory_ms == 0U) ||
        (context->config.threshold_weight <= 0.0F) ||
        (context->config.threshold_weight >= 1.0F) ||
        (context->config.envelope_shift == 0U) ||
        (context->config.envelope_shift > 15U))
    {
        context->config = defaults;
    }

    context->envelope = 0.0F;
    context->previous_envelope = 0.0F;
    context->previous_previous_envelope = 0.0F;
    context->learning_peak_max = 0.0F;
    context->signal_level = 0.0F;
    context->noise_level = 0.0F;
    context->learning_peak_count = 0U;
    context->processed_samples = 0U;
    context->previous_sample_index = 0U;
    context->last_peak_sample = 0U;
    context->has_last_peak = false;
    context->learning_complete = false;
}

void QRSDetector_SetSampleRateMilliHz(QRSDetectorContext *context,
                                      uint32_t sample_rate_millihz)
{
    if ((context == NULL) || (sample_rate_millihz == 0U))
    {
        return;
    }
    context->config.sample_rate_millihz = sample_rate_millihz;
}

bool QRSDetector_Push(QRSDetectorContext *context,
                      uint32_t sample_index,
                      float qrs_sample,
                      uint32_t *peak_sample)
{
    uint32_t learning_samples;
    uint32_t refractory_samples;
    bool local_maximum;
    float energy;

    if ((context == NULL) || (peak_sample == NULL))
    {
        return false;
    }

    energy = qrs_sample * qrs_sample;
    context->envelope +=
        (energy - context->envelope) /
        (float)(1UL << context->config.envelope_shift);
    local_maximum =
        (context->previous_envelope >=
         context->previous_previous_envelope) &&
        (context->previous_envelope > context->envelope);
    learning_samples = MillisecondsToSamples(
        context->config.sample_rate_millihz,
        context->config.learning_ms);

    if (!context->learning_complete)
    {
        if (local_maximum)
        {
            /* The mean of all local maxima is dominated by tiny noise peaks
               on low-amplitude ECG. Seed the signal level from the strongest
               learning peak; 2 s covers at least one beat at 40 BPM. */
            if (context->previous_envelope > context->learning_peak_max)
                context->learning_peak_max = context->previous_envelope;
            ++context->learning_peak_count;
        }
        if (context->processed_samples >= learning_samples)
        {
            context->signal_level =
                (context->learning_peak_count > 0U)
                    ? context->learning_peak_max
                    : context->previous_envelope;
            if (context->signal_level < 1.0F)
            {
                context->signal_level = 1.0F;
            }
            context->noise_level = 0.03F * context->signal_level;
            context->learning_complete = true;
        }
    }
    else if (local_maximum)
    {
        float threshold = context->noise_level +
                          context->config.threshold_weight *
                              (context->signal_level -
                               context->noise_level);
        uint32_t candidate_sample = context->previous_sample_index;
        uint32_t gap = candidate_sample - context->last_peak_sample;

        refractory_samples = MillisecondsToSamples(
            context->config.sample_rate_millihz,
            context->config.refractory_ms);
        if ((context->previous_envelope >= threshold) &&
            (!context->has_last_peak || (gap >= refractory_samples)))
        {
            float limited_peak = context->previous_envelope;
            float maximum_update = 2.0F * context->signal_level;
            if (limited_peak > maximum_update)
            {
                limited_peak = maximum_update;
            }
            context->signal_level +=
                0.125F * (limited_peak - context->signal_level);
            context->last_peak_sample = candidate_sample;
            context->has_last_peak = true;
            *peak_sample = candidate_sample;

            context->previous_previous_envelope =
                context->previous_envelope;
            context->previous_envelope = context->envelope;
            context->previous_sample_index = sample_index;
            ++context->processed_samples;
            return true;
        }

        if (context->previous_envelope < threshold)
        {
            context->noise_level +=
                0.125F * (context->previous_envelope -
                          context->noise_level);
        }
    }

    context->previous_previous_envelope = context->previous_envelope;
    context->previous_envelope = context->envelope;
    context->previous_sample_index = sample_index;
    ++context->processed_samples;
    return false;
}
