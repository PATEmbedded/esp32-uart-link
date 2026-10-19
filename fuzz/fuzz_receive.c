/*
 * libFuzzer target for the receive path.
 *
 * Every input goes into a link that has a message in flight, in pieces whose sizes the input
 * picks, and must not crash it or hand over a payload longer than UL_MAX_PAYLOAD. Inputs short
 * enough to be a payload must also come back unchanged through the encoder and the decoder.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ul/link.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void ignore_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    (void)bytes;
    if (len > UL_MAX_WIRE) {
        abort();
    }
}

static bool check_message(void *ctx, const uint8_t *payload, size_t len, bool reliable)
{
    (void)ctx;
    (void)reliable;
    if (len > UL_MAX_PAYLOAD || (len != 0u && payload == NULL)) {
        abort();
    }
    return (len & 1u) == 0u; /* refuse some, to reach that path as well */
}

typedef struct {
    const uint8_t *payload;
    size_t len;
    int found;
} round_trip_t;

static void check_round_trip(void *ctx, const ul_frame_t *frame)
{
    round_trip_t *rt = ctx;
    rt->found++;
    if (frame->kind != UL_KIND_DATAGRAM || frame->session != 0x5A5Au || frame->len != rt->len ||
        (rt->len != 0u && memcmp(frame->payload, rt->payload, rt->len) != 0)) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static ul_link_t link;
    const ul_link_config_t config = {
        .write = ignore_write,
        .on_message = check_message,
        .ctx = NULL,
        .session = 0x1234,
        .ack_timeout_ms = 10,
        .max_retries = 2,
    };
    ul_link_init(&link, &config);
    (void)ul_link_send(&link, "x", 1, 0);

    uint32_t now = 0;
    for (size_t at = 0; at < size;) {
        size_t piece = 1u + data[at] % 64u;
        if (piece > size - at) {
            piece = size - at;
        }
        ul_link_receive(&link, &data[at], piece, now);
        at += piece;
        now += 3u;
    }
    ul_link_poll(&link, now + 100u);
    const ul_link_stats_t stats = ul_link_stats(&link);
    if (stats.wire.frames < stats.received + stats.refused + stats.duplicates + stats.ignored) {
        abort();
    }

    if (size <= UL_MAX_PAYLOAD) {
        static uint8_t wire[UL_MAX_WIRE];
        const ul_frame_t frame = {UL_KIND_DATAGRAM, 0x5A5A, 0, data, size};
        const size_t n = ul_frame_encode(&frame, wire, sizeof(wire));
        static ul_decoder_t dec;
        ul_decoder_init(&dec);
        round_trip_t rt = {data, size, 0};
        ul_decoder_feed(&dec, wire, n, check_round_trip, &rt);
        if (n == 0u || rt.found != 1) {
            abort();
        }
    }
    return 0;
}
