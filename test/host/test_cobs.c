#include <string.h>

#include "ul/cobs.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void check_vector(const uint8_t *raw, size_t raw_len, const uint8_t *encoded, size_t encoded_len)
{
    uint8_t out[600];
    TEST_ASSERT_EQUAL_size_t(encoded_len, ul_cobs_encode(raw, raw_len, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(encoded, out, encoded_len);

    uint8_t back[600];
    size_t back_len = 0;
    TEST_ASSERT_TRUE(ul_cobs_decode(encoded, encoded_len, back, &back_len));
    TEST_ASSERT_EQUAL_size_t(raw_len, back_len);
    if (raw_len != 0u) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, back, raw_len);
    }
}

/* The short examples from the COBS article on Wikipedia. */
static void test_short_vectors(void)
{
    check_vector(NULL, 0, (const uint8_t[]){0x01}, 1);
    check_vector((const uint8_t[]){0x00}, 1, (const uint8_t[]){0x01, 0x01}, 2);
    check_vector((const uint8_t[]){0x00, 0x00}, 2, (const uint8_t[]){0x01, 0x01, 0x01}, 3);
    check_vector((const uint8_t[]){0x00, 0x11, 0x00}, 3, (const uint8_t[]){0x01, 0x02, 0x11, 0x01}, 4);
    check_vector((const uint8_t[]){0x11, 0x22, 0x00, 0x33}, 4, (const uint8_t[]){0x03, 0x11, 0x22, 0x02, 0x33}, 5);
    check_vector((const uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4, (const uint8_t[]){0x05, 0x11, 0x22, 0x33, 0x44}, 5);
    check_vector((const uint8_t[]){0x11, 0x00, 0x00, 0x00}, 4, (const uint8_t[]){0x02, 0x11, 0x01, 0x01, 0x01}, 5);
}

/* The long examples from the same article, around the 254-byte block limit. */
static void test_block_limit_vectors(void)
{
    uint8_t raw[256];
    uint8_t enc[260];

    /* 01 02 ... FE -> FF 01 02 ... FE */
    for (size_t i = 0; i < 254; ++i) {
        raw[i] = (uint8_t)(i + 1);
    }
    enc[0] = 0xFF;
    memcpy(&enc[1], raw, 254);
    check_vector(raw, 254, enc, 255);

    /* 00 01 ... FE -> 01 FF 01 ... FE */
    raw[0] = 0x00;
    for (size_t i = 1; i < 255; ++i) {
        raw[i] = (uint8_t)i;
    }
    enc[0] = 0x01;
    enc[1] = 0xFF;
    memcpy(&enc[2], &raw[1], 254);
    check_vector(raw, 255, enc, 256);

    /* 01 02 ... FF -> FF 01 ... FE 02 FF */
    for (size_t i = 0; i < 255; ++i) {
        raw[i] = (uint8_t)(i + 1);
    }
    enc[0] = 0xFF;
    memcpy(&enc[1], raw, 254);
    enc[255] = 0x02;
    enc[256] = 0xFF;
    check_vector(raw, 255, enc, 257);

    /* 02 03 ... FF 00 -> FF 02 ... FF 01 01 */
    for (size_t i = 0; i < 254; ++i) {
        raw[i] = (uint8_t)(i + 2);
    }
    raw[254] = 0x00;
    enc[0] = 0xFF;
    memcpy(&enc[1], raw, 254);
    enc[255] = 0x01;
    enc[256] = 0x01;
    check_vector(raw, 255, enc, 257);

    /* 03 04 ... FF 00 01 -> FE 03 ... FF 02 01 */
    for (size_t i = 0; i < 253; ++i) {
        raw[i] = (uint8_t)(i + 3);
    }
    raw[253] = 0x00;
    raw[254] = 0x01;
    enc[0] = 0xFE;
    memcpy(&enc[1], raw, 253);
    enc[254] = 0x02;
    enc[255] = 0x01;
    check_vector(raw, 255, enc, 256);
}

static uint32_t s_rng = 12345u;
static uint32_t next_random(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void test_round_trip_of_random_data(void)
{
    uint8_t raw[600];
    uint8_t enc[UL_COBS_MAX_ENCODED(600u)];
    uint8_t back[sizeof(enc)];

    for (int round = 0; round < 2000; ++round) {
        const size_t len = next_random() % sizeof(raw);
        const uint32_t zero_odds = 1u + next_random() % 64u; /* from many zeros to almost none */
        for (size_t i = 0; i < len; ++i) {
            raw[i] = (next_random() % zero_odds == 0u) ? 0u : (uint8_t)(1u + next_random() % 255u);
        }
        const size_t n = ul_cobs_encode(raw, len, enc);
        TEST_ASSERT_TRUE(n <= UL_COBS_MAX_ENCODED(len));
        TEST_ASSERT_NULL(memchr(enc, 0, n));

        /* Decode in place, as the frame decoder does. */
        memcpy(back, enc, n);
        size_t back_len = 0;
        TEST_ASSERT_TRUE(ul_cobs_decode(back, n, back, &back_len));
        TEST_ASSERT_EQUAL_size_t(len, back_len);
        if (len != 0u) {
            TEST_ASSERT_EQUAL_MEMORY(raw, back, len);
        }
    }
}

static void test_rejects_invalid_input(void)
{
    uint8_t out[8];
    size_t len = 0;
    TEST_ASSERT_FALSE(ul_cobs_decode((const uint8_t[]){0x00}, 1, out, &len));             /* zero code */
    TEST_ASSERT_FALSE(ul_cobs_decode((const uint8_t[]){0x03, 0x11}, 2, out, &len));       /* block too short */
    TEST_ASSERT_FALSE(ul_cobs_decode((const uint8_t[]){0x03, 0x11, 0x00}, 3, out, &len)); /* zero in a block */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_vectors);
    RUN_TEST(test_block_limit_vectors);
    RUN_TEST(test_round_trip_of_random_data);
    RUN_TEST(test_rejects_invalid_input);
    return UNITY_END();
}
