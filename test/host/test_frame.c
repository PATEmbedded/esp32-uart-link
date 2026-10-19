#include <string.h>

#include "ul/frame.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    int count;
    ul_frame_t last;
    uint8_t payload[UL_MAX_PAYLOAD];
} capture_t;

static void capture(void *ctx, const ul_frame_t *frame)
{
    capture_t *c = ctx;
    c->count++;
    c->last = *frame;
    TEST_ASSERT_TRUE(frame->len <= UL_MAX_PAYLOAD);
    if (frame->len != 0u) {
        memcpy(c->payload, frame->payload, frame->len);
    }
    c->last.payload = c->payload;
}

static size_t encode(uint8_t kind, uint16_t session, uint8_t seq, const void *payload, size_t len, uint8_t *out)
{
    const ul_frame_t frame = {kind, session, seq, payload, len};
    return ul_frame_encode(&frame, out, UL_MAX_WIRE);
}

static void test_round_trip(void)
{
    const char text[] = "hello, link";
    uint8_t wire[UL_MAX_WIRE];
    const size_t n = encode(UL_KIND_DATA, 0xBEEF, 7, text, sizeof(text), wire);
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[n - 1u]);
    TEST_ASSERT_NULL(memchr(wire, 0, n - 1u));

    ul_decoder_t dec;
    ul_decoder_init(&dec);
    capture_t got = {0};
    ul_decoder_feed(&dec, wire, n, capture, &got);
    TEST_ASSERT_EQUAL_INT(1, got.count);
    TEST_ASSERT_EQUAL_UINT8(UL_KIND_DATA, got.last.kind);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, got.last.session);
    TEST_ASSERT_EQUAL_UINT8(7, got.last.seq);
    TEST_ASSERT_EQUAL_size_t(sizeof(text), got.last.len);
    TEST_ASSERT_EQUAL_STRING(text, (const char *)got.payload);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.frames);
}

/* A frame written out by hand, so the layout cannot drift without this test noticing. */
static void test_wire_layout(void)
{
    uint8_t wire[UL_MAX_WIRE];
    const size_t n = encode(UL_KIND_ACK, 0x1234, 0x56, NULL, 0, wire);
    /* raw: 02 34 12 56 crc_lo crc_hi; CRC-16/CCITT-FALSE of 02 34 12 56 is 0x2FEF */
    const uint8_t expected[] = {0x07, 0x02, 0x34, 0x12, 0x56, 0xEF, 0x2F, 0x00};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, wire, sizeof(expected));
}

static void test_empty_and_largest_payloads(void)
{
    static uint8_t payload[UL_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i % 7u == 0u ? 0u : i); /* zeros spread through the payload */
    }
    ul_decoder_t dec;
    ul_decoder_init(&dec);
    capture_t got = {0};
    uint8_t wire[UL_MAX_WIRE];

    size_t n = encode(UL_KIND_DATAGRAM, 1, 0, NULL, 0, wire);
    ul_decoder_feed(&dec, wire, n, capture, &got);
    TEST_ASSERT_EQUAL_INT(1, got.count);
    TEST_ASSERT_EQUAL_size_t(0, got.last.len);

    n = encode(UL_KIND_DATAGRAM, 1, 0, payload, sizeof(payload), wire);
    TEST_ASSERT_TRUE(n <= UL_MAX_WIRE);
    ul_decoder_feed(&dec, wire, n, capture, &got);
    TEST_ASSERT_EQUAL_INT(2, got.count);
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), got.last.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, got.payload, sizeof(payload));
}

static void test_rejects_oversized_payloads_and_small_buffers(void)
{
    static uint8_t payload[UL_MAX_PAYLOAD + 1u];
    uint8_t wire[UL_MAX_WIRE];
    TEST_ASSERT_EQUAL_size_t(0, encode(UL_KIND_DATA, 1, 0, payload, sizeof(payload), wire));

    const ul_frame_t frame = {UL_KIND_DATA, 1, 0, payload, 10};
    TEST_ASSERT_EQUAL_size_t(0, ul_frame_encode(&frame, wire, 12));
}

static void test_a_flipped_bit_is_a_crc_error(void)
{
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t wire[UL_MAX_WIRE];
    const size_t n = encode(UL_KIND_DATA, 0x0101, 1, payload, sizeof(payload), wire);

    for (size_t byte = 0; byte + 1u < n; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t bad[UL_MAX_WIRE];
            memcpy(bad, wire, n);
            bad[byte] ^= (uint8_t)(1u << bit);
            ul_decoder_t dec;
            ul_decoder_init(&dec);
            capture_t got = {0};
            ul_decoder_feed(&dec, bad, n, capture, &got);
            /* A flip may turn a byte into a delimiter and split the frame; nothing gets through either way. */
            TEST_ASSERT_EQUAL_INT(0, got.count);
            TEST_ASSERT_TRUE(dec.stats.crc_errors + dec.stats.format_errors >= 1u);
        }
    }
}

