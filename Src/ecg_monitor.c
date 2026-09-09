#include "ecg_monitor.h"
#include <math.h>
#include <string.h>

static float Smoothed(const ECGMonitor *m, uint32_t index)
{
    float sum = 0.0F;
    for (int i = -2; i <= 2; ++i)
        sum += m->history[(index + (uint32_t)i) % ECG_HISTORY_SIZE];
    return sum / 5.0F;
}

/* Morphology landmarks relative to a confirmed R: require an interior
   extremum with prominence above both window endpoints. No fixed-offset dots. */
static void MarkExtremum(ECGMonitor *m, uint32_t r, int first, int last,
                         ECGPoint point, float direction, float fraction)
{
    if (last - first < 8) return;
    float left = direction * Smoothed(m, r + (uint32_t)first);
    float right = direction * Smoothed(m, r + (uint32_t)last);
    float floor = fmaxf(left, right);
    float best = floor;
    uint32_t chosen = 0U;
    bool found = false;
    for (int offset = first + 3; offset <= last - 3; ++offset) {
        uint32_t pos = r + (uint32_t)offset;
        float value = direction * Smoothed(m, pos);
        if (value > best && value >= direction * Smoothed(m, pos - 1U) &&
            value > direction * Smoothed(m, pos + 1U)) {
            best = value; chosen = pos; found = true;
        }
    }
    float threshold = fmaxf(4.0F, fraction * m->r_amplitude);
    if (found && best - floor >= threshold)
        m->markers[chosen % ECG_HISTORY_SIZE] = (uint8_t)point;
}

static void FinishLandmarks(ECGMonitor *m, uint32_t index)
{
    uint32_t age = index - m->pending_r;
    if (m->pending_s && age >= 43U) {
        MarkExtremum(m, m->pending_r, 4, 40, ECG_POINT_S, -m->polarity, .015F);
        m->pending_s = false;
    }
    if (m->pending_t && age >= (uint32_t)m->t_end + 3U) {
        MarkExtremum(m, m->pending_r, 45, m->t_end, ECG_POINT_T, m->polarity, .04F);
        m->pending_t = false;
    }
}

void ECGMonitor_Invalidate(ECGMonitor *m)
{
    HeartRate_Invalidate(&m->heart_rate);
    HRV_Invalidate(&m->hrv);
    QRSDetector_Init(&m->detector, NULL);
    m->has_peak = false;
    m->pending_s = m->pending_t = false;
    m->invalid = true;
    m->result.signal_valid = false;
    memset(m->markers, 0, sizeof(m->markers));
}

void ECGMonitor_Init(ECGMonitor *m)
{
    const HeartRateConfig rate = {ECG_MONITOR_RATE, 30U, 220U};
    memset(m, 0, sizeof(*m));
    ECGPreprocess_Init(&m->preprocess, NULL);
    HeartRate_Init(&m->heart_rate, &rate);
    HRV_Init(&m->hrv, NULL);
    ECGMonitor_Invalidate(m);
}

