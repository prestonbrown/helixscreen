// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lan_sweep.h"

#include <map>
#include <string>
#include <vector>

namespace helix::test {

/// Stands in for the network under every MoonrakerConnectionTest while alive: a probe that
/// answers from `answers` ("host:port" -> what it says) and fails with `failure` elsewhere,
/// and a worker that runs the test inline, so its result is one UpdateQueue drain away.
class ScopedFakeConnectionTest {
  public:
    ScopedFakeConnectionTest() {
        set_connection_test_seams_for_test(
            [this](const std::string& host, uint16_t port, const std::string& api_key) {
                const std::string key = host + ":" + std::to_string(port);
                asked.push_back(key);
                keys_sent.push_back(api_key);
                if (const auto it = accepted_keys.find(key); it != accepted_keys.end()) {
                    MoonrakerProbe found(true);
                    found.login_required = api_key != it->second;
                    return found;
                }
                const auto it = answers.find(key);
                if (it != answers.end()) {
                    return it->second;
                }
                MoonrakerProbe none;
                none.failure = failure;
                return none;
            },
            [](size_t, const LanSweepLimits&, std::function<void()> body) {
                body();
                return LanSweepWorkers{nullptr, 1};
            });
    }
    ~ScopedFakeConnectionTest() {
        set_connection_test_seams_for_test(nullptr, nullptr);
    }
    ScopedFakeConnectionTest(const ScopedFakeConnectionTest&) = delete;
    ScopedFakeConnectionTest& operator=(const ScopedFakeConnectionTest&) = delete;

    /// Moonraker at "host:port", answering this.
    void answer(const std::string& address, const std::string& klippy_state = "") {
        MoonrakerProbe found(true);
        found.klippy_state = klippy_state;
        answers[address] = found;
    }

    /// Moonraker behind its login at "host:port": it accepts only this key.
    void require_key(const std::string& address, const std::string& key) {
        accepted_keys[address] = key;
    }

    std::map<std::string, MoonrakerProbe> answers;
    std::map<std::string, std::string> accepted_keys;
    ConnectFailureKind failure = ConnectFailureKind::Unknown;
    std::vector<std::string> asked;     ///< "host:port", in order
    std::vector<std::string> keys_sent; ///< The API key each ask carried, "" for none
};

} // namespace helix::test
