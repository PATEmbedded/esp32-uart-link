/*
 * Encodes and decodes frames from the command line, so the Python implementation in
 * tools/uart_link.py can be checked against the C core byte for byte.
 *
 *   ul_tool encode KIND SESSION SEQ PAYLOAD_HEX   prints the frame as it goes on the wire
 *   ul_tool decode WIRE_HEX                       prints every frame found, then the counters
 *
 * Hex strings have no separators; "-" stands for no bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ul/frame.h"

static size_t parse_hex(const char *text, uint8_t *out, size_t out_size)
{
    if (strcmp(text, "-") == 0) {
        return 0;
    }
    const size_t len = strlen(text);
    if (len % 2u != 0u || len / 2u > out_size) {
        fprintf(stderr, "bad hex string\n");
        exit(2);
    }
    for (size_t i = 0; i < len / 2u; ++i) {
        unsigned int byte = 0;
        if (sscanf(&text[2u * i], "%2x", &byte) != 1) {
            fprintf(stderr, "bad hex string\n");
            exit(2);
        }
        out[i] = (uint8_t)byte;
    }
    return len / 2u;
}

static void print_hex(const uint8_t *data, size_t len)
{
    if (len == 0u) {
        fputs("-", stdout);
    }
    for (size_t i = 0; i < len; ++i) {
        printf("%02x", data[i]);
    }
}

static void print_frame(void *ctx, const ul_frame_t *frame)
{
    (void)ctx;
    printf("frame %u %u %u ", (unsigned)frame->kind, (unsigned)frame->session, (unsigned)frame->seq);
    print_hex(frame->payload, frame->len);
    putchar('\n');
}

int main(int argc, char **argv)
{
    static uint8_t input[1u << 16];

    if (argc == 6 && strcmp(argv[1], "encode") == 0) {
        uint8_t payload[UL_MAX_PAYLOAD + 1u];
        const ul_frame_t frame = {
            .kind = (uint8_t)strtoul(argv[2], NULL, 0),
            .session = (uint16_t)strtoul(argv[3], NULL, 0),
            .seq = (uint8_t)strtoul(argv[4], NULL, 0),
            .payload = payload,
            .len = parse_hex(argv[5], payload, sizeof(payload)),
        };
        uint8_t wire[UL_MAX_WIRE];
        const size_t n = ul_frame_encode(&frame, wire, sizeof(wire));
        if (n == 0u) {
            fprintf(stderr, "payload too long\n");
            return 1;
        }
        print_hex(wire, n);
        putchar('\n');
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "decode") == 0) {
        const size_t len = parse_hex(argv[2], input, sizeof(input));
        static ul_decoder_t dec;
        ul_decoder_init(&dec);
        ul_decoder_feed(&dec, input, len, print_frame, NULL);
        printf("stats %u %u %u %u\n", (unsigned)dec.stats.frames, (unsigned)dec.stats.crc_errors,
               (unsigned)dec.stats.format_errors, (unsigned)dec.stats.overruns);
        return 0;
    }
    fprintf(stderr, "usage: ul_tool encode KIND SESSION SEQ PAYLOAD_HEX | ul_tool decode WIRE_HEX\n");
    return 2;
}
