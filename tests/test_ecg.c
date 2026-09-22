#include "ecg_monitor.h"
#include "ecg_plot.h"
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static ECGMonitor m;
static float gaussian(float t, float center, float width)
{
    float d = (t - center) / width;
    return expf(-0.5F * d * d);
}

static uint16_t ecg(float t, float period, float polarity, float gain, bool noisy)
{
    float phase = fmodf(t, period);
    float center = period * 0.35F;
    float wave = 100.0F * gaussian(phase, center - 0.14F, 0.025F)
               - 150.0F * gaussian(phase, center - 0.025F, 0.008F)
               + 1000.0F * gaussian(phase, center, 0.010F)
               - 240.0F * gaussian(phase, center + 0.025F, 0.010F)
               + 240.0F * gaussian(phase, center + 0.18F, 0.045F);
    float noise = noisy ? 180.0F * sinf(6.2831853F * 0.2F * t) +
                         25.0F * sinf(6.2831853F * 50.0F * t) : 0.0F;
    return (uint16_t)(2048.0F + polarity * gain * wave + noise);
}

static void regular(float bpm, float polarity, float gain, bool noisy)
{
    ECGMonitor_Init(&m);
    unsigned int peaks = 0U, markers = 0U, measurements = 0U;
    float worst = 0.0F, worst_position = 0.0F;
    float period = 60.0F / bpm;
    for (unsigned int i = 0; i < 30000U; ++i) {
        const ECGMonitorResult *r = ECGMonitor_Push(&m,
            ecg((float)i / 500.0F, period, polarity, gain, noisy), false);
        if (i > 5000U && r->new_r_peak) {
            ++peaks;
            float phase = fmodf((float)r->r_sample / 500.0F, period);
            float position_error = fabsf(phase - period * 0.35F);
            if (position_error > worst_position) worst_position = position_error;
            if (m.heart_rate.result.valid) {
                float err = fabsf(m.heart_rate.result.display_bpm - bpm) / bpm;
                if (err > worst) worst = err;
                ++measurements;
            }
        }
        if (i > 5000U && r->r_marker) ++markers;
    }
    printf("BPM %.0f polarity %.0f gain %.2f noise %d: peaks %u markers %u error %.3f%% R offset %.1f ms\n",
           bpm, polarity, gain, noisy, peaks, markers, worst * 100.0F, worst_position * 1000.0F);
    fflush(stdout);
    assert(measurements > 10U);
    assert(fabsf((float)peaks - 50.0F / period) <= 2.0F);
    assert(abs((int)peaks - (int)markers) <= 1);
    assert(worst <= 0.05F);
    assert(worst_position < 0.030F);
    assert(m.hrv.result.metrics_valid && !m.hrv.result.alarm_active);
}

static void hrv_metrics(void)
{
    HRVContext h;
    HRV_Init(&h, NULL);
    HRV_PushRR(&h, 800U);
    assert(!h.result.metrics_valid && h.result.rr_count == 1U);
    HRV_PushRR(&h, 1000U);
    assert(h.result.metrics_valid && h.result.rr_count == 2U);
    assert(fabsf(h.result.mean_rr_ms - 900.0F) < 0.01F);
    assert(fabsf(h.result.sdnn_ms - 141.42136F) < 0.01F);
    assert(fabsf(h.result.rmssd_ms - 200.0F) < 0.01F);
    assert(!h.result.alarm_active);
    HRV_Init(&h, NULL);
    for (unsigned int i = 0; i < 32U; ++i) HRV_PushRR(&h, i % 2U ? 1000U : 600U);
    assert(h.result.alarm_active);
    assert(fabsf(h.result.mean_rr_ms - 800.0F) < 0.01F);
    assert(fabsf(h.result.sdnn_ms - sqrtf(30.0F * 40000.0F / 29.0F)) < 0.01F);
    assert(fabsf(h.result.rmssd_ms - 400.0F) < 0.01F);
    for (unsigned int i = 0; i < 40U; ++i) HRV_PushRR(&h, 800U);
    assert(!h.result.alarm_active);
    HRV_Invalidate(&h);
    assert(!h.result.metrics_valid && !h.result.alarm_active);
    puts("HRV statistics, chronological ring wrap, alarm and hysteresis: PASS");
}

