// Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Precompiled header for LVGL and common includes
// This header is precompiled to speed up build times (30-50% faster clean builds)
//
// Only include headers that are:
// 1. Used frequently across many translation units
// 2. Rarely change (external libraries, stable APIs)
// 3. Heavy to parse (LVGL, STL containers)

#ifndef LVGL_PCH_H
#define LVGL_PCH_H

// LVGL core headers (processed 200+ times without PCH)
#include "lvgl/lvgl.h"

// Common STL headers used throughout the project
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <cmath>

// spdlog (used in nearly every file)
#include "spdlog/spdlog.h"
#include "spdlog/fmt/fmt.h"

#endif // LVGL_PCH_H
