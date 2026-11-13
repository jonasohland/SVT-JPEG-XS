/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/* WebAssembly SIMD128 version of dequant_c(). Bit-exact with the C version,
 * including its corner cases (gcli compared as int8_t, 16-bit wrap-around). */

#ifdef __wasm_simd128__

#include <wasm_simd128.h>
#include "Dequant_wasm_simd128.h"
#include "Dequant.h"
#include "Codestream.h"
#include "Definitions.h"
#include "SvtUtility.h"

/* The C version compares (int8_t)gcli > gtli, i.e. as int. */
static INLINE int gcli_above(uint8_t gcli, uint8_t gtli) {
    return (int8_t)gcli > (int)gtli;
}

static INLINE void inv_quant_deadzone_scalar(uint16_t* buf, uint32_t len, uint8_t gcli, uint8_t gtli) {
    if (!gcli_above(gcli, gtli)) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] & ~BITSTREAM_MASK_SIGN) {
            buf[i] |= (1 << (gtli - 1));
        }
    }
}

static void inv_quant_deadzone_wasm_simd128(uint16_t* buf, uint32_t size, const uint8_t* gclis, uint8_t gtli) {
    if (gtli > INT8_MAX) {
        return; /* (int8_t)gcli > gtli never holds. */
    }
    const v128_t gtli_v = wasm_i8x16_splat((int8_t)gtli);
    const v128_t add = wasm_i16x8_splat((int16_t)(1 << (gtli - 1)));
    const v128_t magnitude_mask = wasm_i16x8_splat((int16_t)(uint16_t)~BITSTREAM_MASK_SIGN);
    const v128_t zero = wasm_i16x8_splat(0);

    /* 16 coefficients = 4 groups per iteration. */
    uint32_t i = 0;
    for (; i + 16 <= size; i += 16, gclis += 4) {
        const v128_t gcli_mask = wasm_i8x16_gt(wasm_v128_load32_zero(gclis), gtli_v);
        /* Widen each group's byte mask to its 4 16-bit lanes. */
        const v128_t m0 = wasm_i8x16_shuffle(gcli_mask, gcli_mask, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1);
        const v128_t m1 = wasm_i8x16_shuffle(gcli_mask, gcli_mask, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3);

        v128_t c0 = wasm_v128_load(buf + i);
        v128_t c1 = wasm_v128_load(buf + i + 8);
        const v128_t nz0 = wasm_i16x8_ne(wasm_v128_and(c0, magnitude_mask), zero);
        const v128_t nz1 = wasm_i16x8_ne(wasm_v128_and(c1, magnitude_mask), zero);
        c0 = wasm_v128_or(c0, wasm_v128_and(add, wasm_v128_and(m0, nz0)));
        c1 = wasm_v128_or(c1, wasm_v128_and(add, wasm_v128_and(m1, nz1)));
        wasm_v128_store(buf + i, c0);
        wasm_v128_store(buf + i + 8, c1);
    }
    for (; i < size; i += GROUP_SIZE, gclis++) {
        inv_quant_deadzone_scalar(buf + i, MIN(GROUP_SIZE, size - i), *gclis, gtli);
    }
}

static void inv_quant_uniform_wasm_simd128(uint16_t* buf, uint32_t size, const uint8_t* gclis, uint8_t gtli) {
    const v128_t sign_mask = wasm_i16x8_splat((int16_t)BITSTREAM_MASK_SIGN);
    const v128_t magnitude_mask = wasm_i16x8_splat((int16_t)(uint16_t)~BITSTREAM_MASK_SIGN);

    /* One group (4 coefficients, 64 bits) at a time: the shift differs per group. */
    for (uint32_t i = 0; i < size; i += GROUP_SIZE, gclis++) {
        const uint8_t gcli = *gclis;
        if (!gcli_above(gcli, gtli)) {
            continue;
        }
        const uint32_t len = MIN(GROUP_SIZE, size - i);
        /* (uint8_t) like the C version's scale_value. */
        const uint8_t scale = (uint8_t)(gcli - gtli + 1);
        v128_t dq;
        if (len == GROUP_SIZE) {
            dq = wasm_v128_load64_zero(buf + i);
        }
        else {
            uint16_t tmp[GROUP_SIZE] = {0};
            for (uint32_t k = 0; k < len; k++) {
                tmp[k] = buf[i + k];
            }
            dq = wasm_v128_load64_zero(tmp);
        }
        const v128_t sign = wasm_v128_and(dq, sign_mask);
        v128_t phi = wasm_v128_and(dq, magnitude_mask);
        v128_t rho = phi;
        /* val >>= scale on a 15-bit value: wasm shifts count modulo 16, the C
         * version shifts an int, which gives 0 for scale >= 16. */
        if (scale < 16) {
            phi = wasm_u16x8_shr(phi, scale);
            while (wasm_v128_any_true(phi)) {
                rho = wasm_i16x8_add(rho, phi);
                phi = wasm_u16x8_shr(phi, scale);
            }
        }
        const v128_t out = wasm_v128_or(rho, sign);
        if (len == GROUP_SIZE) {
            wasm_v128_store64_lane(buf + i, out, 0);
        }
        else {
            uint16_t tmp[GROUP_SIZE];
            wasm_v128_store64_lane(tmp, out, 0);
            for (uint32_t k = 0; k < len; k++) {
                buf[i + k] = tmp[k];
            }
        }
    }
}

void dequant_wasm_simd128(uint16_t* buf, uint32_t buf_len, uint8_t* gclis, uint32_t group_size, uint8_t gtli,
                          QUANT_TYPE dq_type) {
    if (group_size != GROUP_SIZE) {
        dequant_c(buf, buf_len, gclis, group_size, gtli, dq_type);
        return;
    }
    if (gtli == 0) {
        return;
    }
    switch (dq_type) {
    case QUANT_TYPE_DEADZONE:
        inv_quant_deadzone_wasm_simd128(buf, buf_len, gclis, gtli);
        return;
    case QUANT_TYPE_UNIFORM:
        inv_quant_uniform_wasm_simd128(buf, buf_len, gclis, gtli);
        return;
    default:
        dequant_c(buf, buf_len, gclis, group_size, gtli, dq_type);
        return;
    }
}

#endif /* __wasm_simd128__ */
