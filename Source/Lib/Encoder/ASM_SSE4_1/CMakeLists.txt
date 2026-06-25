#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

file(GLOB all_files CONFIGURE_DEPENDS "*.h" "*.c")

add_library(ENCODER_ASM_SSE4_1 OBJECT ${all_files})

target_include_directories(ENCODER_ASM_SSE4_1 PRIVATE
    ${PROJECT_SOURCE_DIR}/Source/API
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/Codec
    ${PROJECT_SOURCE_DIR}/Source/Lib/Encoder/Codec)

target_compile_options(ENCODER_ASM_SSE4_1 PRIVATE -msse4.1)
