#ifndef ECG_PLOT_H
#define ECG_PLOT_H
#include <stdint.h>
#include <stdbool.h>
#include "ecg_landmark.h"
#define ECG_PLOT_WIDTH 240U
#define ECG_PLOT_HEIGHT 168U
#define ECG_PLOT_TOP 28U
/* Old-trace fade originally covered 60 columns. Keep only 30% of that span. */
#define ECG_PLOT_FADE_WIDTH 18U
#define ECG_PLOT_FADE_START_AGE (ECG_PLOT_WIDTH - ECG_PLOT_FADE_WIDTH)
#define ECG_PLOT_FADE_MID_AGE (ECG_PLOT_FADE_START_AGE + ECG_PLOT_FADE_WIDTH / 3U)
#define ECG_PLOT_FADE_DARK_AGE (ECG_PLOT_FADE_START_AGE + 2U * ECG_PLOT_FADE_WIDTH / 3U)
typedef struct {
    uint16_t low[ECG_PLOT_WIDTH], high[ECG_PLOT_WIDTH];
    uint16_t marker[ECG_PLOT_WIDTH];
    uint8_t point[ECG_PLOT_WIDTH], pending_point;
    float counts_per_pixel, sweep_peak;
    uint16_t x, previous_y, min_y, max_y, marker_y;
    uint8_t samples, scale_mode;
    bool previous_valid, marked;
} ECGPlot;
void ECGPlot_Init(ECGPlot *plot);
void ECGPlot_NextScale(ECGPlot *plot);
/* Five 500 Hz samples per column. Returns the changed column, or -1. */
int ECGPlot_Push(ECGPlot *plot, float sample, ECGPoint point);
/* Compose trace + retained PQRST squares/labels on adjacent-column refresh. */
void ECGPlot_RenderColumn(const ECGPlot *plot, uint16_t x, uint16_t *pixels);
#endif
