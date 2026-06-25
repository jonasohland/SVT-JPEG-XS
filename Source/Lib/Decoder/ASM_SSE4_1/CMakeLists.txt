#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

file(GLOB all_files CONFIGURE_DEPENDS "*.h" "*.c" "*.asm")

add_library(DECODER_ASM_SSE4_1 OBJECT ${all_files})

target_include_directories(DECODER_ASM_SSE4_1 PRIVATE
    ${PROJECT_SOURCE_DIR}/Source/API
    ${PROJECT_SOURCE_DIR}/Source/Lib/Decoder/Codec
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/Codec)

target_compile_options(DECODER_ASM_SSE4_1 PRIVATE -msse4.1)
