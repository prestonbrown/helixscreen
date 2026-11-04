// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "printer_discovery.h"

#include <string>
#include <vector>

namespace helix::test {

/// A PrinterDiscovery that knows exactly these printer.objects.list names.
inline PrinterDiscovery discovery_of(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json(objects));
    return hw;
}

} // namespace helix::test
