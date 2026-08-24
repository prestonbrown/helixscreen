// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __ANDROID__

#include <string>
#include <utility>
#include <vector>

namespace helix::android {

/// HTTP(S) GET via Android's Java HttpURLConnection (JNI bridge).
/// libhv is built without SSL on Android (no NDK OpenSSL), so we route
/// update-check traffic through the platform TLS stack instead.
///
/// Returns {status_code, body_or_error}. status_code == 0 means the
/// request never reached the server (network, DNS, JNI, or TLS failure)
/// and the second element carries a short error message instead of a body.
std::pair<int, std::string> https_get(const std::string& url, const std::string& user_agent,
                                      const std::string& accept, int timeout_sec);

/// HTTP(S) POST with a raw binary body via Android's Java HttpURLConnection.
/// Used by the debug-bundle upload, which sends gzip-compressed bytes that
/// cannot round-trip through a Java String. Returns {status_code, body_or_error}
/// with the same contract as https_get.
std::pair<int, std::string>
https_post_binary(const std::string& url, const std::vector<unsigned char>& body,
                  const std::string& content_type, const std::string& content_encoding,
                  const std::string& user_agent, const std::string& api_key, int timeout_sec);

} // namespace helix::android

#endif // __ANDROID__
