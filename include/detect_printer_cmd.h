// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "json_fwd.h"
#include "printer_detector.h"
#include "printer_discovery.h"

#include <string>
namespace helix::detect {
std::string format_detect_verdict(const PrinterDetectionResult& result,
                                  const std::string& runner_up_preset);

/// Populate a PrinterDiscovery from already-fetched Moonraker REST JSON responses.
/// Null/missing/wrong-type fields are skipped, never throw. `objects` is the raw
/// array from result.objects (printer/objects/list); `info` is the result object
/// from /printer/info; `cfg` is the result.status object from
/// /printer/objects/query?configfile=settings.
void populate_discovery(helix::PrinterDiscovery& disc, const nlohmann::json& objects,
                        const nlohmann::json& info, const nlohmann::json& cfg);

int run_detect_printer(const std::string& host, int port);
} // namespace helix::detect
