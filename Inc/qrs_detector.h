#ifndef QRS_DETECTOR_H
#define QRS_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t sample_rate_millihz;
    uint16_t learning_ms;
    uint16_t refractory_ms;
    float threshold_weight;
    uint8_t envelope_shift;
} QRSDetectorConfig;

typedef struct
{
    QRSDetectorConfig config;
    float envelope;
    float previous_envelope;
    float previous_previous_envelope;
    float learning_peak_sum;
    float signal_level;
    float noise_level;
    uint32_t learning_peak_count;
    uint32_t processed_samples;
    uint32_t previous_sample_index;
    uint32_t last_peak_sample;
    bool has_last_peak;
    bool learning_complete;
} QRSDetectorContext;

void QRSDetector_DefaultConfig(QRSDetectorConfig *config);
void QRSDetector_Init(QRSDetectorContext *context,
                      const QRSDetectorConfig *config);
void QRSDetector_SetSampleRateMilliHz(QRSDetectorContext *context,
                                      uint32_t sample_rate_millihz);

/* Push the 5...15 Hz output from ECGPreprocess_Push(). On true, peak_sample
 * contains the detected R-peak sample index. No history buffer is allocated. */
bool QRSDetector_Push(QRSDetectorContext *context,
                      uint32_t sample_index,
                      float qrs_sample,
                      uint32_t *peak_sample);

#ifdef __cplusplus
}
#endif

#endif /* QRS_DETECTOR_H */
