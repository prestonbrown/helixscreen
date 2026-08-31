// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests for AmsBackend::classify_error virtual hook.
// Default returns nullopt; derived backends can override to return a domain ErrorEvent.

#include "../test_helpers/ams_backend_probes.h"
#include "ams_backend_afc.h"
#include "error_event.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

// Minimal fake that overrides classify_error to recognize "FAKE-JAM" lines.
class FakeJamBackend : public AmsBackendAfc {
  public:
    FakeJamBackend() : AmsBackendAfc(nullptr, nullptr) {}

    std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line,
                   const helix::ClassifyContext& /*ctx*/) const override {
        if (raw_line.find("FAKE-JAM") != std::string::npos) {
            helix::ErrorEvent ev;
            ev.source = helix::ErrorSource::AFC;
            ev.severity = helix::ErrorSeverity::CRITICAL;
            ev.title = "AFC Jam";
            ev.detail = raw_line;
            return ev;
        }
        return std::nullopt;
    }
};

TEST_CASE("AmsBackend::classify_error default returns nullopt", "[ams][error-center]") {
    AfcProbe backend;
    helix::ClassifyContext ctx;
    auto result = backend.classify_error("!! Error: something happened", ctx);
    REQUIRE_FALSE(result.has_value());

    // Dispatch through the base reference to prove the virtual resolves through
    // AmsBackend's vtable, not just the concrete type.
    AmsBackend& base = backend;
    REQUIRE_FALSE(base.classify_error("!! anything", ctx).has_value());
}

TEST_CASE("AmsBackend::classify_error override is honored", "[ams][error-center]") {
    FakeJamBackend backend;
    helix::ClassifyContext ctx;

    SECTION("FAKE-JAM line returns CRITICAL AFC event") {
        auto result = backend.classify_error("!! FAKE-JAM xyz toolhead stuck", ctx);
        REQUIRE(result.has_value());
        REQUIRE(result->severity == helix::ErrorSeverity::CRITICAL);
        REQUIRE(result->source == helix::ErrorSource::AFC);

        // Dispatch through the base reference to prove the override is reached
        // via AmsBackend's vtable.
        AmsBackend& base = backend;
        auto e = base.classify_error("!! FAKE-JAM xyz", ctx);
        REQUIRE(e.has_value());
        REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    }

    SECTION("non-jam line returns nullopt") {
        auto result = backend.classify_error("!! Error: heater fault", ctx);
        REQUIRE_FALSE(result.has_value());
    }
}
