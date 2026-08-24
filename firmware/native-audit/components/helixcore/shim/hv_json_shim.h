// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audit shim: hv/json.hpp → the repo's own vendored nlohmann/json.
//
// lib/libhv/include/hv/json.hpp IS nlohmann/json 3.12.0, single-header, verbatim —
// zero libhv platform dependencies. So this is purely an include-path alias to the
// exact header the Linux build compiles; no second vendored copy, no drift.
// The app's lint rule (tests/shell/test_code_lint.bats) guarantees all JSON usage
// goes through hv/json.hpp, so this one alias covers every JSON include in src/.

#pragma once

#include <libhv/include/hv/json.hpp> // via -isystem <repo>/lib
