#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

file(GLOB all_files CONFIGURE_DEPENDS
    "Threads/*.h" "Threads/*.c"
    "*.h" "*.c")

add_library(COMMON_CODEC OBJECT ${all_files})

target_include_directories(COMMON_CODEC PRIVATE
    ${PROJECT_SOURCE_DIR}/Source/API
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/Codec
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/ASM_SSE2
    ${PROJECT_SOURCE_DIR}/Source/Lib/Common/ASM_AVX2
    ${PROJECT_SOURCE_DIR}/Source/Lib/Encoder/Codec
    ${PROJECT_SOURCE_DIR}/third_party/cpuinfo/include)
