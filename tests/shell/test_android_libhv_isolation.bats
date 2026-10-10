#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Android build must neither read nor write libhv's in-tree generated config.
#
# lib/libhv/hconfig.h and lib/libhv/include/hv/ are written by the desktop
# build's ./configure with WITH_OPENSSL on. Android has no OpenSSL headers, so
# a main-target include path that reaches the source-tree copies compiles
# libhv's OpenSSL backend and tls_trust.cpp's OpenSSL block and dies on
# 'openssl/ssl.h'. The Android build configures libhv from a staging directory
# under the CMake binary dir; these checks pin that wiring.

CMAKE="android/app/jni/CMakeLists.txt"

@test "android configures libhv from the staging dir, not the source tree" {
  run grep -E '^[[:space:]]*add_subdirectory\(\$\{LIBHV_DIR\}' "$CMAKE"
  [ "$status" -ne 0 ]
  grep -qE '^[[:space:]]*add_subdirectory\(\$\{LIBHV_STAGE_DIR\} libhv\)' "$CMAKE"
}

@test "android main target includes libhv only through the staging dir" {
  run grep -nE '\$\{PROJECT_ROOT\}/lib/libhv' "$CMAKE"
  # Only the LIBHV_DIR definition may name the source tree.
  [ "$status" -eq 0 ]
  [ "$(printf '%s\n' "$output" | wc -l)" -eq 1 ]
  printf '%s\n' "$output" | grep -qF 'set(LIBHV_DIR '
  grep -qF '"${LIBHV_STAGE_DIR}/include"' "$CMAKE"
}

@test "staging include dir precedes cpputil and the staging root" {
  inc=$(grep -nF '"${LIBHV_STAGE_DIR}/include"' "$CMAKE" | head -1 | cut -d: -f1)
  cpp=$(grep -nF '"${LIBHV_STAGE_DIR}/cpputil"' "$CMAKE" | head -1 | cut -d: -f1)
  root=$(grep -nE '^[[:space:]]+"\$\{LIBHV_STAGE_DIR\}"[[:space:]]' "$CMAKE" | head -1 | cut -d: -f1)
  [ -n "$inc" ] && [ -n "$cpp" ] && [ -n "$root" ]
  [ "$inc" -lt "$cpp" ]
  [ "$cpp" -lt "$root" ]
}

@test "the staging dir never links the generated hconfig.h or include/" {
  grep -qE 'STREQUAL "hconfig.h" OR _entry STREQUAL "include"' "$CMAKE"
}
