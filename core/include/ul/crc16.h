#ifndef UL_CRC16_H
#define UL_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR.
 * The check value for "123456789" is 0x29B1. Python computes the same with
 * binascii.crc_hqx(data, 0xFFFF).
 */
uint16_t ul_crc16(const uint8_t *data, size_t len);

/** Continues a CRC over more data; ul_crc16(d, n) equals ul_crc16_update(0xFFFF, d, n). */
uint16_t ul_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
