/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/* WebAssembly SIMD128 version of unpack_data_c(). Bit-exact with the C version,
 * including its error handling (per-group check of precinct_bits_left).
 *
 * A code group stores one nibble per bit plane (MSB plane first); bit 3 of a
 * nibble belongs to the group's first coefficient, bit 0 to the last. With
 * sign_flag == 0 the group's sign nibble precedes the bit planes. The kernel
 * loads a window of 16 nibbles, spreads them into the high halves of byte
 * lanes in reverse order and collects each coefficient's bits with
 * i8x16.bitmask, instead of looping over the bit planes. One window serves as
 * many consecutive groups as fit into it (groups below gtli take no nibbles);
 * each group's 4 outputs are then built with vector shifts and stored at
 * once. */

#ifdef __wasm_simd128__

#include <string.h>
#include <wasm_simd128.h>
#include "UnPack_wasm_simd128.h"
#include "Packing.h"
#include "Codestream.h"
#include "Definitions.h"

typedef struct nibble_reader {
    const uint8_t* mem; /* start of the bitstream buffer */
    uint64_t nibble; /* position in nibbles, relative to mem */
    uint64_t size; /* buffer size in bytes */
} nibble_reader_t;

/* Reads n nibbles (1..16) and returns the bit columns: cols[k] holds bit
 * (3 - k) of each nibble, the first nibble in the most significant position. */
static FORCE_INLINE void read_columns(nibble_reader_t* r, uint32_t n, uint32_t cols[4]) {
    const uint64_t byte = r->nibble >> 1;
    const uint8_t* p = r->mem + byte;
    const uint32_t odd = (uint32_t)(r->nibble & 1);
    r->nibble += n;
    /* The 8-byte load must stay inside the buffer. */
    if (n + odd <= 16 && byte + 8 <= r->size) {
        const v128_t bytes = wasm_v128_load64_zero(p);
        /* High nibbles in place; low nibbles moved up (the low halves of the
         * lanes get garbage, which is never shifted into bit 7 below). */
        const v128_t hi = wasm_v128_and(bytes, wasm_i8x16_splat((int8_t)0xF0));
        const v128_t lo = wasm_i16x8_shl(bytes, 4);
        /* Nibble j (hi of byte 0 is j = 0) goes to lane 15 - j. */
        v128_t v = wasm_i8x16_shuffle(hi, lo, 23, 7, 22, 6, 21, 5, 20, 4, 19, 3, 18, 2, 17, 1, 16, 0);
        const uint32_t shift = 16 - (n + odd);
        const uint32_t mask = (1u << n) - 1;
        cols[0] = (wasm_i8x16_bitmask(v) >> shift) & mask;
        v = wasm_i8x16_add(v, v);
        cols[1] = (wasm_i8x16_bitmask(v) >> shift) & mask;
        v = wasm_i8x16_add(v, v);
        cols[2] = (wasm_i8x16_bitmask(v) >> shift) & mask;
        v = wasm_i8x16_add(v, v);
        cols[3] = (wasm_i8x16_bitmask(v) >> shift) & mask;
        return;
    }
    /* Near the end of the buffer or more than 16 nibbles: scalar. */
    uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    uint32_t nibble = odd;
    for (uint32_t i = 0; i < n; i++, nibble++) {
        const uint32_t val = (p[nibble >> 1] >> ((~nibble & 1) << 2)) & 0xF;
        c0 = (c0 << 1) | (val >> 3);
        c1 = (c1 << 1) | ((val >> 2) & 1);
        c2 = (c2 << 1) | ((val >> 1) & 1);
        c3 = (c3 << 1) | (val & 1);
    }
    cols[0] = c0;
    cols[1] = c1;
    cols[2] = c2;
    cols[3] = c3;
}

/* Bit columns of the 16 nibbles starting at p (nibble 0 = high half of
 * p[0]): lane k holds bit (3 - k) of each nibble, nibble 0 in bit 15. */
