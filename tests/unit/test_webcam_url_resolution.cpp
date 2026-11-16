// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_webcam_url_resolution.cpp
 * @brief Unit tests for webcam URL resolution and snapshot-endpoint usability.
 *
 * Covers two related discovery fixes:
 *  1. MoonrakerAPI::resolve_webcam_url() port handling — strip the port only for
 *     the Moonraker-direct case (:7125, webcam lives on the :80 reverse proxy);
 *     keep the port for reverse-proxy connections (e.g. a Creality K2 on :4408).
 *  2. helix::is_usable_snapshot_url() — reject HTML viewer pages (/snapshot.html,
 *     /camera.html) so the discovery snapshot fallback never enters a never-decodes
 *     snapshot-only mode (the K2 "iframe" service bug).
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/moonraker_discovery_sequence.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../ui_test_utils.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Helper: build a MoonrakerAPI with an explicit HTTP base URL and resolve a url.
std::string resolve_with_base(const std::string& base, const std::string& url) {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(mock, state);
    api.set_http_base_url(base);
    std::string out = url;
    api.resolve_webcam_url(out);
    return out;
}
} // namespace

// ============================================================================
// resolve_webcam_url — port handling
// ============================================================================

TEST_CASE("resolve_webcam_url strips Moonraker default port 7125", "[api][webcam]") {
    // Direct-to-Moonraker: webcam served by reverse proxy on :80, so port dropped.
    REQUIRE(resolve_with_base("http://192.168.1.112:7125", "/webcam/?action=stream") ==
            "http://192.168.1.112/webcam/?action=stream");
}

TEST_CASE("resolve_webcam_url keeps a non-default reverse-proxy port (K2 :4408)", "[api][webcam]") {
    // Connected via a reverse proxy on :4408 — the frontend serving the webcam is
    // on that same port, so it must be preserved.
    REQUIRE(resolve_with_base("http://192.168.1.74:4408", "/snapshot.html") ==
            "http://192.168.1.74:4408/snapshot.html");
    REQUIRE(resolve_with_base("http://192.168.1.74:4408", "/webcam/?action=snapshot") ==
            "http://192.168.1.74:4408/webcam/?action=snapshot");
}

TEST_CASE("resolve_webcam_url resolves against base when base has no port", "[api][webcam]") {
    REQUIRE(resolve_with_base("http://camera.local", "/webcam/?action=stream") ==
            "http://camera.local/webcam/?action=stream");
}

TEST_CASE("resolve_webcam_url leaves absolute URLs unchanged", "[api][webcam]") {
    const std::string abs = "http://10.0.0.5:8080/?action=stream";
    REQUIRE(resolve_with_base("http://192.168.1.74:4408", abs) == abs);
    // https absolute also untouched.
    const std::string abs_https = "https://cam.example.com/stream.mjpg";
    REQUIRE(resolve_with_base("http://192.168.1.112:7125", abs_https) == abs_https);
}

TEST_CASE("resolve_webcam_url handles empty inputs", "[api][webcam]") {
    // Empty url -> unchanged (empty).
    REQUIRE(resolve_with_base("http://192.168.1.74:4408", "") == "");
    // Empty base -> url unchanged (cannot resolve).
    REQUIRE(resolve_with_base("", "/webcam/?action=stream") == "/webcam/?action=stream");
    // Base with no scheme separator -> prepended as-is (early fallback path).
    REQUIRE(resolve_with_base("garbage-no-scheme", "/webcam") == "garbage-no-scheme/webcam");
}

// ============================================================================
// is_usable_snapshot_url — image vs HTML page
// ============================================================================

TEST_CASE("is_usable_snapshot_url rejects HTML viewer pages", "[discovery][webcam]") {
    // The Creality K2 "iframe" service advertises these — HTML, never a JPEG.
    REQUIRE_FALSE(is_usable_snapshot_url("/snapshot.html"));
    REQUIRE_FALSE(is_usable_snapshot_url("/camera.html"));
    // Case-insensitive.
    REQUIRE_FALSE(is_usable_snapshot_url("/Snapshot.HTML"));
    // Absolute HTML page too.
    REQUIRE_FALSE(is_usable_snapshot_url("http://192.168.1.74:4408/snapshot.html"));
}

TEST_CASE("is_usable_snapshot_url accepts real image endpoints", "[discovery][webcam]") {
    REQUIRE(is_usable_snapshot_url("/webcam/?action=snapshot"));
    REQUIRE(is_usable_snapshot_url("http://127.0.0.1:8080/?action=snapshot"));
    REQUIRE(is_usable_snapshot_url("/snapshot.jpg"));
    REQUIRE(is_usable_snapshot_url("/cam.jpeg"));
    REQUIRE(is_usable_snapshot_url("/frame.PNG"));
}

TEST_CASE("is_usable_snapshot_url rejects empty url", "[discovery][webcam]") {
    REQUIRE_FALSE(is_usable_snapshot_url(""));
}

TEST_CASE("is_usable_snapshot_url does not mistake .html in a query for the path",
          "[discovery][webcam]") {
    // Path ends in image marker; .html only appears in the query string -> usable.
    REQUIRE(is_usable_snapshot_url("/webcam/?action=snapshot&page=foo.html"));
}
