// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

// lodepng.h declares its C++ convenience overloads inside its own extern "C"
// block, so including it from C++ fails to compile. This is the one entry
// point the app uses (built in via LV_USE_LODEPNG). The buffer it returns comes
// from lv_malloc and goes back through lv_free.
//
// Encode to memory, not lodepng_encode32_file(): LVGL routes lodepng's disk I/O
// through lv_fs, which rejects a plain filesystem path for want of a driver
// letter.
// NAMESPACE_OK: a C symbol from LVGL's lodepng
extern "C" unsigned lodepng_encode32(unsigned char** out, size_t* outsize,
                                     const unsigned char* image, unsigned w, unsigned h);
