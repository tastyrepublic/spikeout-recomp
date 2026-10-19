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
    src/runtime/netboard.cpp
    src/runtime/jtag.cpp
    src/runtime/commboard.cpp
    src/runtime/eeprom.cpp
    src/runtime/rtc.cpp
    src/runtime/tilegen.cpp
    src/runtime/real3d.cpp
    src/runtime/real3d_render.cpp
    src/runtime/crypt315_5881.cpp
    src/runtime/memory_map.cpp
    src/runtime/rom.cpp
    src/games/games.cpp
    src/games/spikeofe.cpp)
target_include_directories(m3runtime PUBLIC src)
target_link_libraries(m3runtime PUBLIC ppc PRIVATE moira spikeout_warnings)

# Moira 68000 core (MIT, Dirk W. Hoffmann, v3.0; third_party/moira) for the
# network board's 68000. Musashi was rejected: its bundled SoftFloat 2b
# license has a disputed indemnity clause.
add_library(moira STATIC third_party/moira/Moira.cpp third_party/moira/MoiraDebugger.cpp)
target_include_directories(moira SYSTEM PUBLIC third_party/moira)
if(MSVC)
    target_compile_options(moira PRIVATE /bigobj /w)
else()
    target_compile_options(moira PRIVATE -w)
    if(MINGW)
        target_compile_options(moira PRIVATE -Wa,-mbig-obj)
    endif()
endif()

# Static analysis: control flow (functions, blocks, jump tables).
add_library(analysis
    src/analysis/code_image.cpp
    src/analysis/control_flow.cpp
    src/analysis/boot_image.cpp)
target_include_directories(analysis PUBLIC src)
target_link_libraries(analysis PUBLIC ppc m3runtime PRIVATE spikeout_warnings)

# Recompiled code has to give the same floating-point results on every
# machine (save states and rollback netplay compare state across them), so
# separate multiplies and adds must never be fused into FMA instructions.
# MSVC doesn't contract by default.
add_library(spikeout_fp INTERFACE)
if(NOT MSVC)
    target_compile_options(spikeout_fp INTERFACE -ffp-contract=off)
endif()

# What recompiled code runs against (CPU state, bus, function table).
# Linking it brings in spikeout_fp for the generated code.
add_library(recomp_runtime src/recomp/context.cpp src/recomp/frame_timing.cpp)
target_include_directories(recomp_runtime PUBLIC src)
target_link_libraries(recomp_runtime PUBLIC ppc m3runtime spikeout_fp PRIVATE spikeout_warnings)

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
    # The boot ROM (fixed CROM code, run from reset).
    set(boot_rom_cpp ${SPIKEOUT_GENERATED_DIR}/boot_rom.cpp)
    add_custom_command(
        OUTPUT ${boot_rom_cpp}
        COMMAND ppcrecomp --rom ${SPIKEOUT_ROM_DIR} --segment crom --name boot_rom -o ${boot_rom_cpp}
        DEPENDS ppcrecomp
        COMMENT "Recompiling the boot ROM"
        VERBATIM)
    add_library(boot_rom STATIC ${boot_rom_cpp})
    target_link_libraries(boot_rom PUBLIC recomp_runtime PRIVATE spikeout_warnings)

    # The game: the program the boot ROM copies to RAM, split over several
    # files so they compile in parallel.
    set(game_ram_parts 8)
    set(game_ram_cpp ${SPIKEOUT_GENERATED_DIR}/game_ram.cpp)
    set(game_ram_files ${game_ram_cpp})
    math(EXPR last_part "${game_ram_parts} - 1")
    foreach(i RANGE ${last_part})
        list(APPEND game_ram_files ${SPIKEOUT_GENERATED_DIR}/game_ram_${i}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${game_ram_files}
        COMMAND ppcrecomp --rom ${SPIKEOUT_ROM_DIR} --segment ram --scan-pointers --name game_ram
                --split ${game_ram_parts} -o ${game_ram_cpp}
        DEPENDS ppcrecomp
        COMMENT "Recompiling the game"
        VERBATIM)
    add_library(game_ram STATIC ${game_ram_files})
    # Report where FP NaNs start (Context::first_nan_pc); slows the game down.
    option(SPIKEOUT_FP_TRAP "Check every FP result of the recompiled game for NaN" OFF)
    if(SPIKEOUT_FP_TRAP)
        target_compile_definitions(game_ram PRIVATE SPIKEOUT_FP_TRAP)
    endif()
    target_link_libraries(game_ram PUBLIC recomp_runtime PRIVATE spikeout_warnings)
    if(MSVC)
        target_compile_options(game_ram PRIVATE /bigobj)
    endif()

    add_executable(boottest tools/boottest.cpp)
    target_link_libraries(boottest PRIVATE boot_rom spikeout_warnings)

    add_executable(gamerun tools/gamerun.cpp)
    target_link_libraries(gamerun PRIVATE game_ram boot_rom spikeout_warnings)

    # The game in a window. SDL3 from the system if CMake finds it, otherwise
    # downloaded and built with the project (zlib license).
    option(SPIKEOUT_FRONTEND "Build the spikeout window (SDL3)" ON)
    if(SPIKEOUT_FRONTEND)
        find_package(SDL3 CONFIG QUIET)
        if(NOT SDL3_FOUND)
            include(FetchContent)
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
            set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
            set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(SDL3
                GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                GIT_TAG release-3.4.16
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(SDL3)
        endif()
        add_executable(spikeout tools/spikeout.cpp)
        target_link_libraries(spikeout PRIVATE game_ram boot_rom SDL3::SDL3 spikeout_warnings)
    endif()
endif()

if(SPIKEOUT_BUILD_TESTS)
    enable_testing()
    add_executable(spikeout_tests
        tests/test_main.cpp
        tests/test_ppc.cpp
        tests/test_runtime.cpp
        tests/test_analysis.cpp
        tests/test_devices.cpp)
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