static void invalid_and_recovery(void)
{
    for (unsigned int i = 0; i < 1600U; ++i) ECGMonitor_Push(&m, 2048U, false);
    assert(!m.heart_rate.result.valid && !m.hrv.result.metrics_valid);
    for (unsigned int i = 0; i < 125U; ++i)
        ECGMonitor_Push(&m, 2048U, true);
    assert(!m.result.signal_valid && !m.hrv.result.alarm_active);
    for (unsigned int i = 0; i < 7000U; ++i)
        ECGMonitor_Push(&m, ecg((float)i / 500.0F, 1.0F, 1, 1, false), false);
    assert(m.heart_rate.result.valid);
    for (unsigned int i = 0; i < 500U; ++i) ECGMonitor_Push(&m, 4095U, false);
    assert(!m.heart_rate.result.valid && !m.hrv.result.alarm_active);
    ECGMonitor_Init(&m); /* queue discontinuity */
    assert(!m.heart_rate.result.valid && m.hrv.result.rr_count == 0U);
    puts("Flatline, lead-off, reconnect, clipping, dropped-block reset: PASS");
}

static void quality_fault_keeps_waveform_running(void)
{
    ECGMonitor_Init(&m);
    const ECGMonitorResult *result = NULL;
    for (unsigned int i = 0U; i < 1300U; ++i)
        result = ECGMonitor_Push(&m,
            ecg((float)i / 500.0F, 1.0F, 1.0F, 1.0F, false), false);
    assert(result != NULL && result->waveform_valid);

    /* A short LO+/LO- chatter pulse must not erase a valid rhythm. */
    result = ECGMonitor_Push(&m, 2048U, true);
    assert(result->signal_valid);
    for (unsigned int i = 1U; i < 125U; ++i)
        result = ECGMonitor_Push(&m, 2048U, true);
    assert(!result->signal_valid);
    assert(result->waveform_valid);
    result = ECGMonitor_Push(&m, 2048U, false);
    assert(result->waveform_valid);
    puts("Lead/contact quality faults keep the LCD waveform continuous: PASS");
}

static void isolated_step_preserves_rhythm_alarm(void)
{
    ECGMonitor_Init(&m);
    ECGPreprocess_Reset(&m.preprocess, 2048.0F);
    m.preprocess.processed_samples = 1001U;
    m.invalid = false;
    m.result.signal_valid = true;
    HRV_SetSignalValid(&m.hrv);
    for (unsigned int i = 0U; i < 32U; ++i)
        HRV_PushRR(&m.hrv, (i & 1U) != 0U ? 1000U : 600U);
    assert(m.hrv.result.alarm_active && m.hrv.result.rr_count == 30U);

    const ECGMonitorResult *result = ECGMonitor_Push(&m, 3000U, false);
    assert((result->quality_flags & ECG_QUALITY_STEP_ARTIFACT) != 0U);
    assert(result->signal_valid);
    assert(m.hrv.result.alarm_active && m.hrv.result.rr_count == 30U);
    puts("Isolated ADC step preserves accumulated rhythm alarm: PASS");
}

static void rr_wrap(void)
{
    HeartRateContext h;
    HeartRate_Init(&h, NULL);
    assert(HeartRate_PushRPeak(&h, UINT32_MAX - 249U) == HEART_RATE_EVENT_FIRST_PEAK);
    assert(HeartRate_PushRPeak(&h, 250U) == HEART_RATE_EVENT_UPDATED);
    assert(h.result.rr_ms == 1000U);
    assert(HeartRate_PushRPeak(&h, 260U) == HEART_RATE_EVENT_REJECTED);
    assert(HeartRate_PushRPeak(&h, 750U) == HEART_RATE_EVENT_UPDATED);
    assert(h.result.rr_ms == 1000U);
    assert(HeartRate_PushRPeak(&h, 760U) == HEART_RATE_EVENT_REJECTED);
    assert(HeartRate_PushRPeak(&h, 2000U) == HEART_RATE_EVENT_GAP);
    assert(HeartRate_PushRPeak(&h, 2500U) == HEART_RATE_EVENT_UPDATED);
    puts("RR counter wrap, premature rejection and overlong-gap distinction: PASS");
}

