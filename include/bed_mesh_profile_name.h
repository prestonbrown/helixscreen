// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file bed_mesh_profile_name.h
/// @brief What a name typed into a bed-mesh profile field actually means.
///
/// An empty field must not silently become "default": tapping Save without
/// typing would replace whatever mesh is stored under `default` with no
/// confirmation, no warning, and no sign in the success toast that a name the
/// user never chose was used (#1360). Input is trimmed, so a field holding only
/// spaces never produces a profile literally named "  ".
///
/// The three fields (calibrate, save, rename) all ask here, so a field added
/// later cannot get its own answer.
///
/// Policy stays with the caller: this reports which situation the user is
/// in and hands back the name it would use.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace helix {
namespace ui {
namespace bed_mesh {

/// The outcomes of reading a profile-name field.
enum class ProfileNameVerdict {
    Empty,     ///< Nothing usable was typed. Reject, and say why.
    New,       ///< No stored profile answers to this name. Proceed.
    Overwrite, ///< A stored profile would be replaced. Confirm first.
    Unusable,  ///< Holds ';' or a line break, where Klipper cuts a command. Reject, and say why.
};

struct ProfileNameCheck {
    ProfileNameVerdict verdict = ProfileNameVerdict::Empty;
    std::string name; ///< Trimmed. Empty exactly when verdict is Empty.
};

/// Trim surrounding whitespace off @p raw and classify it against the profiles
/// the printer already holds.
///
/// Matching is exact and case-sensitive because Klipper's profile names are:
/// `BED_MESH_PROFILE SAVE=Default` and `SAVE=default` are two profiles, so
/// treating them as a clash would refuse a save Klipper would have accepted.
///
/// @param raw       Field contents as read back, may be empty or whitespace.
/// @param existing  Profile names the printer currently stores.
ProfileNameCheck check_profile_name(std::string_view raw, const std::vector<std::string>& existing);

} // namespace bed_mesh
} // namespace ui
} // namespace helix
