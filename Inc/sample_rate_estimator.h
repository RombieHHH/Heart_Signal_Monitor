#ifndef SAMPLE_RATE_ESTIMATOR_H
#define SAMPLE_RATE_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t nominal_rate_millihz;
    uint32_t minimum_rate_millihz;
    uint32_t maximum_rate_millihz;
    uint32_t minimum_observation_ms;
} SampleRateEstimatorConfig;

typedef struct
{
    SampleRateEstimatorConfig config;
    uint32_t anchor_sample_index;
    uint32_t anchor_elapsed_ms;
    uint32_t estimated_rate_millihz;
    bool initialized;
    bool estimate_valid;
} SampleRateEstimator;

void SampleRateEstimator_DefaultConfig(SampleRateEstimatorConfig *config);
void SampleRateEstimator_Init(SampleRateEstimator *estimator,
                              const SampleRateEstimatorConfig *config);

/* Push a cumulative sample index and an independent monotonic time in ms.
 * Memory use is constant. Repeated/coarse timestamps are acceptable because
 * the estimate is made over a long observation interval. */
bool SampleRateEstimator_Push(SampleRateEstimator *estimator,
                              uint32_t sample_index,
                              uint32_t elapsed_ms);

uint32_t SampleRateEstimator_GetMilliHz(
    const SampleRateEstimator *estimator);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_RATE_ESTIMATOR_H */
