#include "ul/frame.h"

#include <string.h>

#include "ul/crc16.h"

size_t ul_frame_encode(const ul_frame_t *frame, uint8_t *out, size_t out_size)
{
    if (frame->len > UL_MAX_PAYLOAD) {
        return 0;
    }
    const size_t raw_len = UL_HEADER_SIZE + frame->len + UL_CRC_SIZE;
    if (out_size < UL_COBS_MAX_ENCODED(raw_len) + 1u) {
        return 0;
    }

    uint8_t raw[UL_MAX_FRAME];
    raw[0] = frame->kind;
    raw[1] = (uint8_t)(frame->session & 0xFFu);
    raw[2] = (uint8_t)(frame->session >> 8);
    raw[3] = frame->seq;
    if (frame->len != 0u) {
        memcpy(&raw[UL_HEADER_SIZE], frame->payload, frame->len);
    }
    const size_t crc_at = UL_HEADER_SIZE + frame->len;
    const uint16_t crc = ul_crc16(raw, crc_at);
    raw[crc_at] = (uint8_t)(crc & 0xFFu);
    raw[crc_at + 1u] = (uint8_t)(crc >> 8);

    const size_t encoded = ul_cobs_encode(raw, raw_len, out);
    out[encoded] = 0;
    return encoded + 1u;
}

void ul_decoder_init(ul_decoder_t *dec)
{
    memset(dec, 0, sizeof(*dec));
}

static void finish_frame(ul_decoder_t *dec, ul_frame_handler_t handler, void *ctx)
{
    size_t len = 0;
    /* The buffer fits the largest encoded frame, and other input of that size can decode to more. */
    if (!ul_cobs_decode(dec->buf, dec->len, dec->buf, &len) || len < UL_HEADER_SIZE + UL_CRC_SIZE ||
        len > UL_MAX_FRAME) {
        dec->stats.format_errors++;
        return;
    }
    const size_t crc_at = len - UL_CRC_SIZE;
    const uint16_t crc = (uint16_t)(dec->buf[crc_at] | (dec->buf[crc_at + 1u] << 8));
    if (ul_crc16(dec->buf, crc_at) != crc) {
        dec->stats.crc_errors++;
        return;
    }
    dec->stats.frames++;
    const ul_frame_t frame = {
        .kind = dec->buf[0],
        .session = (uint16_t)(dec->buf[1] | (dec->buf[2] << 8)),
        .seq = dec->buf[3],
        .payload = &dec->buf[UL_HEADER_SIZE],
        .len = crc_at - UL_HEADER_SIZE,
    };
    handler(ctx, &frame);
}

void ul_decoder_feed(ul_decoder_t *dec, const uint8_t *data, size_t len, ul_frame_handler_t handler, void *ctx)
{
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        if (byte == 0u) {
            if (dec->overrun) {
                dec->stats.overruns++;
            } else if (dec->len != 0u) {
                finish_frame(dec, handler, ctx);
            }
            /* An empty frame (two delimiters in a row) is idle time, not an error. */
            dec->len = 0;
            dec->overrun = false;
        } else if (!dec->overrun) {
            if (dec->len == sizeof(dec->buf)) {
                dec->overrun = true;
            } else {
                dec->buf[dec->len++] = byte;
            }
        }
    }
}
