#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

# Decoder/ASM_WASM_SIMD128 Directory CMakeLists.txt
#
# WebAssembly SIMD128 kernels. The sources are empty unless the whole build is
# compiled with -msimd128 (__wasm_simd128__), which also enables their entries
# in the rtcd dispatch.

include_directories(
    ${PROJECT_SOURCE_DIR}/Source/API/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Decoder/Codec/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/Codec/
    )

file(GLOB all_files
    "*.h"
    "*.c")

add_library(DECODER_ASM_WASM_SIMD128 OBJECT ${all_files})