static FORCE_INLINE v128_t window_columns(const uint8_t* p) {
    const v128_t bytes = wasm_v128_load64_zero(p);
    const v128_t hi = wasm_v128_and(bytes, wasm_i8x16_splat((int8_t)0xF0));
    const v128_t lo = wasm_i16x8_shl(bytes, 4);
    v128_t v = wasm_i8x16_shuffle(hi, lo, 23, 7, 22, 6, 21, 5, 20, 4, 19, 3, 18, 2, 17, 1, 16, 0);
    const uint32_t m0 = wasm_i8x16_bitmask(v);
    v = wasm_i8x16_add(v, v);
    const uint32_t m1 = wasm_i8x16_bitmask(v);
    v = wasm_i8x16_add(v, v);
    const uint32_t m2 = wasm_i8x16_bitmask(v);
    v = wasm_i8x16_add(v, v);
    const uint32_t m3 = wasm_i8x16_bitmask(v);
    return wasm_i32x4_make((int32_t)m0, (int32_t)m1, (int32_t)m2, (int32_t)m3);
}

/* Stores a group's 4 outputs; c holds its bits at the bottom of each lane:
 * the sign bit (if with_sign) above `bitplanes` magnitude bits. */
static FORCE_INLINE void store_group(uint16_t* out, v128_t c, int32_t bitplanes, uint8_t gtli, int with_sign) {
    v128_t v = wasm_i32x4_shl(wasm_v128_and(c, wasm_i32x4_splat((int32_t)((1u << bitplanes) - 1))), gtli);
    if (with_sign) {
        const v128_t sign = wasm_v128_and(wasm_u32x4_shr(c, bitplanes), wasm_i32x4_splat(1));
        v = wasm_v128_or(v, wasm_i32x4_shl(sign, BITSTREAM_BIT_POSITION_SIGN));
    }
    /* Truncate to 16 bits like the C version's uint16_t casts. */
    wasm_v128_store64_lane(out, wasm_i8x16_shuffle(v, v, 0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13), 0);
}

/* One group: bit planes (and the sign nibble before them if with_sign). */
static FORCE_INLINE void unpack_group(nibble_reader_t* r, uint16_t* out, int32_t bitplanes, uint8_t gtli, int with_sign) {
    uint32_t cols[4];
    if (with_sign && bitplanes < 16) {
        /* Sign nibble and bit planes in one go: the sign is the top bit. */
        read_columns(r, (uint32_t)bitplanes + 1, cols);
        const uint32_t mag_mask = (1u << bitplanes) - 1;
        for (int k = 0; k < 4; k++) {
            const uint16_t sign = (uint16_t)((cols[k] >> bitplanes) << BITSTREAM_BIT_POSITION_SIGN);
            out[k] = (uint16_t)((cols[k] & mag_mask) << gtli) | sign;
        }
    }
    else if (with_sign) {
        /* Only the low 16 bits of a magnitude survive the uint16_t cast, so
         * 32-bit columns give the same result as the C version. */
        uint32_t signs[4];
        read_columns(r, 1, signs);
        read_columns(r, (uint32_t)bitplanes, cols);
        for (int k = 0; k < 4; k++) {
            out[k] = (uint16_t)(cols[k] << gtli) | (uint16_t)(signs[k] << BITSTREAM_BIT_POSITION_SIGN);
        }
    }
    else {
        read_columns(r, (uint32_t)bitplanes, cols);
        for (int k = 0; k < 4; k++) {
            out[k] = (uint16_t)(cols[k] << gtli);
        }
    }
}

