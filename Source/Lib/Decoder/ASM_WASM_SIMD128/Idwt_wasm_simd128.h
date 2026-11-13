/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef _IDWT_WASM_SIMD128_H
#define _IDWT_WASM_SIMD128_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void idwt_horizontal_line_lf16_hf16_wasm_simd128(const int16_t* in_lf, const int16_t* in_hf, int32_t* out, uint32_t len,
                                                 uint8_t shift);
void idwt_horizontal_line_lf32_hf16_wasm_simd128(const int32_t* in_lf, const int16_t* in_hf, int32_t* out, uint32_t len,
                                                 uint8_t shift);

#ifdef __cplusplus
}
#endif

#endif /*_IDWT_WASM_SIMD128_H*/
