#include "ecg_plot.h"
#include <math.h>
#include <string.h>
#define EMPTY UINT16_MAX
#define BLACK 0x0000U
#define GREEN 0x07E0U
#define RED 0xF800U
#define GRID 0x1082U

void ECGPlot_Init(ECGPlot *p)
{
    memset(p, 0, sizeof(*p));
    for (unsigned i = 0; i < ECG_PLOT_WIDTH; ++i)
        p->low[i] = p->high[i] = p->marker[i] = EMPTY;
    p->counts_per_pixel = 4.0F;
}

void ECGPlot_NextScale(ECGPlot *p)
{
    uint8_t next = (uint8_t)((p->scale_mode + 1U) % 5U);
    ECGPlot_Init(p);
    p->scale_mode = next;
    if (next) p->counts_per_pixel = 16.0F / (float)(1U << (next - 1U));
}

static uint16_t MapY(const ECGPlot *p, float sample)
{
    float y = ECG_PLOT_HEIGHT / 2.0F - sample / p->counts_per_pixel;
    /* Keep room for labels, even when manual gain clips the trace. */
    if (y < 14.0F) y = 14.0F;
    if (y > ECG_PLOT_HEIGHT - 10.0F) y = ECG_PLOT_HEIGHT - 10.0F;
    return (uint16_t)y;
}

int ECGPlot_Push(ECGPlot *p, float sample, ECGPoint point)
{
    float amplitude = fabsf(sample);
    if (amplitude > p->sweep_peak) p->sweep_peak = amplitude;
    uint16_t y = MapY(p, sample);
    if (!p->samples) { p->min_y = p->max_y = y; p->marked = false; }
    if (y < p->min_y) p->min_y = y;
    if (y > p->max_y) p->max_y = y;
    if (point != ECG_POINT_NONE) {
        p->marked = true; p->marker_y = y; p->pending_point = (uint8_t)point;
    }
    if (++p->samples < 5U) return -1;
    if (p->previous_valid) {
        if (p->previous_y < p->min_y) p->min_y = p->previous_y;
        if (p->previous_y > p->max_y) p->max_y = p->previous_y;
    }
    uint16_t x = p->x;
    p->low[x] = p->min_y;
    p->high[x] = p->max_y;
    p->marker[x] = p->marked ? p->marker_y : EMPTY;
    p->point[x] = p->marked ? p->pending_point : ECG_POINT_NONE;
    p->previous_y = y;
    p->previous_valid = true;
    p->samples = 0U;
    if (++p->x == ECG_PLOT_WIDTH) {
        p->x = 0U;
        p->previous_valid = false;
        if (!p->scale_mode) {
            /* Sweep-wise gain avoids changing the size halfway through a beat.
               At most 16x base gain; do not magnify an absent signal endlessly. */
            float scale = p->sweep_peak / 60.0F;
            if (scale < 1.0F) scale = 1.0F;
            if (scale > 32.0F) scale = 32.0F;
            p->counts_per_pixel = scale;
        }
        p->sweep_peak = 0.0F;
    }
    return x;
}

void ECGPlot_RenderColumn(const ECGPlot *p, uint16_t x, uint16_t *pixels)
{
    static const uint8_t glyphs[6][5] = {
        {0}, {0x7F,0x09,0x19,0x29,0x46}, /* R */
        {0x7F,0x09,0x09,0x09,0x06}, /* P */
        {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
        {0x46,0x49,0x49,0x49,0x31}, /* S */
        {0x01,0x01,0x7F,0x01,0x01}  /* T */
    };
    static const uint16_t colors[6] = {BLACK, RED, 0x07FFU, 0xFD20U, 0xF81FU, 0xFFE0U};
    for (unsigned y = 0; y < ECG_PLOT_HEIGHT; ++y) {
        pixels[y] = (x % 20U == 0U || y % 20U == 4U) ? GRID : BLACK;
        if (p->low[x] != EMPTY && y >= p->low[x] && y <= p->high[x]) pixels[y] = GREEN;
    }
    /* Draw only this column's intersection with every nearby annotation.
       Incoming waveform columns therefore cannot erase a retained label. */
    int start = x > 4U ? (int)x - 4 : 0;
    int end = x + 4U < ECG_PLOT_WIDTH ? (int)x + 4 : ECG_PLOT_WIDTH - 1;
    for (int center = start; center <= end; ++center) {
        if (p->marker[center] == EMPTY) continue;
        int mx = center < 2 ? 2 : center > 237 ? 237 : center;
        int col = (int)x - (mx - 2);
        if (col < 0 || col >= 5) continue;
        int my = p->marker[center];
        uint8_t point = p->point[center];
        if (point == ECG_POINT_NONE || point > ECG_POINT_T) continue;
        uint16_t color = colors[point];
        int label_y = my - 12;
        if (point == ECG_POINT_Q) label_y = my + 6;
        if (point == ECG_POINT_S) label_y = my + 16;
        if (label_y > (int)ECG_PLOT_HEIGHT - 7) label_y = ECG_PLOT_HEIGHT - 7;
        for (int y = my - 2; y <= my + 2; ++y) pixels[y] = color;
        for (int row = 0; row < 7; ++row)
            if (glyphs[point][col] & (1U << row)) pixels[label_y + row] = color;
    }
}
