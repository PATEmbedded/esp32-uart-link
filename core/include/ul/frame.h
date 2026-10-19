#ifndef UL_FRAME_H
#define UL_FRAME_H

/*
 * Frames on the wire.
 *
 *   kind (1) | session (2, little endian) | seq (1) | payload (0..UL_MAX_PAYLOAD) | CRC-16 (2, little endian)
 *
 * The CRC covers everything before it. The whole frame is COBS-encoded and followed by a zero
 * byte, so frames can be told apart in a byte stream that starts or breaks anywhere.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ul/cobs.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UL_MAX_PAYLOAD
#define UL_MAX_PAYLOAD 256u
#endif

#define UL_HEADER_SIZE 4u
#define UL_CRC_SIZE 2u
#define UL_MAX_FRAME (UL_HEADER_SIZE + UL_MAX_PAYLOAD + UL_CRC_SIZE)
/** Largest frame on the wire, including the zero delimiter. */
#define UL_MAX_WIRE (UL_COBS_MAX_ENCODED(UL_MAX_FRAME) + 1u)

enum {
    UL_KIND_DATA = 1,     /**< needs an acknowledgement */
    UL_KIND_ACK = 2,      /**< acknowledges a DATA frame: session and seq of that frame, no payload */
    UL_KIND_DATAGRAM = 3, /**< not acknowledged, not repeated */
};

typedef struct {
    uint8_t kind;
    uint16_t session;
    uint8_t seq;
    const uint8_t *payload;
    size_t len;
} ul_frame_t;

/**
 * Writes a frame as it goes on the wire, delimiter included. Returns the number of bytes, or 0 if
 * the payload is longer than UL_MAX_PAYLOAD or out_size is too small (UL_MAX_WIRE always fits).
 */
size_t ul_frame_encode(const ul_frame_t *frame, uint8_t *out, size_t out_size);

typedef struct {
    uint32_t frames;        /**< frames with a correct CRC */
    uint32_t crc_errors;    /**< frames with a wrong CRC */
    uint32_t format_errors; /**< not valid COBS, or too short for a header and a CRC */
    uint32_t overruns;      /**< longer than the largest frame; dropped */
} ul_decoder_stats_t;

/** Receives one frame. payload points into the decoder and is valid only during the call. */
typedef void (*ul_frame_handler_t)(void *ctx, const ul_frame_t *frame);

typedef struct {
    uint8_t buf[UL_COBS_MAX_ENCODED(UL_MAX_FRAME)];
    size_t len;
    bool overrun; /* discarding bytes until the next delimiter */
    ul_decoder_stats_t stats;
} ul_decoder_t;

void ul_decoder_init(ul_decoder_t *dec);

/**
 * Takes bytes from the wire in pieces of any size and calls handler for every complete frame with
 * a correct CRC. Bytes before the first delimiter of a stream that started mid-frame end up in a
 * frame with a wrong CRC or format and are counted, not delivered.
 */
void ul_decoder_feed(ul_decoder_t *dec, const uint8_t *data, size_t len, ul_frame_handler_t handler, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
