/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef _UNPACK_WASM_SIMD128_H
#define _UNPACK_WASM_SIMD128_H

#include <stdint.h>
#include "SvtJpegxsDec.h"
#include "BitstreamReader.h"

#ifdef __cplusplus
extern "C" {
#endif

SvtJxsErrorType_t unpack_data_wasm_simd128(bitstream_reader_t* bitstream, uint16_t* buf, uint32_t w, uint8_t* gclis,
                                           uint32_t group_size, uint8_t gtli, uint8_t sign_flag, uint8_t* leftover_signs_num,
                                           int32_t* precinct_bits_left);

#ifdef __cplusplus
}
#endif

#endif /*_UNPACK_WASM_SIMD128_H*/
