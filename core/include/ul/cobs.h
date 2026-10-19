#ifndef UL_COBS_H
#define UL_COBS_H

/*
 * Consistent Overhead Byte Stuffing (S. Cheshire and M. Baker, 1999).
 *
 * Encoded data contains no zero byte, so a zero byte can mark the end of every frame and a
 * receiver that starts in the middle of a frame finds the next one at the next zero.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest encoded size of len bytes, without the frame delimiter. */
#define UL_COBS_MAX_ENCODED(len) ((len) + (len) / 254u + 1u)

/**
 * Encodes len bytes into out, which must hold UL_COBS_MAX_ENCODED(len) bytes and must not
 * overlap in. Returns the encoded length. No delimiter is written.
 */
size_t ul_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);

/**
 * Decodes len bytes, without the delimiter, into out. out may be the same buffer as in; it needs
 * len bytes in any case. Returns false if the input is not valid COBS.
 */
bool ul_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
