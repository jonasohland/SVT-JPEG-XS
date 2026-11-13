/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/* WebAssembly SIMD128 versions of the horizontal inverse 5/3 lifting steps
 * idwt_horizontal_line_lf16_hf16_c() and idwt_horizontal_line_lf32_hf16_c().
 * Bit-exact with C (32-bit wrap-around, arithmetic shifts).
 *
 * With L/H the (shifted) low/high pass inputs and E[k] = out[2k],
 * O[k] = out[2k + 1], the C loop computes
 *   E[k]     = L[k] - ((H[k - 1] + H[k] + 2) >> 2)
 *   O[k - 1] = H[k - 1] + ((E[k - 1] + E[k]) >> 1)
 * reading E[k - 1] back from out[]. The even outputs only depend on the
 * inputs, so a block computes E[k - 1 .. k + 2] and E[k .. k + 3] directly,
 * derives O[k - 1 .. k + 2] and interleaves. The first and last outputs keep
 * the C code. */

#ifdef __wasm_simd128__

#include <wasm_simd128.h>
#include "Idwt_wasm_simd128.h"
#include "Definitions.h"

/* One scalar step of the C loop (k >= 1). */
#define IDWT_H_STEP(L_AT, k)                                                                                          \
    do {                                                                                                              \
        out[2 * (k)] = L_AT(k) - ((LSHIFT32(in_hf[(k)-1], shift) + LSHIFT32(in_hf[(k)], shift) + 2) >> 2);            \
        out[2 * (k)-1] = LSHIFT32(in_hf[(k)-1], shift) + ((out[2 * (k)-2] + out[2 * (k)]) >> 1);                      \
    } while (0)

/* Tail after the loop, k is the first index the loop did not process. */
#define IDWT_H_TAIL(L_AT, k)                                                                                          \
    do {                                                                                                              \
        if (len & 1) {                                                                                                \
            out[2 * (k)] = L_AT(k) - ((LSHIFT32(in_hf[(k)-1], shift) + 1) >> 1);                                      \
            out[2 * (k)-1] = LSHIFT32(in_hf[(k)-1], shift) + ((out[2 * (k)-2] + out[2 * (k)]) >> 1);                  \
        }                                                                                                             \
        else {                                                                                                        \
            out[2 * (k)-1] = LSHIFT32(in_hf[(k)-1], shift) + out[2 * (k)-2];                                          \
        }                                                                                                             \
    } while (0)

static FORCE_INLINE v128_t load_h(const int16_t* p, uint8_t shift) {
    return wasm_i32x4_shl(wasm_i32x4_load16x4(p), shift);
}

/* Even outputs E[j .. j + 3] = L[j..] - ((H[j - 1..] + H[j..] + 2) >> 2). */
static FORCE_INLINE v128_t evens(v128_t l, v128_t h_prev, v128_t h) {
    return wasm_i32x4_sub(l, wasm_i32x4_shr(wasm_i32x4_add(wasm_i32x4_add(h_prev, h), wasm_i32x4_splat(2)), 2));
}

/* Block k: writes out[2k - 1 .. 2k + 6] = O[k - 1], E[k], ..., O[k + 2], E[k + 3]. */
static FORCE_INLINE void block(int32_t* out, const int16_t* in_hf, v128_t l_a, v128_t l_b, uint32_t k, uint8_t shift) {
    const v128_t h_m2 = load_h(in_hf + k - 2, shift);
    const v128_t h_m1 = load_h(in_hf + k - 1, shift);
    const v128_t h_0 = load_h(in_hf + k, shift);
    const v128_t e_a = evens(l_a, h_m2, h_m1); /* E[k - 1 .. k + 2] */
    const v128_t e_b = evens(l_b, h_m1, h_0); /* E[k .. k + 3] */
    const v128_t o = wasm_i32x4_add(h_m1, wasm_i32x4_shr(wasm_i32x4_add(e_a, e_b), 1)); /* O[k - 1 .. k + 2] */
    wasm_v128_store(out + 2 * k - 1, wasm_i32x4_shuffle(o, e_b, 0, 4, 1, 5));
    wasm_v128_store(out + 2 * k + 3, wasm_i32x4_shuffle(o, e_b, 2, 6, 3, 7));
}

#define L16_AT(k) LSHIFT32(in_lf[(k)], shift)
#define L32_AT(k) (in_lf[(k)])

void idwt_horizontal_line_lf16_hf16_wasm_simd128(const int16_t* in_lf, const int16_t* in_hf, int32_t* out, uint32_t len,
                                                 uint8_t shift) {
    assert((len >= 2) && "[idwt_c()] ERROR: Length is too small!");
    out[0] = LSHIFT32(in_lf[0], shift) - (((LSHIFT32(in_hf[0], shift)) + 1) >> 1);
    /* The C loop runs for k = 1 .. while 2k <= len - 2. */
    const uint32_t k_end = (len - 2) / 2 + 1;
    uint32_t k = 1;
    if (k < k_end) {
        IDWT_H_STEP(L16_AT, k);
        k++;
    }
    /* Vector blocks need E[k + 3] from the loop formula: k + 3 < k_end. */
    for (; k + 4 <= k_end; k += 4) {
        const v128_t l_a = wasm_i32x4_shl(wasm_i32x4_load16x4(in_lf + k - 1), shift);
        const v128_t l_b = wasm_i32x4_shl(wasm_i32x4_load16x4(in_lf + k), shift);
        block(out, in_hf, l_a, l_b, k, shift);
    }
    for (; k < k_end; k++) {
        IDWT_H_STEP(L16_AT, k);
    }
    IDWT_H_TAIL(L16_AT, k);
}

void idwt_horizontal_line_lf32_hf16_wasm_simd128(const int32_t* in_lf, const int16_t* in_hf, int32_t* out, uint32_t len,
                                                 uint8_t shift) {
    assert((len >= 2) && "[idwt_c()] ERROR: Length is too small!");
    out[0] = in_lf[0] - (((LSHIFT32(in_hf[0], shift)) + 1) >> 1);
    const uint32_t k_end = (len - 2) / 2 + 1;
    uint32_t k = 1;
    if (k < k_end) {
        IDWT_H_STEP(L32_AT, k);
        k++;
    }
    for (; k + 4 <= k_end; k += 4) {
        const v128_t l_a = wasm_v128_load(in_lf + k - 1);
        const v128_t l_b = wasm_v128_load(in_lf + k);
        block(out, in_hf, l_a, l_b, k, shift);
    }
    for (; k < k_end; k++) {
        IDWT_H_STEP(L32_AT, k);
    }
    IDWT_H_TAIL(L32_AT, k);
}

#endif /* __wasm_simd128__ */
