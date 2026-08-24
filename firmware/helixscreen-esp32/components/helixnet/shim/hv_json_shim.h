// SPDX-License-Identifier: GPL-3.0-or-later
//
// hv/json.hpp → the repo's own vendored nlohmann/json. lib/libhv/include/hv/json.hpp
// IS nlohmann/json single-header, verbatim, with zero libhv platform deps.
//
// That header is gitignored upstream in libhv and only appears once libhv is
// BUILT (its CMake fetches nlohmann/json). The desktop build builds libhv so the
// file is present; this firmware shim exists precisely to AVOID building libhv,
// so a fresh checkout has no json.hpp there and the angle-bracket include below
// fails. Vendor the same single-header (nlohmann/json 3.12.0, MIT) alongside the
// shim and reference it directly — byte-identical to what the Linux build
// compiles, so no drift. Keep it in sync with lib/libhv/include/hv/json.hpp when
// that submodule is bumped.

#pragma once

#include "nlohmann/json.hpp"
