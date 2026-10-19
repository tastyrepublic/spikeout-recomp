cmake_minimum_required(VERSION 3.20)

project(spikeout-recomp
    VERSION 0.1.0
    DESCRIPTION "Static recompilation of Sega Model 3 Spikeout"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPIKEOUT_BUILD_TESTS "Build unit tests" ON)
option(SPIKEOUT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_library(spikeout_warnings INTERFACE)
if(MSVC)
    target_compile_options(spikeout_warnings INTERFACE /W4 /permissive- /utf-8)
    if(SPIKEOUT_WARNINGS_AS_ERRORS)
        target_compile_options(spikeout_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(spikeout_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(SPIKEOUT_WARNINGS_AS_ERRORS)
        target_compile_options(spikeout_warnings INTERFACE -Werror)
    endif()
endif()

# PowerPC 603e decoder and disassembler (the front end of the recompiler).
add_library(ppc
    src/ppc/decoder.cpp
    src/ppc/disasm.cpp)
target_include_directories(ppc PUBLIC src)
target_link_libraries(ppc PRIVATE spikeout_warnings)

# Model 3 runtime: memory map, ROM loading, game definitions.
add_library(m3runtime
    src/runtime/bus.cpp
    src/runtime/memory_map.cpp
    src/runtime/rom.cpp
    src/games/games.cpp
    src/games/spikeofe.cpp)
target_include_directories(m3runtime PUBLIC src)
target_link_libraries(m3runtime PUBLIC ppc PRIVATE spikeout_warnings)

# Static analysis: control flow (functions, blocks, jump tables).
add_library(analysis
    src/analysis/code_image.cpp
    src/analysis/control_flow.cpp
    src/analysis/boot_image.cpp)
target_include_directories(analysis PUBLIC src)
target_link_libraries(analysis PUBLIC ppc m3runtime PRIVATE spikeout_warnings)

# What recompiled code runs against (CPU state, bus, function table).
add_library(recomp_runtime src/recomp/context.cpp)
target_include_directories(recomp_runtime PUBLIC src)
target_link_libraries(recomp_runtime PUBLIC ppc m3runtime PRIVATE spikeout_warnings)

# The lifter: analyzed PowerPC code -> C++ source.
add_library(recomp_lifter src/recomp/lifter.cpp)
target_include_directories(recomp_lifter PUBLIC src)
target_link_libraries(recomp_lifter PUBLIC analysis PRIVATE spikeout_warnings)

add_executable(ppcdis tools/ppcdis.cpp)
target_link_libraries(ppcdis PRIVATE ppc spikeout_warnings)

add_executable(romcheck tools/romcheck.cpp)
target_link_libraries(romcheck PRIVATE m3runtime spikeout_warnings)

add_executable(codemap tools/codemap.cpp)
target_link_libraries(codemap PRIVATE analysis spikeout_warnings)

add_executable(ppcrecomp tools/ppcrecomp.cpp)
target_link_libraries(ppcrecomp PRIVATE recomp_lifter spikeout_warnings)

# Code generated from ROM data goes here, never into the source tree.
set(SPIKEOUT_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SPIKEOUT_GENERATED_DIR})

# Folder with your unzipped spikeofe ROM set. When set, the build
# recompiles the boot ROM and adds a test that runs it.
set(SPIKEOUT_ROM_DIR "" CACHE PATH "Folder with the unzipped spikeofe ROM set (enables the boot ROM test)")

if(SPIKEOUT_ROM_DIR)
    set(boot_rom_cpp ${SPIKEOUT_GENERATED_DIR}/boot_rom.cpp)
    add_custom_command(
        OUTPUT ${boot_rom_cpp}
        COMMAND ppcrecomp --rom ${SPIKEOUT_ROM_DIR} --segment crom --name boot_rom -o ${boot_rom_cpp}
        DEPENDS ppcrecomp
        COMMENT "Recompiling the boot ROM"
        VERBATIM)
    add_executable(boottest tools/boottest.cpp ${boot_rom_cpp})
    target_link_libraries(boottest PRIVATE recomp_runtime spikeout_warnings)
endif()

if(SPIKEOUT_BUILD_TESTS)
    enable_testing()
    add_executable(spikeout_tests
        tests/test_main.cpp
        tests/test_ppc.cpp
        tests/test_runtime.cpp
        tests/test_analysis.cpp)
    target_link_libraries(spikeout_tests PRIVATE analysis m3runtime spikeout_warnings)
    add_test(NAME spikeout_tests COMMAND spikeout_tests)

    # Lifter tests: a small test program is lifted at build time and run.
    add_library(lifter_test_program STATIC tests/lifter/program.cpp)
    target_include_directories(lifter_test_program PUBLIC tests)
    target_link_libraries(lifter_test_program PRIVATE spikeout_warnings)

    add_executable(gen_lifter_tests tests/lifter/gen_lifter_tests.cpp)
    target_link_libraries(gen_lifter_tests PRIVATE lifter_test_program recomp_lifter spikeout_warnings)

    set(lifter_test_cpp ${SPIKEOUT_GENERATED_DIR}/lifter_test.cpp)
    add_custom_command(
        OUTPUT ${lifter_test_cpp}
        COMMAND gen_lifter_tests ${lifter_test_cpp}
        DEPENDS gen_lifter_tests
        COMMENT "Lifting the lifter test program"
        VERBATIM)
    add_executable(lifter_tests tests/test_main.cpp tests/lifter/lifter_tests.cpp ${lifter_test_cpp})
    target_include_directories(lifter_tests PRIVATE tests)
    target_link_libraries(lifter_tests PRIVATE lifter_test_program recomp_runtime spikeout_warnings)
    add_test(NAME lifter_tests COMMAND lifter_tests)

    if(SPIKEOUT_ROM_DIR)
        add_test(NAME boot_rom COMMAND boottest ${SPIKEOUT_ROM_DIR})
    endif()
endif()