static float filter_rms(float hz)
{
    ECGPreprocessContext p;
    ECGPreprocess_Init(&p, NULL);
    float sum = 0.0F;
    for (unsigned int i = 0; i < 20000U; ++i) {
        float value = 2048.0F + 200.0F * sinf(6.2831853F * hz * (float)i / 500.0F);
        const ECGPreprocessResult *r = ECGPreprocess_Push(&p, value);
        if (i >= 10000U) sum += r->display_sample * r->display_sample;
    }
    return sqrtf(sum / 10000.0F);
}

static void filter_response(void)
{
    float pass = filter_rms(10.0F);
    float baseline = filter_rms(0.1F);
    float mains = filter_rms(50.0F);
    float high = filter_rms(100.0F);
    printf("Display filter RMS: 0.1 Hz %.2f, 10 Hz %.2f, 50 Hz %.2f, 100 Hz %.2f\n",
           baseline, pass, mains, high);
    assert(pass > 130.0F && pass < 150.0F);
    assert(baseline < pass * 0.10F);
    assert(mains < pass * 0.02F);
    assert(high < pass * 0.20F);
}

static void irregular_pipeline(void)
{
    ECGMonitor_Init(&m);
    unsigned int within = 0U, beat = 0U, detections = 0U;
    bool alarm_seen = false;
    for (unsigned int i = 0; i < 30000U; ++i) {
        unsigned int period = beat % 2U ? 500U : 300U;
        /* Fixed morphology and R at 200 ms; alternate RR 600/1000 ms. */
        float t = (float)within / 500.0F;
        float wave = 2048.0F + 1000.0F * gaussian(t, .2F, .01F)
                     - 200.0F * gaussian(t, .225F, .01F)
                     + 200.0F * gaussian(t, .38F, .04F);
        const ECGMonitorResult *r = ECGMonitor_Push(&m, (uint16_t)wave, false);
        if (r->new_r_peak) ++detections;
        if (m.hrv.result.alarm_active) alarm_seen = true;
        if (++within == period) { within = 0; ++beat; }
    }
    printf("Irregular ECG: %u R peaks, alarm %d\n", detections, alarm_seen);
    assert(detections > 65U && detections < 80U);
    assert(alarm_seen && m.hrv.result.alarm_active);
    for (unsigned int i = 0U; i < 125U; ++i)
        ECGMonitor_Push(&m, 2048U, true);
    assert(!m.hrv.result.alarm_active && !m.heart_rate.result.valid);
}

static void plot_visibility(void)
{
    ECGPlot p;
    uint16_t pixels[ECG_PLOT_HEIGHT];
    ECGPlot_Init(&p);
    /* A 100-count R used to occupy only 6 pixels at the old fixed gain. */
    for (unsigned i = 0; i < 1200U; ++i)
        ECGPlot_Push(&p, i % 400U == 0U ? 100.0F : 0.0F, false);
    assert(fabsf(p.counts_per_pixel - 100.0F / 60.0F) < 0.01F);
    for (unsigned i = 0; i < 5U; ++i) ECGPlot_Push(&p, 100.0F, i == 0U);
    assert(p.marker[0] >= 23U && p.marker[0] <= 25U);
    /* Clear/draw all following columns, then verify square AND R survive. */
    for (unsigned i = 0; i < 40U; ++i) ECGPlot_Push(&p, 0.0F, false);
    unsigned red = 0U;
    for (unsigned x = 0; x < 5U; ++x) {
        ECGPlot_RenderColumn(&p, x, pixels);
        for (unsigned y = 0; y < ECG_PLOT_HEIGHT; ++y) red += pixels[y] == 0xF800U;
    }
    assert(red > 35U);
    /* The sweep head is cyan and increasingly old trace columns are dimmer. */
    ECGPlot_Init(&p);
    p.x = 0U;
    for (unsigned x = 0U; x < ECG_PLOT_WIDTH; ++x)
        p.low[x] = p.high[x] = 80U;
    ECGPlot_RenderColumn(&p, 0U, pixels);
    for (unsigned y = 0U; y < ECG_PLOT_HEIGHT; ++y)
        assert(pixels[y] == 0x07FFU);
    ECGPlot_RenderColumn(&p, 239U, pixels);
    assert(pixels[80] == 0x07E0U);
    ECGPlot_RenderColumn(&p, 17U, pixels);
    assert(pixels[80] == 0x03E0U);
    ECGPlot_RenderColumn(&p, 5U, pixels);
    assert(pixels[80] == 0x00E0U);
    for (unsigned mode = 1; mode <= 4; ++mode) {
        ECGPlot_NextScale(&p);
        assert(p.scale_mode == mode);
        assert(p.counts_per_pixel == 16.0F / (float)(1U << (mode - 1U)));
    }
    ECGPlot_NextScale(&p);
    assert(!p.scale_mode);
    ECGMonitor_Init(&m);
    for (unsigned i = 0; i < 24000U; ++i) {
        const ECGMonitorResult *r = ECGMonitor_Push(&m,
            ecg(i / 500.0F, .8F, 1, .3F, true), false);
        if (r->waveform_valid) ECGPlot_Push(&p, r->waveform, r->point);
    }
    static uint16_t frame[ECG_PLOT_HEIGHT][ECG_PLOT_WIDTH];
    for (unsigned x = 0; x < ECG_PLOT_WIDTH; ++x) {
        ECGPlot_RenderColumn(&p, x, pixels);
        for (unsigned y = 0; y < ECG_PLOT_HEIGHT; ++y) frame[y][x] = pixels[y];
    }
    FILE *f = fopen("build/host-tests/ecg_plot.ppm", "wb");
    assert(f);
    fprintf(f, "P6\n%u %u\n255\n", ECG_PLOT_WIDTH, ECG_PLOT_HEIGHT);
    for (unsigned y = 0; y < ECG_PLOT_HEIGHT; ++y)
        for (unsigned x = 0; x < ECG_PLOT_WIDTH; ++x) {
            uint16_t c = frame[y][x];
            fputc(((c >> 11) & 31) * 255 / 31, f);
            fputc(((c >> 5) & 63) * 255 / 63, f);
            fputc((c & 31) * 255 / 31, f);
        }
    fclose(f);
    puts("Auto gain, manual gain cycling, R square/label persistence: PASS");
}

