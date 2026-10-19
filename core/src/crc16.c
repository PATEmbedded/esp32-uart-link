#include "ul/crc16.h"

uint16_t ul_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((unsigned)data[i] << 8);
        for (int bit = 0; bit < 8; ++bit) {
            const unsigned shifted = (unsigned)crc << 1;
            crc = (uint16_t)((crc & 0x8000u) != 0u ? shifted ^ 0x1021u : shifted);
        }
    }
    return crc;
}

uint16_t ul_crc16(const uint8_t *data, size_t len)
{
    return ul_crc16_update(0xFFFFu, data, len);
}
