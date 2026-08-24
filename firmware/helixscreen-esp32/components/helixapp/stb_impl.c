/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * STB implementation TU. On Linux the STB_IMAGE / STB_IMAGE_RESIZE
 * implementations are hosted in src/print/thumbnail_processor.cpp, which is a
 * platform seam excluded from this component (libhv HThreadPool). The stb
 * headers are vendored portable C compiled as-is — only the implementation host
 * moved here, mirroring the Plan 2 native-audit (audit_stb_impl.c). Provides
 * stbi_load / stbi_image_free / stbi_info / stbi_failure_reason /
 * stbir_resize_uint8 used by the image-loading path (lvgl_image_writer,
 * prerendered_images, printer_image_manager).
 */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize.h"
