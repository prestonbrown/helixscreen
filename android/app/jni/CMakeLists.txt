cmake_minimum_required(VERSION 3.10)
project(HelixScreen)

# Project root is three levels up from android/app/jni/
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# ============================================================================
# SDL2
# ============================================================================
set(SDL2_DIR "${PROJECT_ROOT}/lib/sdl2")
add_subdirectory(${SDL2_DIR} SDL2)

# ============================================================================
# spdlog (header-only)
# ============================================================================
set(SPDLOG_DIR "${PROJECT_ROOT}/lib/spdlog")
# Just add include path, no build needed

# ============================================================================
# libhv (static library)
# ============================================================================
set(LIBHV_DIR "${PROJECT_ROOT}/lib/libhv")
# libhv has its own CMakeLists.txt but we need to configure it for Android
set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)
set(WITH_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${LIBHV_DIR} libhv)

# ============================================================================
# libjpeg-turbo (shared — dlopen'd at runtime by CameraStream)
# ============================================================================
# Android ships no libturbojpeg, so CameraStream's dlopen fails and every MJPEG
# frame falls back to stb_image: scalar IDCT (STBI_NEON is never defined) *and*
# no decode-time downscale, because compute_scaled_size() is gated on
# tjGetScalingFactors. A 1080p stream then costs a full-res scalar decode plus an
# LVGL software rescale per frame (prestonbrown/helixscreen#1245).
#
# ExternalProject_Add rather than add_subdirectory: libjpeg-turbo's top-level
# CMakeLists.txt hard-fails with "cannot be integrated into another build system
# using add_subdirectory()" whenever CMAKE_SOURCE_DIR != its own source dir, and
# tells downstreams to use ExternalProject_Add instead. A separate configure also
# keeps its install rules, CMAKE_INSTALL_PREFIX and C flags out of our build.
include(ExternalProject)
set(LIBJPEG_TURBO_DIR "${PROJECT_ROOT}/lib/libjpeg-turbo")
set(TURBOJPEG_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo")
set(TURBOJPEG_SO "${TURBOJPEG_BINARY_DIR}/libturbojpeg.so")

# Fail the build if SIMD is silently dropped on the ABIs that are the whole point
# of this: a libturbojpeg without SIMD is barely worth shipping. Arm NEON is
# intrinsics-only, so the NDK's clang is the only prerequisite and REQUIRE_SIMD is
# safe. x86-64 needs a separate NASM-syntax assembler; this NDK bundles yasm and
# CMake finds it, so that ABI gets SIMD too — but it is an emulator-only ABI, so
# let it warn and degrade rather than hard-fail on a host where yasm is missing.
if(ANDROID_ABI MATCHES "^arm")
    set(TURBOJPEG_REQUIRE_SIMD 1)
else()
    set(TURBOJPEG_REQUIRE_SIMD 0)
endif()

ExternalProject_Add(turbojpeg_ext
    SOURCE_DIR "${LIBJPEG_TURBO_DIR}"
    BINARY_DIR "${TURBOJPEG_BINARY_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    # Build only the TurboJPEG API shared library — not libjpeg.so, not the
    # bundled tools, not the install tree.
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${TURBOJPEG_SO}"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        # Deliberately NOT ${CMAKE_BUILD_TYPE}: the NDK's Debug config passes no
        # -O flag at all, and -O0 NEON intrinsics are slower than the stb_image
        # path this replaces. Nobody steps through libjpeg-turbo, so pin it
        # optimized and let a debug APK still show the real decode cost.
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_INSTALL_PREFIX=${TURBOJPEG_BINARY_DIR}/install
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -DANDROID_NDK=${ANDROID_NDK}
        # TurboJPEG API only: no static libs, no cjpeg/djpeg/tjbench, no
        # regression tests, no fuzzers, no Java/JNA bindings.
        -DENABLE_SHARED=1
        -DENABLE_STATIC=0
        -DWITH_TURBOJPEG=1
        -DWITH_TOOLS=0
        -DWITH_TESTS=0
        -DWITH_FUZZ=0
        -DWITH_SIMD=1
        -DREQUIRE_SIMD=${TURBOJPEG_REQUIRE_SIMD}
    # AGP's ninja parser forwards compiler diagnostics and drops plain progress
    # output, so a sub-build that streams to the console leaves no record of what
    # it configured. Log to files instead and replay them only if it breaks — the
    # configure log is where "SIMD extensions: ARM64" is confirmed.
    LOG_CONFIGURE 1
    LOG_BUILD 1
    LOG_OUTPUT_ON_FAILURE 1
)

# ============================================================================
# LVGL (static library from source globs)
# ============================================================================
file(GLOB_RECURSE LVGL_SOURCES "${PROJECT_ROOT}/lib/lvgl/src/*.c")
# Also include the C++ thorvg sources
file(GLOB_RECURSE LVGL_CXX_SOURCES "${PROJECT_ROOT}/lib/lvgl/src/*.cpp")

# Exclude XML and expat sources from LVGL — those are now in lib/helix-xml
list(FILTER LVGL_SOURCES EXCLUDE REGEX ".*/xml/.*\\.c$")
list(FILTER LVGL_SOURCES EXCLUDE REGEX ".*/libs/expat/.*\\.c$")

add_library(lvgl STATIC ${LVGL_SOURCES} ${LVGL_CXX_SOURCES})
target_include_directories(lvgl PUBLIC
    "${PROJECT_ROOT}"              # For lv_conf.h at project root
    "${PROJECT_ROOT}/include"      # For lvgl_assert_handler.h
    "${PROJECT_ROOT}/lib/lvgl"     # For lvgl.h
    "${PROJECT_ROOT}/lib/lvgl/src" # For <misc/lv_types.h> etc. — helix-xml's
                                   # public headers include LVGL this way, so
                                   # it must be PUBLIC, not private to lvgl.
                                   # Mirrors -isystem $(LVGL_DIR)/src in LVGL_INC.
)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE HELIX_DISPLAY_SDL)
# LVGL's SDL driver needs SDL headers
target_link_libraries(lvgl PUBLIC SDL2)

