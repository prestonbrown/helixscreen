// SPDX-License-Identifier: GPL-3.0-or-later
// Alias: bare "json.hpp" → repo's vendored nlohmann (some repo headers use the
// bare form; the Linux build resolves it via -isystem lib/libhv/cpputil).
#pragma once
#include "../hv_json_shim.h"
