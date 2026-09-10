#ifndef ECG_MONITOR_H
#define ECG_MONITOR_H

#include "ecg_preprocess.h"
#include "qrs_detector.h"
#include "heart_rate.h"
#include "hrv.h"
#include "ecg_landmark.h"

#define ECG_MONITOR_RATE 500U
#define ECG_HISTORY_SIZE 512U
#define ECG_DISPLAY_DELAY 250U

typedef struct {
    float waveform;
    bool waveform_valid;
    bool r_marker;
    ECGPoint point;
    bool new_r_peak;
    uint32_t r_sample;
    bool signal_valid;
    uint32_t quality_flags;
} ECGMonitorResult;

typedef struct {
    ECGPreprocessContext preprocess;
    QRSDetectorContext detector;
    HeartRateContext heart_rate;
    HRVContext hrv;
    ECGMonitorResult result;
    float history[ECG_HISTORY_SIZE];
    uint8_t markers[ECG_HISTORY_SIZE];
    uint32_t pending_r;
    uint16_t t_end;
    float polarity, r_amplitude;
    bool pending_s, pending_t;
    uint32_t sample_index;
    uint32_t flat_samples;
    uint16_t lead_off_samples;
    uint16_t lead_on_samples;
    uint32_t last_peak;
    bool has_peak;
    bool leads_off;
    bool invalid;
} ECGMonitor;

void ECGMonitor_Init(ECGMonitor *monitor);
/* Call only for continuous 500 Hz samples. Reinitialize after a lost block. */
const ECGMonitorResult *ECGMonitor_Push(ECGMonitor *monitor,
                                        uint16_t raw, bool leads_off);
/* Clear stale measurements if ADC delivery stops, even without new samples. */
void ECGMonitor_Invalidate(ECGMonitor *monitor);
#endif
