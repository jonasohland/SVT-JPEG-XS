#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

file(GLOB all_files CONFIGURE_DEPENDS "*.h" "*.c" "*.asm")

add_library(COMMON_ASM_AVX2 OBJECT ${all_files})

target_include_directories(COMMON_ASM_AVX2 PRIVATE
    ${PROJECT_SOURCE_DIR}/Source/API
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/Codec
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/ASM_SSE2
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/ASM_AVX2)

target_compile_options(COMMON_ASM_AVX2 PRIVATE -mavx2)