# ============================================================================
# Helix XML engine (extracted from LVGL, with our patches baked in)
# ============================================================================
set(HELIX_XML_DIR "${PROJECT_ROOT}/lib/helix-xml")
file(GLOB HELIX_XML_SOURCES
    "${HELIX_XML_DIR}/src/xml/*.c"
    "${HELIX_XML_DIR}/src/xml/parsers/*.c"
    "${HELIX_XML_DIR}/src/libs/expat/*.c"
)
add_library(helix_xml STATIC ${HELIX_XML_SOURCES})
target_include_directories(helix_xml PUBLIC
    "${HELIX_XML_DIR}"             # For helix_xml.h
    "${HELIX_XML_DIR}/src"         # For xml/*.h internal headers
)
target_include_directories(helix_xml PRIVATE
    "${PROJECT_ROOT}"              # For lv_conf.h
    "${PROJECT_ROOT}/include"      # For lvgl_assert_handler.h
)
# lib/lvgl and lib/lvgl/src arrive transitively as PUBLIC includes of the lvgl
# target linked below — helix-xml's own headers need them, so they cannot be
# private here.
target_compile_definitions(helix_xml PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(helix_xml PUBLIC lvgl)

# ============================================================================
# lv_markdown (static library)
# ============================================================================
set(LV_MARKDOWN_DIR "${PROJECT_ROOT}/lib/lv_markdown")
file(GLOB LV_MARKDOWN_SOURCES
    "${LV_MARKDOWN_DIR}/src/*.c"
    "${LV_MARKDOWN_DIR}/deps/md4c/md4c.c"
)
add_library(lv_markdown STATIC ${LV_MARKDOWN_SOURCES})
target_include_directories(lv_markdown PUBLIC
    "${LV_MARKDOWN_DIR}/src"
    "${LV_MARKDOWN_DIR}/deps/md4c"
    "${PROJECT_ROOT}"
    "${PROJECT_ROOT}/include"          # For lvgl_assert_handler.h
    "${PROJECT_ROOT}/lib/lvgl"
)
target_compile_definitions(lv_markdown PUBLIC LV_CONF_INCLUDE_SIMPLE)

# ============================================================================
# HelixScreen main library
# ============================================================================
file(GLOB_RECURSE HELIX_SOURCES "${PROJECT_ROOT}/src/*.cpp")

# Exclude files not needed on Android
list(FILTER HELIX_SOURCES EXCLUDE REGEX ".*/test_.*\\.cpp$")
list(FILTER HELIX_SOURCES EXCLUDE REGEX ".*/helix_splash\\.cpp$")
list(FILTER HELIX_SOURCES EXCLUDE REGEX ".*/helix_watchdog\\.cpp$")
list(FILTER HELIX_SOURCES EXCLUDE REGEX ".*/lvgl-demo/.*")
list(FILTER HELIX_SOURCES EXCLUDE REGEX ".*/bluetooth/.*\\.cpp$")
# Remote-control subsystem (helixctl server + folded REPL client) is dev/test-only.
# The Makefile ships it just for PLATFORM_TARGET=native (ENABLE_REMOTE_CONTROL); Android
# never defines HELIX_ENABLE_REMOTE_CONTROL, so every call site is #ifdef'd out and these
# translation units are dead weight — remote_client.cpp additionally needs lib/linenoise.
list(FILTER HELIX_SOURCES EXCLUDE REGEX ".*/remote/.*\\.cpp$")

# Also exclude Objective-C++ files (.mm) - they're macOS only
# (CMake won't glob them with *.cpp anyway, but be explicit)

# Glob font sources
file(GLOB FONT_SOURCES "${PROJECT_ROOT}/assets/fonts/*.c")

# Glob generated translation source
file(GLOB TRANS_SOURCES "${PROJECT_ROOT}/src/generated/*.c")

# Quirc QR decoder library (vendored)
file(GLOB QUIRC_SOURCES "${PROJECT_ROOT}/lib/quirc/lib/*.c")

add_library(main SHARED ${HELIX_SOURCES} ${FONT_SOURCES} ${TRANS_SOURCES} ${QUIRC_SOURCES})

# Generated git-hash header, required by src/system/helix_version.cpp. Gradle
# never runs the Makefile, so run the Makefile's own generator from here rather
# than reimplementing it: the header's format and its only-rewrite-on-change
# behaviour then have exactly one author. BUILD_DIR points at the CMake binary
# dir so `make clean` and a Gradle build cannot pull the header out from under
# each other, and that dir is searched ahead of ${PROJECT_ROOT}/build/generated
# below, or a Makefile build's leftover copy would supply a hash from whenever
# that build last ran. Configure-time only: the hash goes stale until the next
# reconfigure, which is what "android-dev" used to say outright.
set(HELIX_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            bash "${PROJECT_ROOT}/scripts/gen-git-hash.sh"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE GIT_HASH_RESULT
)
if(NOT GIT_HASH_RESULT EQUAL 0)
    message(FATAL_ERROR "gen-git-hash.sh failed (${GIT_HASH_RESULT}); "
                        "src/system/helix_version.cpp cannot compile without it")
endif()

target_include_directories(main PRIVATE
    "${HELIX_GENERATED_DIR}"            # For helix_git_hash.h (generated above)
    "${PROJECT_ROOT}/include"
    "${PROJECT_ROOT}"
    "${PROJECT_ROOT}/lib"                   # For lvgl/lvgl.h, spdlog/spdlog.h etc.
    "${PROJECT_ROOT}/lib/lvgl"              # For direct lvgl includes
    "${PROJECT_ROOT}/lib/lvgl/src"          # For drivers/sdl/lv_sdl_window.h etc.
    "${PROJECT_ROOT}/lib/libhv/include"
    "${PROJECT_ROOT}/lib/libhv/cpputil"     # For json.hpp (nlohmann)
    "${PROJECT_ROOT}/lib/libhv"             # For hv/ includes
    "${PROJECT_ROOT}/lib/spdlog/include"
    "${PROJECT_ROOT}/lib/stb" # For stb_image.h
    "${PROJECT_ROOT}/lib/glm"
    "${PROJECT_ROOT}/lib/lv_markdown/src"
    "${PROJECT_ROOT}/lib/lv_markdown/deps/md4c"
    "${PROJECT_ROOT}/lib/mdns"
    "${PROJECT_ROOT}/lib/quirc/lib"
    "${PROJECT_ROOT}/lib/cpp-terminal/include"
    "${PROJECT_ROOT}/assets/fonts"
    "${PROJECT_ROOT}/src/generated"
    "${PROJECT_ROOT}/build/generated"   # For contributors.h (from gen-contributors.sh)
    "${PROJECT_ROOT}/lib/helix-xml"     # For helix_xml.h
    "${PROJECT_ROOT}/lib/helix-xml/src" # For xml/*.h internal headers
)

# Read version from VERSION.txt
file(READ "${PROJECT_ROOT}/VERSION.txt" HELIX_VERSION)
string(STRIP "${HELIX_VERSION}" HELIX_VERSION)
string(REPLACE "." ";" VERSION_PARTS "${HELIX_VERSION}")
list(GET VERSION_PARTS 0 HELIX_VERSION_MAJOR)
list(GET VERSION_PARTS 1 HELIX_VERSION_MINOR)
list(GET VERSION_PARTS 2 HELIX_VERSION_PATCH)

target_compile_definitions(main PRIVATE
    HELIX_DISPLAY_SDL
    HELIX_PLATFORM_ANDROID
    HELIX_HAS_SOUND
    HELIX_HAS_TRACKER
    # This build bypasses the Makefile, so its `?= 1` feature defaults do not
    # apply here — an undefined macro in `#if` is 0 and silently selects the
    # stub implementation.
    HELIX_HAS_GCODE_VIEWER=1
    HELIX_HAS_BED_MESH_3D=1
    HELIX_HAS_PLUGINS=1
    HELIX_HAS_TIMELAPSE_VIEWER=1
    ENABLE_GLES_3D
    $<$<CONFIG:Debug>:HELIX_ENABLE_MOCKS>
    LV_CONF_INCLUDE_SIMPLE
    HELIX_VERSION="${HELIX_VERSION}"
    HELIX_VERSION_MAJOR=${HELIX_VERSION_MAJOR}
    HELIX_VERSION_MINOR=${HELIX_VERSION_MINOR}
    HELIX_VERSION_PATCH=${HELIX_VERSION_PATCH}
)

target_compile_options(main PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
    -Wall
    -Wno-unused-function
)

# Use the same precompiled header as the Makefile build — it force-includes
# lvgl.h, helix_xml.h, spdlog, and common STL headers into every TU.
target_precompile_headers(main PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${PROJECT_ROOT}/include/lvgl_pch.h>"
)

# Link everything
find_library(log-lib log)
find_library(z-lib z)
target_link_libraries(main
    SDL2
    lvgl
    helix_xml
    lv_markdown
    hv_static
    GLESv2
    ${log-lib}
    ${z-lib}
    android
)

# libturbojpeg is loaded with dlopen(), never linked, so nothing above pulls it
# into the APK — stage it by hand into CMAKE_LIBRARY_OUTPUT_DIRECTORY, AGP's
# per-ABI obj dir, which is what mergeNativeLibs collects and packages.
#
# This is a standalone target, NOT a POST_BUILD hook on `main`: mergeNativeLibs
# re-syncs its output from the obj dir every build, so a copy that only fires
# when libmain.so happens to relink silently drops the library out of the APK on
# any incremental build that leaves `main` up to date.
#
# CMake's Android platform sets CMAKE_PLATFORM_NO_VERSIONED_SONAME, so
# libjpeg-turbo's VERSION/SOVERSION collapse to a plain "libturbojpeg.so" — the
# APK packager only accepts unversioned lib*.so names, never libturbojpeg.so.0.
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    message(FATAL_ERROR
        "CMAKE_LIBRARY_OUTPUT_DIRECTORY is unset — libturbojpeg.so has nowhere to be "
        "staged and would be missing from the APK. AGP normally supplies it.")
endif()
set(TURBOJPEG_STAGED "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libturbojpeg.so")
add_custom_command(
    OUTPUT "${TURBOJPEG_STAGED}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TURBOJPEG_SO}" "${TURBOJPEG_STAGED}"
    DEPENDS turbojpeg_ext "${TURBOJPEG_SO}"
    COMMENT "Staging libturbojpeg.so for packaging"
    VERBATIM
)
add_custom_target(turbojpeg_staged ALL DEPENDS "${TURBOJPEG_STAGED}")
# AGP builds the named library targets, not `all`, so this has to hang off main.
add_dependencies(main turbojpeg_staged)
