/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef _NLT_DEC_WASM_SIMD128_H
#define _NLT_DEC_WASM_SIMD128_H

#include <stdint.h>
#include "Pi.h"
#include "SvtJpegxsDec.h"

#ifdef __cplusplus
extern "C" {
#endif

void linear_output_scaling_8bit_line_wasm_simd128(int32_t* in, uint32_t bw, uint32_t depth, uint8_t* out, uint32_t w);
void linear_output_scaling_16bit_line_wasm_simd128(int32_t* in, uint32_t bw, uint32_t depth, uint16_t* out, uint32_t w);
void linear_output_scaling_8bit_wasm_simd128(const pi_t* const pi, int32_t* comps[MAX_COMPONENTS_NUM], uint32_t bw,
                                             uint32_t depth, svt_jpeg_xs_image_buffer_t* out);
void linear_output_scaling_16bit_wasm_simd128(const pi_t* const pi, int32_t* comps[MAX_COMPONENTS_NUM], uint32_t bw,
                                              uint32_t depth, svt_jpeg_xs_image_buffer_t* out);

#ifdef __cplusplus
}
#endif

#endif /*_NLT_DEC_WASM_SIMD128_H*/
