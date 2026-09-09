#ifndef ECG_BT_PROTOCOL_H
#define ECG_BT_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#define ECG_BT_SAMPLE_RATE 500U
#define ECG_BT_SAMPLE_COUNT 50U
#define ECG_BT_FRAME_SIZE 78U

typedef struct {
    uint8_t data[ECG_BT_FRAME_SIZE];
    uint32_t frame_sequence;
    uint32_t next_source_index;
    uint16_t count;
    uint16_t flags;
    bool has_source_sample;
} ECGBTProtocol;

void ECGBTProtocol_Init(ECGBTProtocol *protocol);
bool ECGBTProtocol_Push(ECGBTProtocol *protocol, uint32_t source_index,
                        uint16_t raw_adc);
void ECGBTProtocol_Sent(ECGBTProtocol *protocol, bool accepted);

#endif
