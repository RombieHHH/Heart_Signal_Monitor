#include "ecg_bt_protocol.h"
#include <string.h>

static void PutU16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void PutU32(uint8_t *p, uint32_t value)
{
    PutU16(p, (uint16_t)value);
    PutU16(p + 2, (uint16_t)(value >> 16));
}

void ECGBTProtocol_Init(ECGBTProtocol *protocol)
{
    memset(protocol, 0, sizeof(*protocol));
}

void ECGBTProtocol_Sent(ECGBTProtocol *protocol, bool accepted)
{
    ++protocol->frame_sequence;
    protocol->count = 0U;
    protocol->flags = accepted ? 0U : (uint16_t)(protocol->flags | 2U);
}

bool ECGBTProtocol_Push(ECGBTProtocol *protocol, uint32_t source_index,
                        uint16_t raw_adc)
{
    if (protocol->has_source_sample &&
        source_index != protocol->next_source_index) {
        if (protocol->count != 0U) ECGBTProtocol_Sent(protocol, false);
        protocol->flags |= 1U;
    }
    protocol->has_source_sample = true;
    protocol->next_source_index = source_index + 1U;

    if (protocol->count == 0U) PutU32(protocol->data + 12, source_index);
    /* The ADC is 12-bit. Mid-bin reconstruction on the host limits error to 8 counts. */
    protocol->data[26U + protocol->count] = (uint8_t)((raw_adc & 0x0FFFU) >> 4);
    if (++protocol->count != ECG_BT_SAMPLE_COUNT) return false;

    protocol->data[0] = 0xA5U;
    protocol->data[1] = 0x5AU;
    protocol->data[2] = 2U;
    protocol->data[3] = 2U;
    PutU16(protocol->data + 4, ECG_BT_SAMPLE_COUNT);
    PutU16(protocol->data + 6, protocol->flags);
    PutU32(protocol->data + 8, protocol->frame_sequence);
    PutU16(protocol->data + 16, ECG_BT_SAMPLE_RATE);
    PutU16(protocol->data + 18, ECG_BT_SAMPLE_COUNT);
    PutU16(protocol->data + 20, UINT16_MAX);
    PutU16(protocol->data + 22, UINT16_MAX);
    PutU16(protocol->data + 24, UINT16_MAX);

    uint16_t crc = 0xFFFFU;
    for (uint16_t i = 2U; i < ECG_BT_FRAME_SIZE - 2U; ++i) {
        crc ^= (uint16_t)((uint16_t)protocol->data[i] << 8);
        for (uint8_t bit = 0U; bit < 8U; ++bit)
            crc = (uint16_t)((crc & 0x8000U) != 0U
                ? (crc << 1) ^ 0x1021U : crc << 1);
    }
    PutU16(protocol->data + ECG_BT_FRAME_SIZE - 2U, crc);
    return true;
}
