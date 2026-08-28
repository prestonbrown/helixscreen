// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file bed_mesh_profile_name.h
/// @brief What a name typed into a bed-mesh profile field actually means.
///
/// Every profile-name field in the bed-mesh panel used to read the textarea
/// back and substitute the literal string "default" whenever it came back
/// empty. Tapping Save without typing therefore replaced whatever mesh was
/// stored under `default` -- no confirmation, no warning, and no sign in the
/// success toast that a name the user never chose had been used
/// (prestonbrown/helixscreen#1360).
///
/// The three fields (calibrate, save, rename) each re-implemented that read,
/// and none of them trimmed, so a field holding only spaces produced a profile
/// literally named "  ". This is the one place that decides, so a field added
/// later cannot get its own answer.
///
/// Policy stays with the caller: this reports which of three situations the
/// user is in and hands back the name it would use.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace helix {
namespace ui {
namespace bed_mesh {

/// The three outcomes of reading a profile-name field.
enum class ProfileNameVerdict {
    Empty,     ///< Nothing usable was typed. Reject, and say why.
    New,       ///< No stored profile answers to this name. Proceed.
    Overwrite, ///< A stored profile would be replaced. Confirm first.
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
