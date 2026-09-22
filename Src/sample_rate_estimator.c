#include "sample_rate_estimator.h"

#include <stddef.h>

#define DEFAULT_RATE_MILLIHZ 500000U
#define DEFAULT_MINIMUM_RATE_MILLIHZ 450000U
#define DEFAULT_MAXIMUM_RATE_MILLIHZ 550000U
#define DEFAULT_OBSERVATION_MS 5000U
#define MICRO_UNITS_PER_UNIT 1000000ULL

void SampleRateEstimator_DefaultConfig(SampleRateEstimatorConfig *config)
{
    if (config == NULL)
    {
        return;
    }

    config->nominal_rate_millihz = DEFAULT_RATE_MILLIHZ;
    config->minimum_rate_millihz = DEFAULT_MINIMUM_RATE_MILLIHZ;
    config->maximum_rate_millihz = DEFAULT_MAXIMUM_RATE_MILLIHZ;
    config->minimum_observation_ms = DEFAULT_OBSERVATION_MS;
}

void SampleRateEstimator_Init(SampleRateEstimator *estimator,
                              const SampleRateEstimatorConfig *config)
{
    SampleRateEstimatorConfig defaults;

    if (estimator == NULL)
    {
        return;
    }

    SampleRateEstimator_DefaultConfig(&defaults);
    estimator->config = (config != NULL) ? *config : defaults;
    if ((estimator->config.nominal_rate_millihz == 0U) ||
        (estimator->config.minimum_rate_millihz == 0U) ||
        (estimator->config.maximum_rate_millihz <=
         estimator->config.minimum_rate_millihz) ||
        (estimator->config.minimum_observation_ms == 0U))
    {
        estimator->config = defaults;
    }
    estimator->anchor_sample_index = 0U;
    estimator->anchor_elapsed_ms = 0U;
    estimator->estimated_rate_millihz =
        estimator->config.nominal_rate_millihz;
    estimator->initialized = false;
    estimator->estimate_valid = false;
}

bool SampleRateEstimator_Push(SampleRateEstimator *estimator,
                              uint32_t sample_index,
                              uint32_t elapsed_ms)
{
    uint32_t delta_samples;
    uint32_t delta_ms;
    uint32_t observed_rate;

    if (estimator == NULL)
    {
        return false;
    }
    if (!estimator->initialized)
    {
        estimator->anchor_sample_index = sample_index;
        estimator->anchor_elapsed_ms = elapsed_ms;
        estimator->initialized = true;
        return false;
    }

    delta_samples = sample_index - estimator->anchor_sample_index;
    delta_ms = elapsed_ms - estimator->anchor_elapsed_ms;
    if ((delta_samples == 0U) ||
        (delta_ms < estimator->config.minimum_observation_ms))
    {
        return false;
    }

    observed_rate = (uint32_t)
        ((((uint64_t)delta_samples * MICRO_UNITS_PER_UNIT) +
          (delta_ms / 2U)) /
         delta_ms);
    if ((observed_rate < estimator->config.minimum_rate_millihz) ||
        (observed_rate > estimator->config.maximum_rate_millihz))
    {
        return false;
    }

    estimator->estimated_rate_millihz = observed_rate;
    estimator->estimate_valid = true;
    return true;
}

uint32_t SampleRateEstimator_GetMilliHz(
    const SampleRateEstimator *estimator)
{
    if (estimator == NULL)
    {
        return DEFAULT_RATE_MILLIHZ;
    }
    return estimator->estimated_rate_millihz;
}
