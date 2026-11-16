// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"
#include "macro_param_cache.h"

#include <string>
#include <unordered_set>

namespace helix::test {

/// The printer's macros read, so the file scan's printer-stopping command
/// check answers instead of reporting it could not run. With no arguments the
/// printer has no macros (none stops it); otherwise @p config is its
/// configfile and @p known_macros the macros Klipper registered. Cleared
/// again on destruction.
struct ScopedMacrosRead {
    explicit ScopedMacrosRead(const nlohmann::json& config = nlohmann::json::object(),
                              const std::unordered_set<std::string>& known_macros = {}) {
        MacroParamCache::instance().populate_from_configfile(config, known_macros);
    }
    ~ScopedMacrosRead() {
        MacroParamCache::instance().clear();
    }
    ScopedMacrosRead(const ScopedMacrosRead&) = delete;
    ScopedMacrosRead& operator=(const ScopedMacrosRead&) = delete;
};

} // namespace helix::test
