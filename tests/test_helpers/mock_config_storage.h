// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config_storage.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace helix::test {

class MockConfigStorage : public helix::ConfigStorage {
  public:
    std::optional<std::string> doc;
    std::string corrupt_stash;
    bool ro = false;
    int store_calls = 0;
    // Simulates "present but unreadable" (e.g. permission denied): load()
    // throws instead of returning nullopt, matching the real
    // ConfigStorage::load() contract — see config_storage.h.
    bool unreadable = false;

    explicit MockConfigStorage(std::optional<std::string> initial = std::nullopt)
        : doc(std::move(initial)) {}

    std::optional<std::string> load() override {
        if (unreadable && doc) {
            throw std::runtime_error("mock: document present but unreadable");
        }
        return doc;
    }
    bool store(const std::string& bytes) override {
        if (ro)
            return false;
        doc = bytes;
        store_calls++;
        return true;
    }
    void preserve_corrupt() override {
        if (doc)
            corrupt_stash = *doc;
        doc.reset();
    }
    bool read_only() override {
        return ro;
    }
    std::string describe() const override {
        return "mock://config";
    }
};

} // namespace helix::test
