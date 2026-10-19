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

add_executable(ppcdis tools/ppcdis.cpp)
target_link_libraries(ppcdis PRIVATE ppc spikeout_warnings)

add_executable(romcheck tools/romcheck.cpp)
target_link_libraries(romcheck PRIVATE m3runtime spikeout_warnings)

add_executable(codemap tools/codemap.cpp)
target_link_libraries(codemap PRIVATE analysis spikeout_warnings)

if(SPIKEOUT_BUILD_TESTS)
    enable_testing()
    add_executable(spikeout_tests
        tests/test_main.cpp
        tests/test_ppc.cpp
        tests/test_runtime.cpp
        tests/test_analysis.cpp)
    target_link_libraries(spikeout_tests PRIVATE analysis m3runtime spikeout_warnings)
    add_test(NAME spikeout_tests COMMAND spikeout_tests)
endif()
