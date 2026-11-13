#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

# SampleEncoder Directory CMakeLists.txt

# Include Subdirectories
include_directories(
    ${PROJECT_SOURCE_DIR}/Source/API/
    )

file(GLOB all_files
    "../../API/*.h"
    "main.c"
)

# App Source Files
add_executable(SvtJpegxsSampleEncoder
    ${all_files})

#Link only with required libraries to detect in linker time references between decoder_lib an encoder_lib
svt_target_objects(lib_list_encoder
    COMMON_CODEC ${SVT_COMMON_ASM_LIBS}
    ENCODER_CODEC ${SVT_ENCODER_ASM_LIBS})
list(APPEND lib_list_encoder ${SVT_CPUINFO_LIB})

#********** SET COMPILE FLAGS************
# Link the Encoder Sample App
target_link_libraries(SvtJpegxsSampleEncoder
    ${lib_list_encoder})

if(UNIX)
    target_link_libraries(SvtJpegxsSampleEncoder
        pthread
        m)
endif()

add_dependencies(SvtJpegxsSampleEncoder
    COMMON_CODEC ${SVT_COMMON_ASM_LIBS}
    ENCODER_CODEC ${SVT_ENCODER_ASM_LIBS}
    ${SVT_CPUINFO_LIB})

svt_install_app(SvtJpegxsSampleEncoder)
