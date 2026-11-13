#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

# SampleDecoder Directory CMakeLists.txt

# Include Subdirectories
include_directories(
    ${PROJECT_SOURCE_DIR}/Source/API/
    )

file(GLOB all_files
    "../../API/*.h"
    "main.c"
)

# App Source Files
add_executable(SvtJpegxsSampleDecoder
    ${all_files})

#Link only with required libraries to detect in linker time references between decoder_lib an encoder_lib
svt_target_objects(lib_list_decoder
    COMMON_CODEC ${SVT_COMMON_ASM_LIBS}
    DECODER_CODEC ${SVT_DECODER_ASM_LIBS})
list(APPEND lib_list_decoder ${SVT_CPUINFO_LIB})

#********** SET COMPILE FLAGS************
# Link the Decoder Sample App
target_link_libraries(SvtJpegxsSampleDecoder
    ${lib_list_decoder})

if(UNIX)
    target_link_libraries(SvtJpegxsSampleDecoder
        pthread
        m)
endif()

add_dependencies(SvtJpegxsSampleDecoder
    COMMON_CODEC ${SVT_COMMON_ASM_LIBS}
    DECODER_CODEC ${SVT_DECODER_ASM_LIBS}
    ${SVT_CPUINFO_LIB})

svt_install_app(SvtJpegxsSampleDecoder)