static void test_resynchronises_after_garbage(void)
{
    const uint8_t garbage[] = {0x41, 0x42, 0x43, 0xFF, 0x13, 0x00, 0x99, 0x98};
    const uint8_t payload[] = {0xAA, 0x00, 0xBB};
    uint8_t wire[UL_MAX_WIRE];
    const size_t n = encode(UL_KIND_DATAGRAM, 9, 0, payload, sizeof(payload), wire);

    ul_decoder_t dec;
    ul_decoder_init(&dec);
    capture_t got = {0};
    ul_decoder_feed(&dec, garbage, sizeof(garbage), capture, &got);
    /* The garbage after the last zero merges with the next frame, which is lost; the one after it gets through. */
    ul_decoder_feed(&dec, wire, n, capture, &got);
    ul_decoder_feed(&dec, wire, n, capture, &got);
    TEST_ASSERT_EQUAL_INT(1, got.count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, got.payload, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(2, dec.stats.crc_errors + dec.stats.format_errors);
}

static void test_drops_frames_that_are_too_long(void)
{
    static uint8_t junk[UL_MAX_WIRE + 50u];
    memset(junk, 0x55, sizeof(junk));
    const uint8_t payload[] = {1, 2, 3};
    uint8_t wire[UL_MAX_WIRE];
    const size_t n = encode(UL_KIND_DATAGRAM, 9, 0, payload, sizeof(payload), wire);

    ul_decoder_t dec;
    ul_decoder_init(&dec);
    capture_t got = {0};
    ul_decoder_feed(&dec, junk, sizeof(junk), capture, &got);
    ul_decoder_feed(&dec, (const uint8_t[]){0x00}, 1, capture, &got);
    ul_decoder_feed(&dec, wire, n, capture, &got);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.overruns);
    TEST_ASSERT_EQUAL_INT(1, got.count);
}

static void test_rejects_frames_that_decode_too_long(void)
{
    /* Valid COBS that fits the receive buffer but decodes to more than the largest frame. */
    static uint8_t ones[UL_COBS_MAX_ENCODED(UL_MAX_FRAME)];
    memset(ones, 0x01, sizeof(ones));
    ul_decoder_t dec;
    ul_decoder_init(&dec);
    capture_t got = {0};
    ul_decoder_feed(&dec, ones, sizeof(ones), capture, &got);
    ul_decoder_feed(&dec, (const uint8_t[]){0x00}, 1, capture, &got);
    TEST_ASSERT_EQUAL_INT(0, got.count);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.format_errors);
}

static void test_ignores_idle_delimiters_and_short_frames(void)
{
    ul_decoder_t dec;
    ul_decoder_init(&dec);
    capture_t got = {0};
    ul_decoder_feed(&dec, (const uint8_t[]){0x00, 0x00, 0x00}, 3, capture, &got);
    TEST_ASSERT_EQUAL_UINT32(0, dec.stats.format_errors);
    ul_decoder_feed(&dec, (const uint8_t[]){0x03, 0x01, 0x02, 0x00}, 4, capture, &got); /* two bytes only */
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.format_errors);
    TEST_ASSERT_EQUAL_INT(0, got.count);
}

static void test_accepts_bytes_one_at_a_time(void)
{
    const char text[] = "split across many reads";
    uint8_t wire[UL_MAX_WIRE];
    const size_t n = encode(UL_KIND_DATA, 3, 3, text, sizeof(text), wire);
    ul_decoder_t dec;
    ul_decoder_init(&dec);
    capture_t got = {0};
    for (size_t i = 0; i < n; ++i) {
        ul_decoder_feed(&dec, &wire[i], 1, capture, &got);
        TEST_ASSERT_EQUAL_INT(i + 1u == n ? 1 : 0, got.count);
    }
    TEST_ASSERT_EQUAL_STRING(text, (const char *)got.payload);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip);
    RUN_TEST(test_wire_layout);
    RUN_TEST(test_empty_and_largest_payloads);
    RUN_TEST(test_rejects_oversized_payloads_and_small_buffers);
    RUN_TEST(test_a_flipped_bit_is_a_crc_error);
    RUN_TEST(test_resynchronises_after_garbage);
    RUN_TEST(test_drops_frames_that_are_too_long);
    RUN_TEST(test_rejects_frames_that_decode_too_long);
    RUN_TEST(test_ignores_idle_delimiters_and_short_frames);
    RUN_TEST(test_accepts_bytes_one_at_a_time);
    return UNITY_END();
}
