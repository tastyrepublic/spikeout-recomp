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
    src/ppc/disasm.cpp
    src/ppc/timing.cpp)
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
    src/runtime/tile_sides.cpp
    src/runtime/widescreen.cpp
    src/runtime/real3d.cpp
    src/runtime/real3d_render.cpp
    src/runtime/real3d_scene.cpp
    src/runtime/real3d_interp.cpp
    src/runtime/scsp.cpp
    src/runtime/soundboard.cpp
    src/runtime/dsb2.cpp
    src/runtime/crypt315_5881.cpp
    src/runtime/memory_map.cpp
    src/runtime/rom.cpp
    src/runtime/usart8251.cpp
    src/runtime/watched_memory.cpp
    src/runtime/snapshots.cpp
    src/runtime/cpu_time.cpp
    src/games/games.cpp
    src/games/spikeofe.cpp)
target_include_directories(m3runtime PUBLIC src)
target_link_libraries(m3runtime PUBLIC ppc PRIVATE moira minimp3 spikeout_warnings)

# Network play (docs/netplay.md): UDP sockets, LAN discovery, Mode B sessions.
add_library(m3net src/net/udp.cpp src/net/session.cpp src/net/stun.cpp src/net/digest.cpp src/net/compress.cpp)
target_include_directories(m3net PUBLIC src)
target_link_libraries(m3net PRIVATE spikeout_warnings)
find_package(Threads REQUIRED)
target_link_libraries(m3net PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(m3net PRIVATE ws2_32 iphlpapi)
endif()

# zstd (BSD-3-Clause, Meta), downloaded and built with the project:
# network play sends save states compressed (a player joining a game in
# progress, a machine that went out of step).
include(FetchContent)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake)
FetchContent_MakeAvailable(zstd)
target_link_libraries(m3net PUBLIC libzstd_static)
target_include_directories(m3net PUBLIC ${zstd_SOURCE_DIR}/lib)

