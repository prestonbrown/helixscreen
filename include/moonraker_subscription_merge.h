// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

namespace helix {

/// Union of two printer.objects.subscribe objects maps. For an object in both, null (every
/// field) wins; otherwise the field lists are unioned, keeping `app`'s order then `extra`'s
/// new fields. An object only in `extra` is added as given. `app` is never narrowed. A
/// malformed `extra` entry (not null, not an array of strings) is ignored. Pure.
nlohmann::json merge_subscription_objects(const nlohmann::json& app, const nlohmann::json& extra);

} // namespace helix
