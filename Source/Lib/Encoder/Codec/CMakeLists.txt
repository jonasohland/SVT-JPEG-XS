#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

file(GLOB all_files CONFIGURE_DEPENDS "*.h" "*.c")

add_library(ENCODER_CODEC OBJECT ${all_files})

target_include_directories(ENCODER_CODEC PRIVATE
    ${PROJECT_SOURCE_DIR}/Source/API
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/Codec
    ${PROJECT_SOURCE_DIR}/Source/Lib/Encoder/Codec
    ${PROJECT_SOURCE_DIR}/Source/Lib/Encoder/ASM_SSE4_1
    ${PROJECT_SOURCE_DIR}/Source/Lib/Encoder/ASM_AVX2
    ${PROJECT_SOURCE_DIR}/Source/Lib/Encoder/ASM_AVX512)