SvtJxsErrorType_t unpack_data_wasm_simd128(bitstream_reader_t* bitstream, uint16_t* buf, uint32_t w, uint8_t* gclis,
                                           uint32_t group_size, uint8_t gtli, uint8_t sign_flag, uint8_t* leftover_signs_num,
                                           int32_t* precinct_bits_left) {
    assert((bitstream->bits_used == 0) || (bitstream->bits_used == 4));
    if (group_size != GROUP_SIZE) {
        return unpack_data_c(
            bitstream, buf, w, gclis, group_size, gtli, sign_flag, leftover_signs_num, precinct_bits_left);
    }
    const uint32_t group_num = w / GROUP_SIZE;
    const uint32_t leftover = w % GROUP_SIZE;
    const int with_sign = (sign_flag == 0);
    /* Nibbles per group on top of the bit planes, and their cost in bits. */
    const int32_t extra = with_sign ? 1 : 0;

    nibble_reader_t r;
    r.mem = bitstream->mem;
    r.nibble = 2 * (uint64_t)bitstream->offset + (bitstream->bits_used >> 2);
    r.size = bitstream->size;

    SvtJxsErrorType_t ret = SvtJxsErrorNone;
    uint32_t group = 0;
    while (group < group_num) {
        int32_t bitplanes = gclis[group] - gtli;
        if (bitplanes <= 0) {
            memset(buf, 0, sizeof(uint16_t) * GROUP_SIZE);
            group++;
            buf += GROUP_SIZE;
            continue;
        }
        const uint64_t byte = r.nibble >> 1;
        if (byte + 8 <= r.size) {
            /* Window of 16 - odd usable nibbles; decode groups while they fit. */
            const uint32_t avail = 16 - (uint32_t)(r.nibble & 1);
            const v128_t cols = window_columns(r.mem + byte);
            uint32_t used = 0;
            for (;;) {
                if (bitplanes <= 0) {
                    memset(buf, 0, sizeof(uint16_t) * GROUP_SIZE);
                }
                else {
                    const uint32_t n = (uint32_t)(bitplanes + extra);
                    if (used + n > avail) {
                        break;
                    }
                    *precinct_bits_left -= 4 * (int32_t)n;
                    if (*precinct_bits_left < 0) {
                        /* unpack_data_c() leaves the failing group zeroed. */
                        memset(buf, 0, sizeof(uint16_t) * GROUP_SIZE);
                        r.nibble += used;
                        ret = SvtJxsErrorDecoderInvalidBitstream;
                        goto end;
                    }
                    store_group(buf, wasm_u32x4_shr(cols, avail - used - n), bitplanes, gtli, with_sign);
                    used += n;
                }
                group++;
                buf += GROUP_SIZE;
                if (group == group_num) {
                    break;
                }
                bitplanes = gclis[group] - gtli;
            }
            r.nibble += used;
            if (used) {
                continue;
            }
            /* The group doesn't fit into a window (corrupt stream): below. */
        }
        /* Near the end of the buffer or a group of more than 16 nibbles. */
        *precinct_bits_left -= 4 * (bitplanes + extra);
        if (*precinct_bits_left < 0) {
            memset(buf, 0, sizeof(uint16_t) * GROUP_SIZE);
            ret = SvtJxsErrorDecoderInvalidBitstream;
            goto end;
        }
        unpack_group(&r, buf, bitplanes, gtli, with_sign);
        group++;
        buf += GROUP_SIZE;
    }
    if (leftover) {
        uint16_t buf_tmp[GROUP_SIZE] = {0};
        const int32_t bitplanes = gclis[group_num] - gtli;
        if (bitplanes > 0) {
            *precinct_bits_left -= 4 * (bitplanes + extra);
            if (*precinct_bits_left < 0) {
                ret = SvtJxsErrorDecoderInvalidBitstream;
                goto end;
            }
            unpack_group(&r, buf_tmp, bitplanes, gtli, with_sign);
        }
        if (!with_sign) {
            *leftover_signs_num = 0;
            for (uint32_t leftover_id = leftover; leftover_id < GROUP_SIZE; leftover_id++) {
                *leftover_signs_num += !!buf_tmp[leftover_id];
            }
        }
        memcpy(buf, buf_tmp, sizeof(uint16_t) * (leftover));
    }

end:
    bitstream->offset = (uint32_t)(r.nibble >> 1);
    bitstream->bits_used = (uint32_t)(r.nibble & 1) << 2;
    return ret;
}

#endif /* __wasm_simd128__ */
