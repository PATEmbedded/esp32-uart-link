#ifndef UL_LINK_H
#define UL_LINK_H

/*
 * A message link over an in-order byte stream such as a UART, with no I/O of its own.
 *
 * The caller passes in received bytes and the time, and gets bytes to write and messages back
 * through callbacks. Reliable messages are sent one at a time (stop-and-wait): each DATA frame is
 * repeated until the peer acknowledges it or the retries run out, and the receiver drops repeats
 * it has already delivered. Datagrams are sent once and never acknowledged.
 *
 * Every start of a link picks a new random session number. A receiver that sees a new session
 * knows that the sender has restarted, so the first message after a restart is never mistaken for
 * a repeat of the last message before it.
 *
 * Not thread-safe: one caller, or one lock around all calls. Callbacks must not call
 * ul_link_receive(); they may call ul_link_send() and ul_link_send_datagram().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ul/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Writes bytes to the wire. It must not feed bytes back into ul_link_receive() in the same call. */
    void (*write)(void *ctx, const uint8_t *data, size_t len);
    /**
     * A message arrived; the payload is valid only during the call. Return false to refuse a
     * reliable message for now, for example because a queue is full: it is not acknowledged, and
     * the sender repeats it. For datagrams the return value only feeds the refused counter.
     */
    bool (*on_message)(void *ctx, const uint8_t *payload, size_t len, bool reliable);
    /** The reliable message in flight was acknowledged (delivered) or ran out of retries. May be NULL. */
    void (*on_sent)(void *ctx, bool delivered);
    void *ctx;
    uint16_t session;        /**< random, different on every start */
    uint32_t ack_timeout_ms; /**< wait this long for an acknowledgement before sending again */
    uint8_t max_retries;     /**< repeats after the first attempt */
} ul_link_config_t;

typedef struct {
    uint32_t sent;        /**< reliable messages acknowledged by the peer */
    uint32_t failed;      /**< reliable messages that ran out of retries */
    uint32_t retransmits; /**< DATA frames sent again */
    uint32_t received;    /**< messages on_message accepted */
    uint32_t refused;     /**< messages on_message refused */
    uint32_t duplicates;  /**< repeated DATA frames that had been delivered already */
    uint32_t ignored;     /**< acknowledgements for other frames, unknown frame kinds */
    ul_decoder_stats_t wire;
} ul_link_stats_t;

typedef enum {
    UL_OK = 0,
    UL_BUSY,     /**< a reliable message is still in flight */
    UL_TOO_LONG, /**< the payload is longer than UL_MAX_PAYLOAD */
} ul_result_t;

typedef struct {
    ul_link_config_t cfg;
    ul_decoder_t decoder;
    uint8_t tx[UL_MAX_WIRE]; /* the DATA frame in flight, kept for repeats */
    size_t tx_len;
    bool in_flight;
    uint8_t tx_seq; /* sequence number of the frame in flight, or of the next one */
    uint8_t retries_left;
    uint32_t deadline_ms;
    bool peer_known;
    uint16_t peer_session; /* of the last DATA frame delivered */
    uint8_t peer_seq;
    ul_link_stats_t stats;
} ul_link_t;

void ul_link_init(ul_link_t *link, const ul_link_config_t *config);

/** Starts sending a reliable message. on_sent reports the outcome. */
ul_result_t ul_link_send(ul_link_t *link, const void *payload, size_t len, uint32_t now_ms);

/** Sends a message once, without acknowledgement. Works while a reliable message is in flight. */
ul_result_t ul_link_send_datagram(ul_link_t *link, const void *payload, size_t len);

/** Passes in bytes from the wire, in pieces of any size. */
void ul_link_receive(ul_link_t *link, const uint8_t *data, size_t len, uint32_t now_ms);

/** Repeats or gives up on the message in flight once its deadline has passed. */
void ul_link_poll(ul_link_t *link, uint32_t now_ms);

bool ul_link_busy(const ul_link_t *link);

/** Milliseconds until ul_link_poll() is due (0: now), or -1 when nothing is in flight. */
int32_t ul_link_time_to_deadline(const ul_link_t *link, uint32_t now_ms);

/** Counters of the link and of the frame decoder. */
ul_link_stats_t ul_link_stats(const ul_link_t *link);

#ifdef __cplusplus
}
#endif

#endif
