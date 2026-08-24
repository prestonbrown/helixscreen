// SPDX-License-Identifier: GPL-3.0-or-later
//
// STB implementation TU for the audit slice. On Linux the implementations
// live in src/print/thumbnail_processor.cpp, which the slice excludes (libhv
// thread pool). stb_image/stb_image_resize are vendored portable C — real
// code, compiled as-is; only the IMPLEMENTATION host moved.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize.h"
