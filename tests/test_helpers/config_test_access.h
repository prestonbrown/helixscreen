// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.h"

namespace helix {

// Test-only accessor for Config private/protected members, so config.h needs
// no per-fixture `friend class` declarations (the friend-TestAccess pattern;
// see tests/test_helpers/*_test_access.h).
class ConfigTestAccess {
  public:
    static json& data(Config& c) {
        return c.data;
    }
    static std::string& path(Config& c) {
        return c.path;
    }
    static std::string& active_printer_id(Config& c) {
        return c.active_printer_id_;
    }
    // Latched by init() when the config dir is unwritable; sticky for the
    // process once set, which would silently disable save() in every later
    // test sharing the singleton.
    static bool& read_only_mode(Config& c) {
        return c.read_only_mode_;
    }
    // Swap the Config singleton pointer (used by the change-host fixture).
    static Config*& instance_ref() {
        return Config::instance;
    }
    // The document store save() writes to. A store swapped in for a scope must
    // also clear storage_is_default, or save() rebuilds the file store over it.
    static std::unique_ptr<ConfigStorage>& storage(Config& c) {
        return c.storage_;
    }
    static bool& storage_is_default(Config& c) {
        return c.storage_is_default_;
    }
};

// Shared replacement for the duplicated per-fixture setup_printer_data helper.
inline void setup_printer_data(Config& config, const json& printer_data) {
    ConfigTestAccess::data(config) = {{"config_version", 3},
                                      {"active_printer_id", "default"},
                                      {"printers", {{"default", printer_data}}}};
    ConfigTestAccess::active_printer_id(config) = "default";
}

} // namespace helix
