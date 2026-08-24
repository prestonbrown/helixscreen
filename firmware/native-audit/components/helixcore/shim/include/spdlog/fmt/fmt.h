// SPDX-License-Identifier: GPL-3.0-or-later
// Audit alias: <spdlog/fmt/fmt.h> → the repo's bundled fmt (header-only), same as
// real spdlog does when SPDLOG_FMT_EXTERNAL is unset. Routed through the shim so
// fmt configuration (FMT_HEADER_ONLY) stays in one place.
#pragma once
#include "../../../spdlog_shim.h"
