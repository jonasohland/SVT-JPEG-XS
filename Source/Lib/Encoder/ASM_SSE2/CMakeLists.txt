#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

file(GLOB all_files CONFIGURE_DEPENDS "*.h" "*.c" "*.asm")

add_library(ENCODER_ASM_SSE2 OBJECT ${all_files})

target_include_directories(ENCODER_ASM_SSE2 PRIVATE
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/ASM_SSE2)

target_compile_options(ENCODER_ASM_SSE2 PRIVATE $<$<COMPILE_LANGUAGE:C>:-msse2>)
