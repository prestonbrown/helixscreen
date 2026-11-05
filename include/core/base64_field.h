// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix {

/// Base64-decode with validation: canonical length, alphabet only, '=' confined
/// to the trailing pad. Invalid input decodes to an empty string, never an error.
std::string decode_b64_field(const std::string& encoded);

/// Base64 of @p raw, any bytes; "" for "".
std::string encode_b64_field(const std::string& raw);

} // namespace helix
