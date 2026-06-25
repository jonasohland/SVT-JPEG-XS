#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

add_subdirectory(Common/Codec)
add_subdirectory(Common/ASM_SSE2)
add_subdirectory(Common/ASM_AVX2)
add_subdirectory(Encoder/Codec)
add_subdirectory(Encoder/ASM_SSE2)
add_subdirectory(Encoder/ASM_SSE4_1)
add_subdirectory(Encoder/ASM_AVX2)
add_subdirectory(Encoder/ASM_AVX512)
add_subdirectory(Decoder/Codec)
add_subdirectory(Decoder/ASM_SSE4_1)
add_subdirectory(Decoder/ASM_AVX2)
add_subdirectory(Decoder/ASM_AVX512)

# Platform libraries
find_library(M_LIB NAMES m)
if(M_LIB)
    list(APPEND PLATFORM_LIBS m)
endif()
list(APPEND PLATFORM_LIBS Threads::Threads)
set(LIBS_PRIVATE "-lpthread -lm")

if(COVERAGE)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU")
        set(LIBS_PRIVATE "-lgcov ${LIBS_PRIVATE}")
    else()
        set(LIBS_PRIVATE "--coverage ${LIBS_PRIVATE}")
    endif()
endif()

# API functions need default visibility even in a static library so consumers
# can re-export them from shared libraries if needed.
foreach(target IN ITEMS COMMON_CODEC ENCODER_CODEC DECODER_CODEC)
    target_compile_definitions(${target} PRIVATE DEF_DLL DEF_BUILDING_SHARED_LIBS)
endforeach()

# Static library with all objects (including cpuinfo/clog) bundled in
add_library(SvtJpegxsLib STATIC
    $<TARGET_OBJECTS:COMMON_CODEC>
    $<TARGET_OBJECTS:COMMON_ASM_SSE2>
    $<TARGET_OBJECTS:COMMON_ASM_AVX2>
    $<TARGET_OBJECTS:ENCODER_CODEC>
    $<TARGET_OBJECTS:ENCODER_ASM_SSE2>
    $<TARGET_OBJECTS:ENCODER_ASM_SSE4_1>
    $<TARGET_OBJECTS:ENCODER_ASM_AVX2>
    $<TARGET_OBJECTS:ENCODER_ASM_AVX512>
    $<TARGET_OBJECTS:DECODER_CODEC>
    $<TARGET_OBJECTS:DECODER_ASM_SSE4_1>
    $<TARGET_OBJECTS:DECODER_ASM_AVX2>
    $<TARGET_OBJECTS:DECODER_ASM_AVX512>
    $<TARGET_OBJECTS:cpuinfo>
    $<TARGET_OBJECTS:clog>)

add_library(SvtJpegxs::SvtJpegxs ALIAS SvtJpegxsLib)

set_target_properties(SvtJpegxsLib PROPERTIES
    OUTPUT_NAME SvtJpegxs)

target_include_directories(SvtJpegxsLib PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/Source/API>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/svt-jpegxs>)

target_link_libraries(SvtJpegxsLib PUBLIC ${PLATFORM_LIBS})

install(TARGETS SvtJpegxsLib
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

configure_file(pkg-config.pc.in ${CMAKE_BINARY_DIR}/SvtJpegxs.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/SvtJpegxs.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