const ECGMonitorResult *ECGMonitor_Push(ECGMonitor *m,
                                        uint16_t raw, bool leads_off)
{
    uint32_t index = m->sample_index++;
    uint32_t slot = index % ECG_HISTORY_SIZE;
    uint32_t candidate;
    const ECGPreprocessResult *p;
    bool bad;
    m->result.new_r_peak = false;
    if (leads_off != m->leads_off) {
        ECGMonitor_Invalidate(m);
        ECGPreprocess_Reset(&m->preprocess, (float)raw);
        m->flat_samples = 0U;
        m->leads_off = leads_off;
    }
    p = ECGPreprocess_Push(&m->preprocess, (float)raw);
    m->result.quality_flags = p->quality_flags;
    /* A half-second isoelectric segment is normal at slow rates. Only a
       sustained flat window (total 2 s) invalidates the measurement. */
    if ((p->quality_flags & ECG_QUALITY_FLATLINE) != 0U) {
        if (m->flat_samples < 750U) ++m->flat_samples;
    } else m->flat_samples = 0U;
    bad = leads_off || m->flat_samples >= 750U ||
          (p->quality_flags & (ECG_QUALITY_CLIPPED |
           ECG_QUALITY_OUT_OF_RANGE | ECG_QUALITY_STEP_ARTIFACT)) != 0U;
    if (bad && !m->invalid) {
        ECGMonitor_Invalidate(m);
        ECGPreprocess_Reset(&m->preprocess, (float)raw);
    }
    m->history[slot] = p->display_sample;
    m->markers[slot] = false;
    if (!m->invalid) FinishLandmarks(m, index);
    if (!bad && !(p->quality_flags & ECG_QUALITY_WARMING_UP)) {
        if (m->invalid) {
            m->invalid = false;
            HRV_SetSignalValid(&m->hrv);
        }
        m->result.signal_valid = true;
        if (QRSDetector_Push(&m->detector, index, p->qrs_sample, &candidate)) {
            /* Envelope/filter peaks lag R. Locate the largest absolute
               display extremum in the previous 160 ms, supporting inversion. */
            uint32_t peak = index;
            float largest = 0.0F;
            for (uint32_t age = 0U; age < 80U; ++age) {
                uint32_t pos = index - age;
                float amplitude = fabsf(m->history[pos % ECG_HISTORY_SIZE]);
                if (amplitude > largest) { largest = amplitude; peak = pos; }
            }
            HeartRateEvent event = HeartRate_PushRPeak(&m->heart_rate, peak);
            if (event != HEART_RATE_EVENT_REJECTED) {
                m->markers[peak % ECG_HISTORY_SIZE] = ECG_POINT_R;
                m->pending_r = peak;
                m->r_amplitude = largest;
                m->polarity = m->history[peak % ECG_HISTORY_SIZE] >= 0.0F ? 1.0F : -1.0F;
                uint16_t rr = m->heart_rate.result.valid ? m->heart_rate.result.rr_ms : 1000U;
                /* Shorten P/T search at high rates to avoid adjacent beats. */
                int p_start = -(int)(rr * 15U / 100U);
                if (p_start < -120) p_start = -120;
                m->t_end = (uint16_t)(rr * 27U / 100U);
                if (m->t_end > 180U) m->t_end = 180U;
                MarkExtremum(m, peak, p_start, -40, ECG_POINT_P, m->polarity, .02F);
                MarkExtremum(m, peak, -33, -4, ECG_POINT_Q, -m->polarity, .015F);
                m->pending_s = m->pending_t = true;
                FinishLandmarks(m, index);
                m->last_peak = peak;
                m->has_peak = true;
                m->result.new_r_peak = true;
                m->result.r_sample = peak;
                if (event == HEART_RATE_EVENT_UPDATED)
                    HRV_PushRR(&m->hrv, m->heart_rate.result.rr_ms);
            } else {
                /* A rejected interval breaks continuity for HRV. */
                HRV_Invalidate(&m->hrv);
                HRV_SetSignalValid(&m->hrv);
            }
        }
        if (m->has_peak && (index - m->last_peak > 1250U))
            ECGMonitor_Invalidate(m);
    }
    /* Delay only the displayed trace, allowing retrospective PQRST marking
       before its column is drawn. Timing measurements use original indices. */
    m->result.waveform_valid = m->preprocess.processed_samples > ECG_DISPLAY_DELAY;
    m->result.r_marker = false;
    m->result.point = ECG_POINT_NONE;
    if (m->result.waveform_valid) {
        uint32_t old = (index - ECG_DISPLAY_DELAY) % ECG_HISTORY_SIZE;
        m->result.waveform = m->history[old];
        m->result.point = (ECGPoint)m->markers[old];
        m->result.r_marker = m->result.point == ECG_POINT_R;
    }
    return &m->result;
}
