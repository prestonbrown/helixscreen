// SPDX-License-Identifier: GPL-3.0-or-later
// Audit alias: bare "json.hpp" → repo's vendored nlohmann. The Linux build
// resolves this via -isystem lib/libhv/cpputil (include/unit_conversions.h and
// friends use the bare form); same include-path fix as <hv/json.hpp>.
#pragma once
#include "../hv_json_shim.h"
