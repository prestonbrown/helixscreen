// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Stage a printer's discovered objects and configfile on PrinterState (and
// the macro cache) for one test, and reset both when it ends.

#include "app_globals.h"
#include "json_fwd.h"
#include "macro_param_cache.h"
#include "printer_discovery.h"
#include "printer_state.h"

namespace helix::test {

struct ScopedPrinterDiscovery {
    explicit ScopedPrinterDiscovery(const nlohmann::json& objects,
                                    const nlohmann::json& config = nlohmann::json::object()) {
        PrinterDiscovery hw;
        hw.parse_objects(objects);
        hw.parse_config_keys(config);
        MacroParamCache::instance().populate_from_configfile(config, hw.macros());
        get_printer_state().set_hardware(std::move(hw));
    }
    ~ScopedPrinterDiscovery() {
        MacroParamCache::instance().clear();
        get_printer_state().set_hardware(PrinterDiscovery());
    }
    ScopedPrinterDiscovery(const ScopedPrinterDiscovery&) = delete;
    ScopedPrinterDiscovery& operator=(const ScopedPrinterDiscovery&) = delete;
};

/// A printer whose firmware switches the active spool (the U1 extended
/// firmware's SpoolLink).
inline nlohmann::json spoollink_printer_objects() {
    return nlohmann::json::array({"toolhead", "extruder", "spoollink"});
}

} // namespace helix::test
