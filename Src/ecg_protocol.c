#include "ecg_protocol.h"
#include <math.h>
#include <string.h>

static void U16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void U32(uint8_t *p, uint32_t v) { U16(p, (uint16_t)v); U16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t Metric(float v, bool valid)
{
    if (!valid || !isfinite(v) || v < 0.0F) return UINT16_MAX;
    if (v >= 6553.4F) return 65534U;
    return (uint16_t)(v * 10.0F + .5F);
}
static int16_t Filtered(float v)
{
    if (!isfinite(v)) return 0;
    if (v >= 32767.0F) return INT16_MAX;
    if (v <= -32768.0F) return INT16_MIN;
    return (int16_t)(v >= 0.0F ? v + .5F : v - .5F);
}
void ECGProtocol_Init(ECGProtocol *p) { memset(p, 0, sizeof(*p)); }
void ECGProtocol_Sent(ECGProtocol *p, bool accepted)
{
    ++p->sequence;
    p->count = 0U;
    /* Retain any acquisition gap until a frame actually gets sent. */
    p->flags = accepted ? 0U : (uint16_t)(p->flags | 2U);
}
bool ECGProtocol_Push(ECGProtocol *p, uint32_t index, uint16_t raw,
                      const ECGMonitor *m)
{
    if (p->has_sample && index != p->next_sample) {
        if (p->count) ECGProtocol_Sent(p, false);
        p->flags |= 1U;
    }
    p->has_sample = true;
    p->next_sample = index + 1U;
    if (!p->count) U32(p->data + 12, index);
    uint8_t *r = p->data + 26U + p->count * 10U;
    uint32_t q = m->result.quality_flags;
    uint8_t quality = m->leads_off ? 1U : 0U;
    if (q & (ECG_QUALITY_CLIPPED | ECG_QUALITY_OUT_OF_RANGE)) quality |= 2U;
    if ((q & ECG_QUALITY_STEP_ARTIFACT) ||
        (!m->result.signal_valid && !(q & ECG_QUALITY_WARMING_UP) && !m->leads_off)) quality |= 4U;
    if (q & ECG_QUALITY_WARMING_UP) quality |= 8U;
    uint8_t alarm = 0U;
    if (m->leads_off || (quality & 6U) ||
        (!m->result.signal_valid && !(quality & 8U))) alarm = 1U;
    else if (m->hrv.result.alarm_active) alarm = 3U;
    else if (m->hrv.result.metrics_valid) alarm = 2U;
    U16(r, raw);
    U16(r + 2, (uint16_t)Filtered(m->preprocess.result.display_sample));
    /* Detector indices restart after monitor reset; ADC indices do not. */
    U32(r + 4, m->result.new_r_peak ? index - (m->sample_index - 1U) + m->result.r_sample : UINT32_MAX);
    r[8] = quality; r[9] = alarm;
    if (++p->count != 50U) return false;
    p->data[0] = 0xA5U; p->data[1] = 0x5AU;
    p->data[2] = 1U; p->data[3] = 1U;
    U16(p->data + 4, 500U); U16(p->data + 6, p->flags);
    U32(p->data + 8, p->sequence);
    U16(p->data + 16, ECG_MONITOR_RATE); U16(p->data + 18, 50U);
    U16(p->data + 20, Metric(m->heart_rate.result.display_bpm, m->heart_rate.result.valid));
    U16(p->data + 22, Metric(m->hrv.result.sdnn_ms, m->hrv.result.metrics_valid));
    U16(p->data + 24, Metric(m->hrv.result.rmssd_ms, m->hrv.result.metrics_valid));
    uint16_t crc = 0xFFFFU;
    for (unsigned i = 2U; i < ECG_FRAME_SIZE - 2U; ++i) {
        crc ^= (uint16_t)((uint16_t)p->data[i] << 8);
        for (unsigned b = 0U; b < 8U; ++b)
            crc = (uint16_t)((crc & 0x8000U) ? (crc << 1) ^ 0x1021U : crc << 1);
    }
    U16(p->data + ECG_FRAME_SIZE - 2U, crc);
    return true;
}
