#ifndef ECG_PROTOCOL_H
#define ECG_PROTOCOL_H
#include "ecg_monitor.h"
#define ECG_FRAME_SIZE 528U
typedef struct {
    uint8_t data[ECG_FRAME_SIZE];
    uint32_t sequence, next_sample;
    uint16_t count, flags;
    bool has_sample;
} ECGProtocol;
void ECGProtocol_Init(ECGProtocol *p);
/* Returns a complete frame. Call Sent before pushing the next sample. */
bool ECGProtocol_Push(ECGProtocol *p, uint32_t index, uint16_t raw,
                      const ECGMonitor *m);
void ECGProtocol_Sent(ECGProtocol *p, bool accepted);
#endif
