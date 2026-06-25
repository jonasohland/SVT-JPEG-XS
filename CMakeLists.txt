#
# Copyright(c) 2024 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

cmake_minimum_required(VERSION 3.16)

project(svt-jpegxs VERSION 0.10.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a separate build directory.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "32-bit is not supported")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)
include(CheckIPOSupported)

# --- Assembler (NASM/YASM) ---
find_program(YASM_EXE yasm)
option(ENABLE_NASM "Use nasm if available (uses yasm by default if found)" OFF)
if(YASM_EXE AND NOT CMAKE_ASM_NASM_COMPILER MATCHES "yasm" AND NOT ENABLE_NASM)
    set(CMAKE_ASM_NASM_COMPILER "${YASM_EXE}" CACHE FILEPATH "Path to nasm compatible compiler" FORCE)
else()
    include(CheckLanguage)
    check_language(ASM_NASM)
    if(CMAKE_ASM_NASM_COMPILER AND CMAKE_ASM_NASM_COMPILER MATCHES "nasm")
        execute_process(COMMAND ${CMAKE_ASM_NASM_COMPILER} -v
            OUTPUT_VARIABLE NASM_VERSION
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "([.0-9])+" NASM_VERSION "${NASM_VERSION}")
        if(NASM_VERSION VERSION_LESS "2.13")
            message(FATAL_ERROR "nasm too old (requires >= 2.13, found ${NASM_VERSION})")
        endif()
    endif()
endif()
enable_language(ASM_NASM)

# --- Global settings ---
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

option(NATIVE "Build for native performance (march=native)" OFF)
option(COVERAGE "Generate coverage report" OFF)
set(SANITIZER "" CACHE STRING "Sanitizer: Address, Memory, Thread, Undefined, Integer")
set_property(CACHE SANITIZER PROPERTY STRINGS "" Address Memory Thread Undefined Integer)

# --- LTO ---
check_ipo_supported(RESULT ipo_supported)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    find_program(LLVM_AR_EXE llvm-ar)
    find_program(LLVM_RANLIB_EXE llvm-ranlib)
    if(LLVM_AR_EXE)
        set(CMAKE_AR "llvm-ar")
    endif()
    if(LLVM_RANLIB_EXE)
        set(CMAKE_RANLIB "llvm-ranlib")
    endif()
endif()

# --- Compile options (C/CXX only, not ASM_NASM) ---
set(LANG_C_CXX "$<COMPILE_LANGUAGE:C,CXX>")
add_compile_options(
    "$<${LANG_C_CXX}:-Wall>"
    "$<${LANG_C_CXX}:-Wextra>"
    "$<${LANG_C_CXX}:-Wformat>"
    "$<${LANG_C_CXX}:-Wformat-security>"
    "$<${LANG_C_CXX}:-Werror>"
    "$<${LANG_C_CXX}:-fstack-protector-strong>")

if(NATIVE)
    add_compile_options(
        "$<${LANG_C_CXX}:-march=native>"
        "$<${LANG_C_CXX}:-mno-avx>")
endif()

add_compile_definitions(ARCH_X86_64=1)
add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)

# --- Linker hardening ---
add_link_options("LINKER:-z,noexecstack" "LINKER:-z,relro" "LINKER:-z,now")

# --- NASM platform/debug flags ---
set(CMAKE_ASM_NASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -DUNIX64")

if(CMAKE_ASM_NASM_COMPILER MATCHES "nasm")
    set(CMAKE_ASM_NASM_FLAGS_DEBUG "${CMAKE_ASM_NASM_FLAGS_DEBUG} -gdwarf")
elseif(CMAKE_ASM_NASM_COMPILER MATCHES "yasm")
    set(CMAKE_ASM_NASM_FLAGS_DEBUG "${CMAKE_ASM_NASM_FLAGS_DEBUG} -gdwarf2")
endif()

# --- Sanitizer ---
if(SANITIZER)
    string(TOLOWER "${SANITIZER}" SANITIZER_LOWER)
    if(SANITIZER_LOWER STREQUAL "address")
        add_compile_options("$<${LANG_C_CXX}:-fsanitize=address>")
        add_link_options(-fsanitize=address)
    elseif(SANITIZER_LOWER STREQUAL "memory")
        add_compile_options("$<${LANG_C_CXX}:-fsanitize=memory>" "$<${LANG_C_CXX}:-fsanitize-memory-track-origins>")
        add_link_options(-fsanitize=memory)
    elseif(SANITIZER_LOWER STREQUAL "thread")
        add_compile_options("$<${LANG_C_CXX}:-fsanitize=thread>")
        add_link_options(-fsanitize=thread)
    elseif(SANITIZER_LOWER STREQUAL "undefined")
        add_compile_options("$<${LANG_C_CXX}:-fsanitize=undefined>")
        add_link_options(-fsanitize=undefined)
    elseif(SANITIZER_LOWER STREQUAL "integer")
        add_compile_options("$<${LANG_C_CXX}:-fsanitize=integer>")
        add_link_options(-fsanitize=integer)
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${SANITIZER}")
    endif()
    add_compile_options("$<${LANG_C_CXX}:-fno-omit-frame-pointer>" "$<${LANG_C_CXX}:-fno-optimize-sibling-calls>")
endif()

# --- Coverage ---
if(COVERAGE)
    add_compile_options("$<${LANG_C_CXX}:--coverage>")
    add_link_options(--coverage)
endif()

# --- Threading ---
set(CMAKE_THREAD_PREFER_PTHREADS ON)
find_package(Threads)

# --- pkg-config paths ---
set(SVT_JPEGXS_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
set(SVT_JPEGXS_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
if(NOT IS_ABSOLUTE "${SVT_JPEGXS_INCLUDEDIR}")
    string(PREPEND SVT_JPEGXS_INCLUDEDIR "\${prefix}/")
endif()
if(NOT IS_ABSOLUTE "${SVT_JPEGXS_LIBDIR}")
    string(PREPEND SVT_JPEGXS_LIBDIR "\${exec_prefix}/")
endif()

# --- Subdirectories ---
add_subdirectory(third_party/cpuinfo)
add_subdirectory(Source/Lib)

# --- Install ---
install(DIRECTORY ${PROJECT_SOURCE_DIR}/Source/API/
    DESTINATION "${CMAKE_INSTALL_FULL_INCLUDEDIR}/svt-jpegxs"
    FILES_MATCHING PATTERN "*.h")
