/*
 * Two links joined by simulated wires that can drop frames, flip bits and lose acknowledgements.
 * Time runs in 1 ms steps; bytes written in one step arrive in the next, in chunks of random size.
 */
#include <stdio.h>
#include <string.h>

#include "ul/link.h"
#include "unity.h"

#define WIRE_SIZE 65536u
#define MAX_RECORDS 1024u
#define ACK_TIMEOUT_MS 20u

typedef struct {
    uint8_t data[WIRE_SIZE];
    size_t len;
    uint32_t drop_per_mille;    /* frames lost */
    uint32_t corrupt_per_mille; /* frames with one flipped bit */
    bool cut;                   /* everything lost */
    int acks_to_drop;
} wire_t;

typedef struct {
    size_t len;
    bool reliable;
    uint8_t data[64];
} record_t;

typedef struct {
    ul_link_t link;
    wire_t *out;
    record_t got[MAX_RECORDS];
    size_t got_count;
    int delivered;
    int failed;
    int chain_left;  /* messages to send from on_sent */
    bool echo;       /* answer every reliable message from on_message */
    int refuse_next; /* messages to refuse, as with a full queue */
} endpoint_t;

static wire_t s_ab;
static wire_t s_ba;
static endpoint_t s_a;
static endpoint_t s_b;
static uint32_t s_now;
static uint32_t s_rng;

static uint32_t next_random(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void note_kind(void *ctx, const ul_frame_t *frame)
{
    *(uint8_t *)ctx = frame->kind;
}

static uint8_t kind_of(const uint8_t *data, size_t len)
{
    static ul_decoder_t dec;
    uint8_t kind = 0;
    ul_decoder_init(&dec);
    ul_decoder_feed(&dec, data, len, note_kind, &kind);
    return kind;
}

static void on_write(void *ctx, const uint8_t *data, size_t len)
{
    wire_t *w = ((endpoint_t *)ctx)->out;
    if (w->cut) {
        return;
    }
    if (w->acks_to_drop > 0 && kind_of(data, len) == UL_KIND_ACK) {
        w->acks_to_drop--;
        return;
    }
    if (next_random() % 1000u < w->drop_per_mille) {
        return;
    }
    TEST_ASSERT_TRUE(w->len + len <= WIRE_SIZE);
    memcpy(&w->data[w->len], data, len);
    if (next_random() % 1000u < w->corrupt_per_mille) {
        w->data[w->len + next_random() % len] ^= (uint8_t)(1u << (next_random() % 8u));
    }
    w->len += len;
}

static bool on_message(void *ctx, const uint8_t *payload, size_t len, bool reliable)
{
    endpoint_t *ep = ctx;
    if (ep->refuse_next > 0) {
        ep->refuse_next--;
        return false;
    }
    TEST_ASSERT_TRUE(ep->got_count < MAX_RECORDS);
    TEST_ASSERT_TRUE(len <= sizeof(ep->got[0].data));
    record_t *r = &ep->got[ep->got_count++];
    r->len = len;
    r->reliable = reliable;
    if (len != 0u) {
        memcpy(r->data, payload, len);
    }
    if (ep->echo && reliable) {
        TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send_datagram(&ep->link, payload, len));
    }
    return true;
}

static void on_sent(void *ctx, bool delivered)
{
    endpoint_t *ep = ctx;
    if (delivered) {
        ep->delivered++;
    } else {
        ep->failed++;
    }
    if (ep->chain_left > 0) {
        ep->chain_left--;
        char text[16];
        const int n = snprintf(text, sizeof(text), "chain %d", ep->chain_left);
        TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&ep->link, text, (size_t)n, s_now));
    }
}

static void start(endpoint_t *ep, wire_t *out, uint16_t session, uint8_t max_retries)
{
    memset(ep, 0, sizeof(*ep));
    ep->out = out;
    const ul_link_config_t config = {
        .write = on_write,
        .on_message = on_message,
        .on_sent = on_sent,
        .ctx = ep,
        .session = session,
        .ack_timeout_ms = ACK_TIMEOUT_MS,
        .max_retries = max_retries,
    };
    ul_link_init(&ep->link, &config);
}

