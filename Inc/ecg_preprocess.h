#ifndef ECG_PREPROCESS_H
#define ECG_PREPROCESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECG_PREPROCESS_QUALITY_WINDOW_MAX 250U

typedef enum
{
    ECG_QUALITY_OK = 0U,
    ECG_QUALITY_WARMING_UP = (1U << 0),
    ECG_QUALITY_CLIPPED = (1U << 1),
    ECG_QUALITY_FLATLINE = (1U << 2),
    ECG_QUALITY_STEP_ARTIFACT = (1U << 3),
    ECG_QUALITY_OUT_OF_RANGE = (1U << 4)
} ECGQualityFlag;

typedef struct
{
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
} ECGBiquadCoefficients;

typedef struct
{
    ECGBiquadCoefficients coefficients;
    float state1;
    float state2;
} ECGBiquad;

typedef struct
{
    uint32_t sample_rate_hz;
    uint16_t quality_window_samples;
    uint16_t learning_samples;
    float adc_min;
    float adc_max;
    float clip_low;
    float clip_high;
    float clip_fraction;
    float flatline_peak_to_peak;
    float step_threshold;
} ECGPreprocessConfig;

typedef struct
{
    float raw_sample;
    float display_sample;
    float qrs_sample;
    float window_peak_to_peak;
    float clipped_fraction;
    uint32_t quality_flags;
    bool ready_for_qrs;
} ECGPreprocessResult;

typedef struct
{
    ECGPreprocessConfig config;
    ECGPreprocessResult result;
    ECGBiquad display_highpass;
    ECGBiquad display_lowpass;
    ECGBiquad qrs_highpass;
    ECGBiquad qrs_lowpass;
    float quality_window[ECG_PREPROCESS_QUALITY_WINDOW_MAX];
    float previous_raw;
    uint32_t processed_samples;
    uint16_t quality_write_index;
    uint16_t quality_count;
    uint16_t clipped_count;
    bool initialized;
} ECGPreprocessContext;

/* Loads the report's 500 Hz coefficients and general quality-control defaults. */
void ECGPreprocess_DefaultConfig(ECGPreprocessConfig *config);

/* Initializes dual filter paths: 0.5...40 Hz for display and 5...15 Hz for
 * QRS detection. The default coefficients require a 500 Hz input stream. */
void ECGPreprocess_Init(ECGPreprocessContext *context,
                        const ECGPreprocessConfig *config);

/* Sets filter state from the current ADC bias and begins a fresh learning
 * period. Call after startup, lead recovery, or a known acquisition break. */
void ECGPreprocess_Reset(ECGPreprocessContext *context, float initial_raw);

/* Processes every input sample without deleting or replacing it. Quality
 * flags describe the sample/window so the caller can gate QRS and HRV. */
const ECGPreprocessResult *ECGPreprocess_Push(ECGPreprocessContext *context,
                                              float raw_sample);

#ifdef __cplusplus
}
#endif

#endif /* ECG_PREPROCESS_H */
