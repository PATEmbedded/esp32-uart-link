#include "ul/cobs.h"

size_t ul_cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t code_pos = 0; /* where the code byte of the current block goes */
    size_t write = 1;
    uint8_t code = 1; /* one more than the data bytes in the current block */

    for (size_t i = 0; i < len; ++i) {
        if (in[i] != 0u) {
            out[write++] = in[i];
            ++code;
        }
        if (in[i] == 0u || code == 0xFFu) {
            out[code_pos] = code;
            code = 1;
            if (in[i] != 0u && i + 1u == len) {
                /* The input ends with a full block of 254 bytes: no final code byte. */
                return write;
            }
            code_pos = write++;
        }
    }
    out[code_pos] = code;
    return write;
}

bool ul_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t *out_len)
{
    size_t read = 0;
    size_t write = 0;

    while (read < len) {
        const uint8_t code = in[read++];
        if (code == 0u) {
            return false;
        }
        for (uint8_t i = 1; i < code; ++i) {
            if (read == len || in[read] == 0u) {
                return false; /* the block runs past the end, or holds a zero */
            }
            out[write++] = in[read++];
        }
        /* A block shorter than 254 bytes stands for a zero, except at the very end. */
        if (code != 0xFFu && read < len) {
            out[write++] = 0;
        }
    }
    *out_len = write;
    return true;
}
