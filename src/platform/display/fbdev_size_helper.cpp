// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __linux__

#include "../../../include/fbdev_size_helper.h"

#include <linux/fb.h>

namespace helix {

FbSize fb_size_from_var_screeninfo(const struct fb_var_screeninfo& vinfo) {
    FbSize size;
    size.width = vinfo.xres;
    size.height = vinfo.yres;
    size.valid = (size.width > 0 && size.height > 0);
    return size;
}

} // namespace helix

#endif // __linux__
