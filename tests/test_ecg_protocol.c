#include "ecg_protocol.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ECGProtocol p;
static ECGMonitor m;
static void Frame(FILE *f, uint32_t start)
{
    for (uint32_t n = 0; n < 50; ++n) {
        m.sample_index = n + 1U;
        m.result.new_r_peak = n == 20U;
        m.result.r_sample = 10U;
        m.preprocess.result.display_sample = n == 0 ? -40000.0F :
            n == 1 ? 40000.0F : n == 2 ? -1.5F : 1.5F;
        assert(ECGProtocol_Push(&p, start + n, (uint16_t)(2000 + n), &m) == (n == 49));
    }
    assert(fwrite(p.data, 1, ECG_FRAME_SIZE, f) == ECG_FRAME_SIZE);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "wb");
    assert(f);
    ECGProtocol_Init(&p);
    m.result.quality_flags = ECG_QUALITY_WARMING_UP;
    Frame(f, 0);
    ECGProtocol_Sent(&p, true);
    m.result.quality_flags = 0;
    m.result.signal_valid = true;
    m.heart_rate.result.valid = true;
    m.heart_rate.result.display_bpm = 72.3F;
    m.hrv.result.metrics_valid = true;
    m.hrv.result.sdnn_ms = 12.4F;
    m.hrv.result.rmssd_ms = 24.6F;
    Frame(f, 50);
    ECGProtocol_Sent(&p, false);
    Frame(f, 100);
    ECGProtocol_Sent(&p, true);
    m.sample_index = 1;
    assert(!ECGProtocol_Push(&p, 150, 2000, &m));
    m.leads_off = true;
    Frame(f, 200);
    ECGProtocol_Sent(&p, true);
    m.leads_off = false;
    m.hrv.result.alarm_active = true;
    Frame(f, 250);
    ECGProtocol_Sent(&p, true);
    p.has_sample = false;
    Frame(f, UINT32_MAX - 24U);
    ECGProtocol_Sent(&p, true);
    Frame(f, 25);
    fclose(f);
    return 0;
}
