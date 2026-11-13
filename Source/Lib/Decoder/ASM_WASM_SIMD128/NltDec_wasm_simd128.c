/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/* WebAssembly SIMD128 versions of the linear output scaling (inverse NLT,
 * Tnlt == 0). Bit-exact with NltDec.c:
 *   v = in + ((1 << bw) >> 1);
 *   v = (v + ((1 << dzeta) >> 1)) >> dzeta;   (computed in 64 bits in C)
 *   out = clamp(v, 0, (1 << depth) - 1);
 * The rounding shift is done in 32 bits without overflow as
 * (v >> dzeta) + (((v & (2^dzeta - 1)) + round) >> dzeta), valid for
 * dzeta <= 30. Other parameters (not produced by valid streams) use C. */

#ifdef __wasm_simd128__

#include <wasm_simd128.h>
#include "NltDec_wasm_simd128.h"
#include "NltDec.h"
#include "Definitions.h"

typedef struct scale_params {
    v128_t half; /* (1 << bw) >> 1 */
    v128_t frac_mask; /* 2^dzeta - 1 */
    v128_t round; /* 2^dzeta >> 1 */
    v128_t max; /* (1 << depth) - 1 */
    uint32_t dzeta;
} scale_params_t;

/* Returns 0 if the C version must be used. */
static int scale_params_init(scale_params_t* p, uint32_t bw, uint32_t depth, int32_t out_max) {
    const int32_t dzeta = bw - depth;
    const int32_t m = (1 << depth) - 1; /* as in C */
    if (dzeta < 0 || dzeta > 30 || m < 0 || m > out_max) {
        return 0;
    }
    p->half = wasm_i32x4_splat((1 << bw) >> 1); /* as in C */
    p->frac_mask = wasm_i32x4_splat((int32_t)((1u << dzeta) - 1));
    p->round = wasm_i32x4_splat((int32_t)((1u << dzeta) >> 1));
    p->max = wasm_i32x4_splat(m);
    p->dzeta = (uint32_t)dzeta;
    return 1;
}

static FORCE_INLINE v128_t scale4(const scale_params_t* p, const int32_t* in) {
    v128_t v = wasm_i32x4_add(wasm_v128_load(in), p->half);
    const v128_t frac = wasm_i32x4_add(wasm_v128_and(v, p->frac_mask), p->round);
    v = wasm_i32x4_add(wasm_i32x4_shr(v, p->dzeta), wasm_i32x4_shr(frac, p->dzeta));
    return wasm_i32x4_min(wasm_i32x4_max(v, wasm_i32x4_splat(0)), p->max);
}

void linear_output_scaling_16bit_line_wasm_simd128(int32_t* in, uint32_t bw, uint32_t depth, uint16_t* out, uint32_t w) {
    scale_params_t p;
    if (!scale_params_init(&p, bw, depth, UINT16_MAX)) {
        linear_output_scaling_16bit_line_c(in, bw, depth, out, w);
        return;
    }
    uint32_t x = 0;
    for (; x + 8 <= w; x += 8) {
        /* Values are in [0, m], m <= 0xFFFF: the saturating narrow is exact. */
        wasm_v128_store(out + x, wasm_u16x8_narrow_i32x4(scale4(&p, in + x), scale4(&p, in + x + 4)));
    }
    if (x < w) {
        linear_output_scaling_16bit_line_c(in + x, bw, depth, out + x, w - x);
    }
}

void linear_output_scaling_8bit_line_wasm_simd128(int32_t* in, uint32_t bw, uint32_t depth, uint8_t* out, uint32_t w) {
    scale_params_t p;
    if (!scale_params_init(&p, bw, depth, UINT8_MAX)) {
        linear_output_scaling_8bit_line_c(in, bw, depth, out, w);
        return;
    }
    uint32_t x = 0;
    for (; x + 16 <= w; x += 16) {
        const v128_t a = wasm_i16x8_narrow_i32x4(scale4(&p, in + x), scale4(&p, in + x + 4));
        const v128_t b = wasm_i16x8_narrow_i32x4(scale4(&p, in + x + 8), scale4(&p, in + x + 12));
        wasm_v128_store(out + x, wasm_u8x16_narrow_i16x8(a, b));
    }
    if (x < w) {
        linear_output_scaling_8bit_line_c(in + x, bw, depth, out + x, w - x);
    }
}

void linear_output_scaling_8bit_wasm_simd128(const pi_t* const pi, int32_t* comps[MAX_COMPONENTS_NUM], uint32_t bw,
                                             uint32_t depth, svt_jpeg_xs_image_buffer_t* out) {
    for (uint32_t i = 0; i < pi->comps_num; i++) {
        const int32_t w = pi->components[i].width;
        const int32_t h = pi->components[i].height;
        uint8_t* out_buf = (uint8_t*)(out->data_yuv[i]);
        const uint32_t out_stride = out->stride[i];
        for (int32_t y = 0; y < h; y++) {
            linear_output_scaling_8bit_line_wasm_simd128(comps[i] + y * w, bw, depth, out_buf + y * out_stride, w);
        }
    }
}

void linear_output_scaling_16bit_wasm_simd128(const pi_t* const pi, int32_t* comps[MAX_COMPONENTS_NUM], uint32_t bw,
                                              uint32_t depth, svt_jpeg_xs_image_buffer_t* out) {
    for (uint32_t i = 0; i < pi->comps_num; i++) {
        const int32_t w = pi->components[i].width;
        const int32_t h = pi->components[i].height;
        uint16_t* out_buf = (uint16_t*)(out->data_yuv[i]);
        const uint32_t out_stride = out->stride[i];
        for (int32_t y = 0; y < h; y++) {
            linear_output_scaling_16bit_line_wasm_simd128(comps[i] + y * w, bw, depth, out_buf + y * out_stride, w);
        }
    }
}

#endif /* __wasm_simd128__ */