static void deliver(wire_t *w, endpoint_t *to)
{
    static uint8_t chunk[WIRE_SIZE];
    const size_t len = w->len;
    memcpy(chunk, w->data, len);
    w->len = 0;
    for (size_t at = 0; at < len;) {
        size_t n = 1u + next_random() % 64u;
        if (n > len - at) {
            n = len - at;
        }
        ul_link_receive(&to->link, &chunk[at], n, s_now);
        at += n;
    }
}

static void step(void)
{
    s_now++;
    deliver(&s_ab, &s_b);
    deliver(&s_ba, &s_a);
    ul_link_poll(&s_a.link, s_now);
    ul_link_poll(&s_b.link, s_now);
}

static void run_for(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; ++i) {
        step();
    }
}

static size_t make_payload(uint32_t index, uint8_t *out)
{
    const size_t len = (size_t)(index * 7u % 49u);
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)((index + i * 31u) % 5u == 0u ? 0u : index + i); /* zeros included */
    }
    return len;
}

/* Sends count messages from A to B, each as soon as the previous one is done. */
static void send_all(uint32_t count, uint32_t time_limit_ms)
{
    uint32_t next = 0;
    const uint32_t end = s_now + time_limit_ms;
    while ((next < count || ul_link_busy(&s_a.link)) && s_now < end) {
        if (next < count && !ul_link_busy(&s_a.link)) {
            uint8_t payload[64];
            const size_t len = make_payload(next, payload);
            TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, payload, len, s_now));
            next++;
        }
        step();
    }
    TEST_ASSERT_EQUAL_UINT32(count, next);
    TEST_ASSERT_FALSE(ul_link_busy(&s_a.link));
    run_for(5); /* let the last acknowledgement and any repeats arrive */
}

static void check_received_in_order(uint32_t count)
{
    TEST_ASSERT_EQUAL_size_t(count, s_b.got_count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t expected[64];
        const size_t len = make_payload(i, expected);
        TEST_ASSERT_TRUE(s_b.got[i].reliable);
        TEST_ASSERT_EQUAL_size_t(len, s_b.got[i].len);
        if (len != 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_b.got[i].data, len);
        }
    }
}

void setUp(void)
{
    memset(&s_ab, 0, sizeof(s_ab));
    memset(&s_ba, 0, sizeof(s_ba));
    s_now = 1000;
    s_rng = 0x9E3779B9u;
    start(&s_a, &s_ab, 0xA001, 5);
    start(&s_b, &s_ba, 0xB002, 5);
}

void tearDown(void) {}

static void test_delivers_in_order_over_a_clean_line(void)
{
    send_all(50, 10000);
    check_received_in_order(50);
    const ul_link_stats_t a = ul_link_stats(&s_a.link);
    const ul_link_stats_t b = ul_link_stats(&s_b.link);
    TEST_ASSERT_EQUAL_INT(50, s_a.delivered);
    TEST_ASSERT_EQUAL_UINT32(50, a.sent);
    TEST_ASSERT_EQUAL_UINT32(0, a.retransmits);
    TEST_ASSERT_EQUAL_UINT32(0, b.duplicates);
    TEST_ASSERT_EQUAL_UINT32(50, b.wire.frames);
}

static void test_delivers_exactly_once_over_a_lossy_line(void)
{
    start(&s_a, &s_ab, 0xA001, 100); /* enough retries never to give up */
    s_ab.drop_per_mille = s_ba.drop_per_mille = 200;
    s_ab.corrupt_per_mille = s_ba.corrupt_per_mille = 50;

    send_all(300, 600000);
    check_received_in_order(300);
    const ul_link_stats_t a = ul_link_stats(&s_a.link);
    const ul_link_stats_t b = ul_link_stats(&s_b.link);
    TEST_ASSERT_EQUAL_INT(300, s_a.delivered);
    TEST_ASSERT_EQUAL_INT(0, s_a.failed);
    TEST_ASSERT_TRUE(a.retransmits > 0u);
    TEST_ASSERT_TRUE(b.duplicates > 0u); /* some acknowledgements were lost */
    TEST_ASSERT_TRUE(b.wire.crc_errors + b.wire.format_errors > 0u);
    TEST_ASSERT_TRUE(a.wire.crc_errors + a.wire.format_errors > 0u);
}