static void landmarks(float polarity, bool noisy, bool absent_pt)
{
    unsigned count[6] = {0};
    float worst[6] = {0};
    const float offsets[6] = {0, 0, -.14F, -.025F, .025F, .18F};
    ECGMonitor_Init(&m);
    for (unsigned i = 0; i < 20000U; ++i) {
        float t = i / 500.0F;
        uint16_t raw = ecg(t, .8F, polarity, 1, noisy);
        if (absent_pt) {
            float phase = fmodf(t, .8F);
            raw = (uint16_t)(2048.0F + polarity * (
                -150 * gaussian(phase, .255F, .008F)
                +1000 * gaussian(phase, .28F, .01F)
                -240 * gaussian(phase, .305F, .01F)));
        }
        const ECGMonitorResult *r = ECGMonitor_Push(&m, raw, false);
        if (i > 5000U && r->point) {
            ++count[r->point];
            float phase = fmodf((i - ECG_DISPLAY_DELAY) / 500.0F, .8F);
            float error = fabsf(phase - (.28F + offsets[r->point]));
            if (error > worst[r->point]) worst[r->point] = error;
        }
    }
    printf("PQRST polarity %.0f noise %d absent PT %d:", polarity, noisy, absent_pt);
    for (unsigned k = 1; k <= 5; ++k) printf(" %u/%.1fms", count[k], worst[k] * 1000);
    puts(""); fflush(stdout);
    for (unsigned k = 1; k <= 5; ++k) {
        if (absent_pt && (k == ECG_POINT_P || k == ECG_POINT_T)) assert(count[k] == 0U);
        else { assert(count[k] >= 35U && count[k] <= 39U); assert(worst[k] < .045F); }
    }
}

int main(void)
{
    const float rates[] = {40, 60, 75, 90, 120, 150, 180, 200};
    for (unsigned int i = 0; i < sizeof(rates)/sizeof(rates[0]); ++i)
        regular(rates[i], 1, 1, false);
    regular(60, -1, 1, false);
    regular(75, 1, 0.3F, true);
    invalid_and_recovery();
    quality_fault_keeps_waveform_running();
    isolated_step_preserves_rhythm_alarm();
    hrv_metrics();
    rr_wrap();
    filter_response();
    irregular_pipeline();
    plot_visibility();
    landmarks(1, false, false);
    landmarks(-1, false, false);
    landmarks(1, true, false);
    landmarks(1, false, true);
    puts("All ECG acceptance regression tests passed.");
    return 0;
}
