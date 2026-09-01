// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_start_profile.h"

#include <memory>
#include <string>

#include "hv/json.hpp"

class PrintStartProfileTestAccess {
  public:
    /// Parse inline JSON into a fresh profile — the private parse_json entry
    /// point, so tests can feed malformed shapes without a file on disk.
    /// Returns nullptr when the profile rejects the JSON outright (missing
    /// 'name'); a tolerated-malformity parse still returns the profile.
    static std::shared_ptr<PrintStartProfile> parse(const nlohmann::json& j,
                                                    const std::string& source = "inline") {
        auto profile = std::make_shared<PrintStartProfile>();
        if (!profile->parse_json(j, source)) {
            return nullptr;
        }
        return profile;
    }
};