static void test_gives_up_when_the_line_is_cut(void)
{
    start(&s_a, &s_ab, 0xA001, 3);
    s_ab.cut = true;
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "lost", 4, s_now));
    run_for(4u * ACK_TIMEOUT_MS + 5u);
    TEST_ASSERT_EQUAL_INT(1, s_a.failed);
    TEST_ASSERT_FALSE(ul_link_busy(&s_a.link));
    TEST_ASSERT_EQUAL_INT32(-1, ul_link_time_to_deadline(&s_a.link, s_now));
    const ul_link_stats_t a = ul_link_stats(&s_a.link);
    TEST_ASSERT_EQUAL_UINT32(3, a.retransmits);
    TEST_ASSERT_EQUAL_UINT32(1, a.failed);

    s_ab.cut = false;
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "back", 4, s_now));
    run_for(10);
    TEST_ASSERT_EQUAL_INT(1, s_a.delivered);
    TEST_ASSERT_EQUAL_size_t(1, s_b.got_count);
    TEST_ASSERT_EQUAL_MEMORY("back", s_b.got[0].data, 4);
}

static void test_a_lost_acknowledgement_does_not_duplicate_the_message(void)
{
    s_ba.acks_to_drop = 1;
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "once", 4, s_now));
    run_for(2u * ACK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(1, s_a.delivered);
    TEST_ASSERT_EQUAL_size_t(1, s_b.got_count);
    TEST_ASSERT_EQUAL_UINT32(1, ul_link_stats(&s_a.link).retransmits);
    TEST_ASSERT_EQUAL_UINT32(1, ul_link_stats(&s_b.link).duplicates);
}

static void test_accepts_the_first_message_after_the_sender_restarts(void)
{
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "before", 6, s_now));
    run_for(5);
    start(&s_a, &s_ab, 0xA002, 5); /* a restart picks a new session; the sequence starts again at 0 */
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "after", 5, s_now));
    run_for(5);
    TEST_ASSERT_EQUAL_size_t(2, s_b.got_count);
    TEST_ASSERT_EQUAL_MEMORY("after", s_b.got[1].data, 5);
}

static void test_a_restart_with_the_same_session_looks_like_a_repeat(void)
{
    /* Why the session must be random: the same session and sequence number mean "seen already". */
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "before", 6, s_now));
    run_for(5);
    start(&s_a, &s_ab, 0xA001, 5);
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "after", 5, s_now));
    run_for(5);
    TEST_ASSERT_EQUAL_size_t(1, s_b.got_count);
    TEST_ASSERT_EQUAL_UINT32(1, ul_link_stats(&s_b.link).duplicates);
    TEST_ASSERT_EQUAL_INT(1, s_a.delivered); /* acknowledged all the same */
}

static void test_datagrams_are_neither_acknowledged_nor_repeated(void)
{
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send_datagram(&s_a.link, "fire", 4));
    TEST_ASSERT_FALSE(ul_link_busy(&s_a.link));
    s_now++;
    deliver(&s_ab, &s_b);
    TEST_ASSERT_EQUAL_size_t(1, s_b.got_count);
    TEST_ASSERT_FALSE(s_b.got[0].reliable);
    TEST_ASSERT_EQUAL_size_t(0, s_ba.len); /* no acknowledgement */
    run_for(100);
    TEST_ASSERT_EQUAL_size_t(1, s_b.got_count);
}

static void test_reports_busy_and_too_long(void)
{
    static uint8_t big[UL_MAX_PAYLOAD + 1u];
    TEST_ASSERT_EQUAL_INT(UL_TOO_LONG, ul_link_send(&s_a.link, big, sizeof(big), s_now));
    TEST_ASSERT_EQUAL_INT(UL_TOO_LONG, ul_link_send_datagram(&s_a.link, big, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, big, UL_MAX_PAYLOAD, s_now));
    TEST_ASSERT_EQUAL_INT(UL_BUSY, ul_link_send(&s_a.link, "x", 1, s_now));
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send_datagram(&s_a.link, "x", 1)); /* datagrams still go */
}

