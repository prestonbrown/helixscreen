// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Forced include for LVGL and common includes
// The build passes `-include include/lvgl_pch.h` to every app and test C++
// source, so everything here is visible without an #include. It is not
// precompiled: each TU parses it, so keep it to headers that are:
// 1. Used frequently across many translation units
// 2. Rarely change (external libraries, stable APIs)
// 3. Heavy to parse (LVGL, STL containers)

#pragma once

// LVGL core headers
#include "helix_features.h"
#include "lvgl/lvgl.h"

// Helix XML engine (extracted from LVGL, standalone since v9.5 removed XML)
#include "helix-xml/helix_xml.h"

// Common STL headers used throughout the project
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// spdlog (used in nearly every file)
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"