# The internet lobby's client (src/net/lobby.cpp): a WebSocket over TLS to
# the lobby Worker (lobby/), through libcurl (curl license): the system's if
# CMake finds it and it speaks wss, else built with the project: on Windows'
# own TLS (Schannel), elsewhere on OpenSSL. Without it, LAN play only. The
# lobby's address is fixed in the build (players can't change it).
include(FetchContent)
option(SPIKEOUT_LOBBY "Build the internet lobby client (libcurl)" ON)
set(SPIKEOUT_LOBBY_URL "wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby" CACHE STRING "The internet lobby (wss://.../v1/lobby; lobby/, on the owner's Cloudflare account)")
if(SPIKEOUT_LOBBY)
    # A system libcurl must speak wss: WebSockets are on by default from curl
    # 8.11, and older builds (Ubuntu 24.04's 8.5, macOS's) leave them out yet
    # still declare curl_ws_send, which then says "Unsupported protocol". So
    # it's asked, by running it (cmake/curl_probe: in a directory of its own,
    # so a curl found there and not wanted leaves no target behind).
    add_subdirectory(cmake/curl_probe)
    set(CURL_FOUND OFF)
    if(SPIKEOUT_SYSTEM_CURL)
        find_package(CURL REQUIRED)
    elseif(SPIKEOUT_SYSTEM_CURL_SEEN)
        message(STATUS "The system's libcurl doesn't speak wss (no WebSockets): curl built with the project")
    endif()
    if(NOT CURL_FOUND AND NOT WIN32)
        if(APPLE AND NOT OPENSSL_ROOT_DIR) # (Homebrew's, which CMake doesn't look in)
            execute_process(COMMAND brew --prefix openssl@3 OUTPUT_VARIABLE _ssl OUTPUT_STRIP_TRAILING_WHITESPACE
                            ERROR_QUIET RESULT_VARIABLE _ssl_found)
            if(_ssl_found EQUAL 0 AND EXISTS "${_ssl}")
                set(OPENSSL_ROOT_DIR "${_ssl}")
            endif()
        endif()
        find_package(OpenSSL QUIET)
        if(NOT OPENSSL_FOUND)
            message(STATUS "No OpenSSL to build curl on")
            set(SPIKEOUT_LOBBY OFF)
        endif()
    endif()
    if(NOT CURL_FOUND AND SPIKEOUT_LOBBY)
        set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
        set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
        set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
        set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        if(WIN32)
            set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
            set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        else()
            set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
        endif()
        set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
        set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
        set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
        set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
        set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
        set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
        set(HTTP_ONLY ON CACHE BOOL "" FORCE)
        set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
        set(_shared ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_Declare(curl
            GIT_REPOSITORY https://github.com/curl/curl.git
            GIT_TAG curl-8_15_0
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(curl)
        set(BUILD_SHARED_LIBS ${_shared})
    endif()
endif()
if(SPIKEOUT_LOBBY AND TARGET CURL::libcurl)
    target_sources(m3net PRIVATE src/net/lobby.cpp)
    target_link_libraries(m3net PUBLIC CURL::libcurl)
    target_compile_definitions(m3net PUBLIC SPIKEOUT_LOBBY SPIKEOUT_LOBBY_URL="${SPIKEOUT_LOBBY_URL}")
else()
    target_sources(m3net PRIVATE src/net/lobby_none.cpp)
    message(STATUS "No libcurl: the internet lobby is left out (LAN play only)")
endif()

# Moira 68000 core (MIT, Dirk W. Hoffmann, v3.0; third_party/moira) for the
# network board's 68000. Musashi was rejected: its bundled SoftFloat 2b
# license has a disputed indemnity clause.
# minimp3 (CC0, lieff/minimp3, third_party/minimp3): decodes the music
# board's MPEG audio.
add_library(minimp3 STATIC third_party/minimp3/minimp3.cpp)
target_include_directories(minimp3 SYSTEM PUBLIC third_party/minimp3)
if(MSVC)
    target_compile_options(minimp3 PRIVATE /w)
else()
    target_compile_options(minimp3 PRIVATE -w)
endif()

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
add_library(recomp_runtime src/recomp/context.cpp src/recomp/frame_timing.cpp src/games/spikeofe_hooks.cpp)
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

# SDL3, for the window and the GPU renderer: from the system if CMake finds
# it, otherwise downloaded and built with the project (zlib license).
option(SPIKEOUT_FRONTEND "Build the spikeout window (SDL3)" ON)
option(SPIKEOUT_GPU "Build the GPU renderer (SDL GPU: Vulkan, Metal)" ON)
include(FetchContent)
if(SPIKEOUT_FRONTEND OR SPIKEOUT_GPU)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
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
endif()

# The GPU renderer. Its shaders (src/gpu/shaders, GLSL) are compiled at build
# time to SPIR-V for Vulkan with glslang (BSD-3-Clause and others) and to
# Metal source with SPIRV-Cross (Apache-2.0), both downloaded as build tools.
if(SPIKEOUT_GPU)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG 16.4.0
        GIT_SHALLOW TRUE)
    set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
    foreach(part GLSL HLSL MSL CPP REFLECT UTIL) # the command-line tool needs them all
        set(SPIRV_CROSS_ENABLE_${part} ON CACHE BOOL "" FORCE)
    endforeach()
    set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG vulkan-sdk-1.4.350.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glslang spirv_cross)

    set(shader_dir ${CMAKE_SOURCE_DIR}/src/gpu/shaders)
    set(shader_out ${CMAKE_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${shader_out})
    # FILE, or FILE@DEFINE@...: a variant built with those defines set,
    # named file_define..._stage (scene.frag@OPAQUE -> scene_opaque_frag).
    set(shaders present.vert present.frag scene.vert scene.frag scene.frag@OPAQUE scene.frag@QUADS
        scene.frag@OPAQUE@QUADS scene.frag@QUADS@BOUNDED scene.frag@OPAQUE@QUADS@BOUNDED background_fog.frag composite.frag screen.frag)
    file(GLOB shader_includes ${shader_dir}/*.glsl)
    set(shader_sources)
    foreach(entry ${shaders})
        string(REPLACE "@" ";" parts ${entry})
        list(GET parts 0 shader)
        string(REGEX REPLACE "\\.(vert|frag)$" "" base ${shader})
        string(REGEX MATCH "(vert|frag)$" stage ${shader})
        set(defines)
        set(name ${base})
        list(REMOVE_AT parts 0)
        foreach(define ${parts})
            list(APPEND defines -D${define})
            string(TOLOWER ${define} lower)
            set(name ${name}_${lower})
        endforeach()
        set(name ${name}_${stage})
        set(spv ${shader_out}/${name}.spv)
        set(msl ${shader_out}/${name}.msl)
        set(cpp ${shader_out}/${name}.cpp)
        add_custom_command(
            OUTPUT ${cpp}
            COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.0 -I${shader_dir} ${defines}
                    -o ${spv} ${shader_dir}/${shader}
            COMMAND $<TARGET_FILE:spirv-cross> ${spv} --msl --msl-version 20100 --msl-decoration-binding --output ${msl}
            COMMAND ${CMAKE_COMMAND} -DNAME=${name} -DSPV=${spv} -DMSL=${msl} -DOUT=${cpp}
                    -P ${CMAKE_SOURCE_DIR}/cmake/embed_shader.cmake
            DEPENDS ${shader_dir}/${shader} ${shader_includes} ${CMAKE_SOURCE_DIR}/cmake/embed_shader.cmake
                    glslang-standalone spirv-cross
            COMMENT "Compiling shader ${name}"
            VERBATIM)
        list(APPEND shader_sources ${cpp})
    endforeach()

    # The in-game menu: RmlUi (MIT) with FreeType (FreeType License) for its
    # text, drawn through RmlUi's SDL GPU renderer (third_party/rmlui_sdl_gpu).
    # Its page, style and fonts (assets/) are embedded in the program.
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-14-2
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(freetype)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()
    set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON) # RmlUi takes the one above
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
    set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(rmlui
        GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
        GIT_TAG 6.3
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(rmlui)

    set(ui_out ${CMAKE_BINARY_DIR}/ui)
    file(MAKE_DIRECTORY ${ui_out})
    set(ui_sources)
    foreach(entry menu_rml:ui/menu.rml menu_rcss:ui/menu.rcss font_medium:fonts/BarlowCondensed-Medium.ttf
                  font_semibold:fonts/BarlowCondensed-SemiBold.ttf font_bold_italic:fonts/BarlowCondensed-BoldItalic.ttf
                  flags_png:ui/flags.png flags_rcss:ui/flags.rcss)
        string(REPLACE ":" ";" parts ${entry})
        list(GET parts 0 name)
        list(GET parts 1 file)
        set(cpp ${ui_out}/${name}.cpp)
        add_custom_command(
            OUTPUT ${cpp}
            COMMAND ${CMAKE_COMMAND} -DNAME=${name} -DIN=${CMAKE_SOURCE_DIR}/assets/${file} -DOUT=${cpp}
                    -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/${file} ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
            COMMENT "Embedding ${file}"
            VERBATIM)
        list(APPEND ui_sources ${cpp})
    endforeach()
    add_library(rmlui_sdl_gpu STATIC third_party/rmlui_sdl_gpu/RmlUi_Renderer_SDL_GPU.cpp)
    target_include_directories(rmlui_sdl_gpu SYSTEM PUBLIC third_party/rmlui_sdl_gpu)
    target_link_libraries(rmlui_sdl_gpu PUBLIC RmlUi::RmlUi SDL3::SDL3)
    if(MSVC)
        target_compile_options(rmlui_sdl_gpu PRIVATE /w)
    else()
        target_compile_options(rmlui_sdl_gpu PRIVATE -w)
    endif()
    add_library(m3ui src/ui/menu.cpp ${ui_sources})
    target_include_directories(m3ui PUBLIC src)
    target_link_libraries(m3ui PUBLIC SDL3::SDL3 PRIVATE rmlui_sdl_gpu RmlUi::RmlUi spikeout_warnings)

    add_library(m3gpu src/gpu/present.cpp src/gpu/real3d_gpu.cpp ${shader_sources})
    target_include_directories(m3gpu PUBLIC src)
    target_link_libraries(m3gpu PUBLIC SDL3::SDL3 m3runtime PRIVATE spikeout_warnings)
endif()

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

    add_executable(spikebot tools/spikebot.cpp)
    target_link_libraries(spikebot PRIVATE game_ram boot_rom spikeout_warnings)

    add_executable(gamerun tools/gamerun.cpp)
    target_link_libraries(gamerun PRIVATE game_ram boot_rom m3net spikeout_warnings)
    if(SPIKEOUT_GPU)
        target_link_libraries(gamerun PRIVATE m3gpu)
        target_compile_definitions(gamerun PRIVATE SPIKEOUT_GPU)
    endif()

    # The game in a window (SDL3, see above); with SPIKEOUT_GPU it draws
    # through the GPU.
    if(SPIKEOUT_FRONTEND)
        add_executable(spikeout tools/spikeout.cpp)
        target_link_libraries(spikeout PRIVATE game_ram boot_rom m3net SDL3::SDL3 spikeout_warnings)
        if(SPIKEOUT_GPU)
            target_link_libraries(spikeout PRIVATE m3gpu m3ui)
            target_compile_definitions(spikeout PRIVATE SPIKEOUT_GPU)
        endif()
    endif()
endif()

if(SPIKEOUT_BUILD_TESTS)
    enable_testing()
    add_executable(spikeout_tests
        tests/test_main.cpp
        tests/test_ppc.cpp
        tests/test_runtime.cpp
        tests/test_analysis.cpp
        tests/test_devices.cpp
        tests/test_net.cpp
        tests/test_snapshots.cpp)
    target_link_libraries(spikeout_tests PRIVATE analysis m3runtime m3net spikeout_warnings)
    if(SPIKEOUT_GPU)
        target_sources(spikeout_tests PRIVATE tests/test_gpu.cpp)
        target_link_libraries(spikeout_tests PRIVATE m3gpu)
    endif()
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