static void test_ignores_acknowledgements_that_do_not_match(void)
{
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "wait", 4, s_now));
    uint8_t wire[UL_MAX_WIRE];
    const ul_frame_t wrong_seq = {UL_KIND_ACK, 0xA001, 1, NULL, 0};
    const ul_frame_t wrong_session = {UL_KIND_ACK, 0xA00F, 0, NULL, 0};
    const ul_frame_t unknown_kind = {9, 0xA001, 0, NULL, 0};
    size_t n = ul_frame_encode(&wrong_seq, wire, sizeof(wire));
    ul_link_receive(&s_a.link, wire, n, s_now);
    n = ul_frame_encode(&wrong_session, wire, sizeof(wire));
    ul_link_receive(&s_a.link, wire, n, s_now);
    n = ul_frame_encode(&unknown_kind, wire, sizeof(wire));
    ul_link_receive(&s_a.link, wire, n, s_now);
    TEST_ASSERT_TRUE(ul_link_busy(&s_a.link));
    TEST_ASSERT_EQUAL_UINT32(3, ul_link_stats(&s_a.link).ignored);

    const ul_frame_t right = {UL_KIND_ACK, 0xA001, 0, NULL, 0};
    n = ul_frame_encode(&right, wire, sizeof(wire));
    ul_link_receive(&s_a.link, wire, n, s_now);
    TEST_ASSERT_FALSE(ul_link_busy(&s_a.link));
    TEST_ASSERT_EQUAL_INT(1, s_a.delivered);
}

static void test_reports_the_time_to_the_deadline(void)
{
    TEST_ASSERT_EQUAL_INT32(-1, ul_link_time_to_deadline(&s_a.link, s_now));
    s_ab.cut = true;
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "t", 1, 100));
    TEST_ASSERT_EQUAL_INT32(15, ul_link_time_to_deadline(&s_a.link, 105));
    TEST_ASSERT_EQUAL_INT32(0, ul_link_time_to_deadline(&s_a.link, 130));
}

static void test_a_refused_message_is_repeated_later(void)
{
    s_b.refuse_next = 2;
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "full", 4, s_now));
    run_for(3u * ACK_TIMEOUT_MS + 5u);
    TEST_ASSERT_EQUAL_INT(1, s_a.delivered);
    TEST_ASSERT_EQUAL_size_t(1, s_b.got_count);
    const ul_link_stats_t b = ul_link_stats(&s_b.link);
    TEST_ASSERT_EQUAL_UINT32(2, b.refused);
    TEST_ASSERT_EQUAL_UINT32(1, b.received);
    TEST_ASSERT_EQUAL_UINT32(0, b.duplicates);
    TEST_ASSERT_EQUAL_UINT32(2, ul_link_stats(&s_a.link).retransmits);
}

static void test_on_sent_may_send_the_next_message(void)
{
    s_a.chain_left = 19;
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "first", 5, s_now));
    run_for(200);
    TEST_ASSERT_EQUAL_INT(20, s_a.delivered);
    TEST_ASSERT_EQUAL_size_t(20, s_b.got_count);
    TEST_ASSERT_EQUAL_MEMORY("chain 0", s_b.got[19].data, 7);
}

static void test_on_message_may_answer(void)
{
    s_b.echo = true;
    TEST_ASSERT_EQUAL_INT(UL_OK, ul_link_send(&s_a.link, "ping", 4, s_now));
    run_for(5);
    TEST_ASSERT_EQUAL_INT(1, s_a.delivered);
    TEST_ASSERT_EQUAL_size_t(1, s_a.got_count);
    TEST_ASSERT_FALSE(s_a.got[0].reliable);
    TEST_ASSERT_EQUAL_MEMORY("ping", s_a.got[0].data, 4);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_delivers_in_order_over_a_clean_line);
    RUN_TEST(test_delivers_exactly_once_over_a_lossy_line);
    RUN_TEST(test_gives_up_when_the_line_is_cut);
    RUN_TEST(test_a_lost_acknowledgement_does_not_duplicate_the_message);
    RUN_TEST(test_accepts_the_first_message_after_the_sender_restarts);
    RUN_TEST(test_a_restart_with_the_same_session_looks_like_a_repeat);
    RUN_TEST(test_datagrams_are_neither_acknowledged_nor_repeated);
    RUN_TEST(test_reports_busy_and_too_long);
    RUN_TEST(test_ignores_acknowledgements_that_do_not_match);
    RUN_TEST(test_reports_the_time_to_the_deadline);
    RUN_TEST(test_a_refused_message_is_repeated_later);
    RUN_TEST(test_on_sent_may_send_the_next_message);
    RUN_TEST(test_on_message_may_answer);
    return UNITY_END();
}
