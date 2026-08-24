// SPDX-License-Identifier: GPL-3.0-or-later
//
// Funnel for the spdlog shim: keeps esp_log.h (and its macro surface) out of
// every translation unit that includes a repo header. Levels use spdlog
// numbering (0=trace .. 5=critical). Mirrors the Plan 2 native-audit shim
// (firmware/native-audit/components/helixcore/shim/platform_stubs.h) so a file
// that compiles against real spdlog compiles here with identical semantics.

#pragma once

void helix_shim_log(int level, const char* msg);
