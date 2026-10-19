#include "ul/link.h"

#include <string.h>

void ul_link_init(ul_link_t *link, const ul_link_config_t *config)
{
    memset(link, 0, sizeof(*link));
    link->cfg = *config;
    ul_decoder_init(&link->decoder);
}

static void send_frame(ul_link_t *link, uint8_t kind, uint16_t session, uint8_t seq, const void *payload,
                       size_t len)
{
    uint8_t wire[UL_MAX_WIRE];
    const ul_frame_t frame = {kind, session, seq, (const uint8_t *)payload, len};
    const size_t n = ul_frame_encode(&frame, wire, sizeof(wire));
    link->cfg.write(link->cfg.ctx, wire, n);
}

ul_result_t ul_link_send(ul_link_t *link, const void *payload, size_t len, uint32_t now_ms)
{
    if (len > UL_MAX_PAYLOAD) {
        return UL_TOO_LONG;
    }
    if (link->in_flight) {
        return UL_BUSY;
    }
    const ul_frame_t frame = {UL_KIND_DATA, link->cfg.session, link->tx_seq, (const uint8_t *)payload, len};
    link->tx_len = ul_frame_encode(&frame, link->tx, sizeof(link->tx));
    link->in_flight = true;
    link->retries_left = link->cfg.max_retries;
    link->deadline_ms = now_ms + link->cfg.ack_timeout_ms;
    link->cfg.write(link->cfg.ctx, link->tx, link->tx_len);
    return UL_OK;
}

ul_result_t ul_link_send_datagram(ul_link_t *link, const void *payload, size_t len)
{
    if (len > UL_MAX_PAYLOAD) {
        return UL_TOO_LONG;
    }
    send_frame(link, UL_KIND_DATAGRAM, link->cfg.session, 0, payload, len);
    return UL_OK;
}

static void finish_send(ul_link_t *link, bool delivered)
{
    /* The next message gets a new number even after a failure: the peer may have received this
     * one and lost only the acknowledgement. */
    link->in_flight = false;
    link->tx_seq++;
    if (delivered) {
        link->stats.sent++;
    } else {
        link->stats.failed++;
    }
    if (link->cfg.on_sent != NULL) {
        link->cfg.on_sent(link->cfg.ctx, delivered);
    }
}

static bool deliver(ul_link_t *link, const ul_frame_t *frame, bool reliable)
{
    const bool accepted = link->cfg.on_message == NULL ||
                          link->cfg.on_message(link->cfg.ctx, frame->payload, frame->len, reliable);
    if (accepted) {
        link->stats.received++;
    } else {
        link->stats.refused++;
    }
    return accepted;
}

static void on_frame(void *ctx, const ul_frame_t *frame)
{
    ul_link_t *link = ctx;

    switch (frame->kind) {
    case UL_KIND_DATA:
        if (link->peer_known && frame->session == link->peer_session && frame->seq == link->peer_seq) {
            /* Delivered before; the acknowledgement of the first copy was lost, so send it again. */
            link->stats.duplicates++;
            send_frame(link, UL_KIND_ACK, frame->session, frame->seq, NULL, 0);
            return;
        }
        if (!deliver(link, frame, true)) {
            return; /* no acknowledgement: the sender repeats the message later */
        }
        link->peer_known = true;
        link->peer_session = frame->session;
        link->peer_seq = frame->seq;
        send_frame(link, UL_KIND_ACK, frame->session, frame->seq, NULL, 0);
        return;

    case UL_KIND_DATAGRAM:
        (void)deliver(link, frame, false);
        return;

    case UL_KIND_ACK:
        if (link->in_flight && frame->session == link->cfg.session && frame->seq == link->tx_seq &&
            frame->len == 0u) {
            finish_send(link, true);
        } else {
            link->stats.ignored++; /* late, repeated or not ours */
        }
        return;

    default:
        link->stats.ignored++; /* a kind from a newer peer */
        return;
    }
}

void ul_link_receive(ul_link_t *link, const uint8_t *data, size_t len, uint32_t now_ms)
{
    ul_decoder_feed(&link->decoder, data, len, on_frame, link);
    ul_link_poll(link, now_ms);
}

void ul_link_poll(ul_link_t *link, uint32_t now_ms)
{
    if (!link->in_flight || (int32_t)(now_ms - link->deadline_ms) < 0) {
        return;
    }
    if (link->retries_left == 0u) {
        finish_send(link, false);
        return;
    }
    link->retries_left--;
    link->stats.retransmits++;
    link->deadline_ms = now_ms + link->cfg.ack_timeout_ms;
    link->cfg.write(link->cfg.ctx, link->tx, link->tx_len);
}

bool ul_link_busy(const ul_link_t *link)
{
    return link->in_flight;
}

int32_t ul_link_time_to_deadline(const ul_link_t *link, uint32_t now_ms)
{
    if (!link->in_flight) {
        return -1;
    }
    const int32_t left = (int32_t)(link->deadline_ms - now_ms);
    return left > 0 ? left : 0;
}

ul_link_stats_t ul_link_stats(const ul_link_t *link)
{
    ul_link_stats_t stats = link->stats;
    stats.wire = link->decoder.stats;
    return stats;
}
