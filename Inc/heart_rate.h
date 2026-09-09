#ifndef HEART_RATE_H
#define HEART_RATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HEART_RATE_RR_WINDOW_SIZE 5U

typedef enum
{
    HEART_RATE_EVENT_NONE = 0,
    HEART_RATE_EVENT_FIRST_PEAK,
    HEART_RATE_EVENT_UPDATED,
    HEART_RATE_EVENT_REJECTED
} HeartRateEvent;

typedef struct
{
    uint32_t sample_rate_hz;
    uint16_t min_bpm;
    uint16_t max_bpm;
} HeartRateConfig;

typedef struct
{
    bool valid;
    bool stable;
    uint16_t rr_ms;
    float instantaneous_bpm;
    float display_bpm;
    uint8_t rr_count;
} HeartRateResult;

typedef struct
{
    HeartRateConfig config;
    /* Internal calibrated rate with 0.001 Hz resolution. */
    uint32_t sample_rate_millihz;
    HeartRateResult result;
    uint32_t last_r_sample;
    uint16_t rr_window[HEART_RATE_RR_WINDOW_SIZE];
    uint8_t rr_write_index;
    bool has_last_r_peak;
} HeartRateContext;

/* Initializes the module. Invalid zero-valued fields are replaced by
 * 500 Hz and the 40...180 BPM validation range from the design report. */
void HeartRate_Init(HeartRateContext *context, const HeartRateConfig *config);

/* Supplies an R-peak position using the continuously increasing ADC sample
 * sequence number. The subtraction is safe across a uint32_t wraparound.
 * On HEART_RATE_EVENT_UPDATED, rr_ms can be passed directly to HRV_PushRR(). */
HeartRateEvent HeartRate_PushRPeak(HeartRateContext *context,
                                  uint32_t r_sample_index);

/* Updates the sample-rate calibration without clearing collected RR data.
 * The estimator module can call this occasionally; 500000 means 500 Hz. */
void HeartRate_SetSampleRateMilliHz(HeartRateContext *context,
                                    uint32_t sample_rate_millihz);

/* Clears the R-peak anchor and all RR/BPM values. Call this for lead-off,
 * clipping, a missing ADC block, or unreliable QRS detection. */
void HeartRate_Invalidate(HeartRateContext *context);

const HeartRateResult *HeartRate_GetResult(const HeartRateContext *context);

#ifdef __cplusplus
}
#endif

#endif /* HEART_RATE_H */
