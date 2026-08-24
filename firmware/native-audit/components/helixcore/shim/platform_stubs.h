// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audit stubs for platform facilities that are legitimately Linux-bound.
//
// AUDIT RULE: every stub added to this pair of files = one row in the
// "Shim/stub categorization" table in docs/devel/ESP32_NATIVE_AUDIT.md.
// Task 2 (compile sweep) needs only the logging funnel below; link-time
// stubs accumulate here during Task 3 (vertical-slice link).

#pragma once

// Funnel for the spdlog shim: keeps esp_log.h (and its macro surface) out of
// every app translation unit. Levels use spdlog numbering (0=trace .. 5=critical).
void helix_shim_log(int level, const char* msg);
