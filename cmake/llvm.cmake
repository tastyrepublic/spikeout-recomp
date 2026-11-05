# LLVM for the JIT (src/jit): one pinned release, built once from its source
# with this build's own compiler, the same version and options on every
# system. Only what the JIT needs: the libraries for this machine's processor,
# no tools, tests or optional libraries. Kept outside the build directory
# (SPIKEOUT_LLVM_ROOT), so it is built only once per compiler: the first
# configure takes a while (about an hour on a 6-core PC), later ones none.
#
#   spikeout_llvm(<var>) sets <var> to the folder with LLVMConfig.cmake.

set(SPIKEOUT_LLVM_VERSION 18.1.8)
set(SPIKEOUT_LLVM_SHA256 0b58557a6d32ceee97c8d533a59b9212d87e0fc4d2833924eb6c611247db2f2a)
set(SPIKEOUT_LLVM_TARGETS "host" CACHE STRING "LLVM's targets to build (host: this machine's processor)")
if(WIN32 AND DEFINED ENV{LOCALAPPDATA})
    set(llvm_default_root "$ENV{LOCALAPPDATA}/spikeout-llvm")
elseif(DEFINED ENV{XDG_CACHE_HOME})
    set(llvm_default_root "$ENV{XDG_CACHE_HOME}/spikeout-llvm")
else()
    set(llvm_default_root "$ENV{HOME}/.cache/spikeout-llvm")
endif()
set(SPIKEOUT_LLVM_JOBS "" CACHE STRING "Compile jobs for LLVM's build (empty: one a core; fewer for less memory)")
set(SPIKEOUT_LLVM_ROOT "${llvm_default_root}" CACHE PATH "Where the pinned LLVM is downloaded, built and kept")

function(spikeout_llvm out_var)
    # One build per LLVM version, compiler and processor.
    string(REPLACE ";" "-" targets "${SPIKEOUT_LLVM_TARGETS}")
    set(tag "${SPIKEOUT_LLVM_VERSION}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_SYSTEM_PROCESSOR}-${targets}")
    set(root "${SPIKEOUT_LLVM_ROOT}")
    set(install "${root}/${tag}")
    set(config "${install}/lib/cmake/llvm")
    if(EXISTS "${config}/LLVMConfig.cmake")
        set(${out_var} "${config}" PARENT_SCOPE)
        return()
    endif()

    set(tarball "${root}/llvm-project-${SPIKEOUT_LLVM_VERSION}.src.tar.xz")
    set(url "https://github.com/llvm/llvm-project/releases/download/llvmorg-${SPIKEOUT_LLVM_VERSION}/llvm-project-${SPIKEOUT_LLVM_VERSION}.src.tar.xz")
    if(NOT EXISTS "${tarball}")
        message(STATUS "LLVM ${SPIKEOUT_LLVM_VERSION}: downloading its source (about 130 MB)")
        file(DOWNLOAD "${url}" "${tarball}.part" EXPECTED_HASH SHA256=${SPIKEOUT_LLVM_SHA256} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${tarball}.part")
            message(FATAL_ERROR "LLVM's source couldn't be downloaded: ${status}")
        endif()
        file(RENAME "${tarball}.part" "${tarball}")
    endif()

    set(work "${root}/work-${tag}")
    file(REMOVE_RECURSE "${work}")
    message(STATUS "LLVM ${SPIKEOUT_LLVM_VERSION}: unpacking")
    set(top "llvm-project-${SPIKEOUT_LLVM_VERSION}.src")
    file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${work}"
         PATTERNS "${top}/llvm" "${top}/cmake" "${top}/third-party")

    set(args
        -S "${work}/${top}/llvm" -B "${work}/build" -G "${CMAKE_GENERATOR}"
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_INSTALL_PREFIX=${install}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DLLVM_TARGETS_TO_BUILD=${SPIKEOUT_LLVM_TARGETS}"
        -DLLVM_ENABLE_PROJECTS= -DLLVM_ENABLE_RUNTIMES=
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF
        -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DLLVM_ENABLE_BINDINGS=OFF
        -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
        -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF
        -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_RTTI=OFF)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # (GCC 15 and later no longer include <cstdint> through other headers, which LLVM 18's rely on)
        list(APPEND args "-DCMAKE_CXX_FLAGS=-include cstdint")
    endif()
    if(CMAKE_MAKE_PROGRAM)
        list(APPEND args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
        list(APPEND args "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    endif()
    message(STATUS "LLVM ${SPIKEOUT_LLVM_VERSION}: configuring (${tag})")
    execute_process(COMMAND "${CMAKE_COMMAND}" ${args} RESULT_VARIABLE result OUTPUT_FILE "${root}/configure-${tag}.log"
                    ERROR_FILE "${root}/configure-${tag}.log")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "LLVM's configure failed: see ${root}/configure-${tag}.log")
    endif()
    cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
    if(SPIKEOUT_LLVM_JOBS)
        set(cores ${SPIKEOUT_LLVM_JOBS})
    endif()
    message(STATUS "LLVM ${SPIKEOUT_LLVM_VERSION}: building on ${cores} threads, once (log: ${root}/build-${tag}.log)")
    # (--config: Visual Studio's and Xcode's generators pick the build type when building, Debug unless told)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${work}/build" --config Release --target install --parallel ${cores}
                    RESULT_VARIABLE result OUTPUT_FILE "${root}/build-${tag}.log" ERROR_FILE "${root}/build-${tag}.log")
    if(NOT result EQUAL 0 OR NOT EXISTS "${config}/LLVMConfig.cmake")
        message(FATAL_ERROR "LLVM's build failed: see ${root}/build-${tag}.log")
    endif()
    file(REMOVE_RECURSE "${work}")
    message(STATUS "LLVM ${SPIKEOUT_LLVM_VERSION}: built into ${install}")
    set(${out_var} "${config}" PARENT_SCOPE)
endfunction()
