#ifndef HRV_H
#define HRV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HRV_RR_WINDOW_SIZE 30U

typedef enum
{
    HRV_STATUS_SIGNAL_INVALID = 0,
    HRV_STATUS_COLLECTING,
    HRV_STATUS_NORMAL,
    HRV_STATUS_SUSPECTED_ARRHYTHMIA
} HRVStatus;

typedef enum
{
    HRV_ALARM_EVENT_NONE = 0,
    HRV_ALARM_EVENT_TRIGGERED,
    HRV_ALARM_EVENT_CLEARED
} HRVAlarmEvent;

typedef void (*HRVAlarmCallback)(bool alarm_active, void *user_data);

typedef struct
{
    float alarm_cv_threshold;
    float alarm_d_threshold;
    float clear_cv_threshold;
    float clear_d_threshold;
    uint8_t alarm_confirm_updates;
    uint8_t clear_confirm_updates;
    HRVAlarmCallback alarm_callback;
    void *callback_user_data;
} HRVConfig;

typedef struct
{
    HRVStatus status;
    HRVAlarmEvent alarm_event;
    /* Valid after two RR intervals; alarm evaluation begins at 30 RR. */
    bool metrics_valid;
    bool alarm_active;
    uint8_t rr_count;
    float mean_rr_ms;
    float sdnn_ms;
    float rmssd_ms;
    float cv;
    float d;
} HRVResult;

typedef struct
{
    HRVConfig config;
    HRVResult result;
    uint16_t rr_window[HRV_RR_WINDOW_SIZE];
    uint8_t rr_write_index;
    uint8_t abnormal_updates;
    uint8_t normal_updates;
} HRVContext;

/* Initializes the 30-RR short-window analyzer. Running SDNN/RMSSD become
 * available after two RR intervals; alarm evaluation starts when all 30 are
 * collected. A NULL config selects the default alarm thresholds. */
void HRV_Init(HRVContext *context, const HRVConfig *config);

/* Adds one continuous, quality-qualified RR interval in milliseconds.
 * Returns the current alarm edge event; the same event is also available in
 * HRVResult until the next call. */
HRVAlarmEvent HRV_PushRR(HRVContext *context, uint16_t rr_ms);

/* Call on lead-off, clipping, a missing ADC block, or unstable QRS detection.
 * It clears the window and suppresses/clears rhythm alarm output. */
void HRV_Invalidate(HRVContext *context);

/* Starts a fresh collection window after signal quality has recovered. */
void HRV_SetSignalValid(HRVContext *context);

const HRVResult *HRV_GetResult(const HRVContext *context);

#ifdef __cplusplus
}
#endif

#endif /* HRV_H */
