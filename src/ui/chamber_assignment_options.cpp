// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file chamber_assignment_options.cpp
 * @brief Implementation of build_chamber_assignment_options()
 */

#include "chamber_assignment_options.h"

namespace helix::settings {

namespace {

/// Drop the Klipper class prefix so the dropdown shows the bare object name.
std::string display_name(const std::string& object, const std::string& prefix) {
    if (!prefix.empty() && object.rfind(prefix, 0) == 0) {
        return object.substr(prefix.size());
    }
    return object;
}

} // namespace

ChamberAssignmentOptions
build_chamber_assignment_options(const std::vector<std::string>& discovered,
                                 const std::string& detected, const std::string& saved,
                                 const std::string& strip_prefix,
                                 const ChamberAssignmentLabels& labels) {
    ChamberAssignmentOptions out;

    out.options = labels.auto_label;
    if (!detected.empty()) {
        out.options += " (" + display_name(detected, strip_prefix) + ")";
    } else {
        out.options += " " + labels.none_detected;
    }

    for (const auto& object : discovered) {
        out.options += "\n" + display_name(object, strip_prefix);
        out.names.push_back(object);
    }

    // Anything that is not one of the two keywords names a specific object.
    const bool assigns_object = !saved.empty() && saved != "auto" && saved != "none";
    size_t assigned = out.names.size();
    if (assigns_object) {
        for (size_t i = 0; i < out.names.size(); i++) {
            if (out.names[i] == saved) {
                assigned = i;
                break;
            }
        }
        if (assigned == out.names.size()) {
            // Appending before the disable option keeps that option at
            // names.size() + 1, which is where the dropdown handler reads it.
            out.options +=
                "\n" + display_name(saved, strip_prefix) + " (" + labels.not_detected + ")";
            out.names.push_back(saved);
        }
    }

    out.options += "\n" + labels.none_disable;

    if (saved == "none") {
        out.selected = static_cast<uint32_t>(out.names.size() + 1);
    } else if (assigns_object) {
        out.selected = static_cast<uint32_t>(assigned + 1);
    } else {
        out.selected = 0;
    }

    return out;
}

} // namespace helix::settings
