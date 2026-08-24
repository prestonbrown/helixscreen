// SPDX-License-Identifier: GPL-3.0-or-later

#include "qr_decoder.h"
#include "snapshot_qr_scanner.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// SnapshotQrScanner lifecycle tests
// ============================================================================

TEST_CASE("SnapshotQrScanner lifecycle", "[qr]") {
    SECTION("default state is not running") {
        SnapshotQrScanner scanner;
        REQUIRE_FALSE(scanner.is_running());
    }

    SECTION("stop without start is safe") {
        SnapshotQrScanner scanner;
        scanner.stop();
        REQUIRE_FALSE(scanner.is_running());
    }

    SECTION("double stop is safe") {
        SnapshotQrScanner scanner;
        scanner.stop();
        scanner.stop();
        REQUIRE_FALSE(scanner.is_running());
    }
}

// Tagged [slow] because it spawns a real poll thread against a non-routable IP.
// Excluded from parallel test shards (make test-run uses ~[slow]).
// Run separately: ./build/bin/helix-tests "[slow]"
TEST_CASE("SnapshotQrScanner destructor stops cleanly", "[qr][slow]") {
    auto scanner = std::make_unique<SnapshotQrScanner>();
    // Target localhost on a closed port so the HTTP GET fails *fast* and
    // deterministically (instant ECONNREFUSED) on every host. The previous
    // RFC 5737 TEST-NET address (192.0.2.1) is non-routable, but networks that
    // black-hole it (drop the SYN rather than reject it) make the poll thread's
    // connect block for the full HTTP_TIMEOUT_SEC — the destructor join then
    // stalls up to 10s. CI happens to reject it quickly; a closed localhost port
    // guarantees the fast path everywhere. The test still exercises what matters:
    // the scanner starts, reports running, and the destructor joins the poll
    // thread cleanly even with a fetch in flight.
    scanner->start("http://127.0.0.1:1/snapshot", [](lv_draw_buf_t*) {}, [](int) {}, nullptr);
    REQUIRE(scanner->is_running());
    scanner.reset(); // destructor should join the poll thread promptly
}

// ============================================================================
// QrDecoder spoolman ID parsing tests
// ============================================================================

TEST_CASE("QrDecoder spoolman ID parsing", "[qr]") {
    SECTION("web+spoolman format") {
        REQUIRE(QrDecoder::parse_spoolman_id("web+spoolman:s-42") == 42);
        REQUIRE(QrDecoder::parse_spoolman_id("web+spoolman:s-1") == 1);
        REQUIRE(QrDecoder::parse_spoolman_id("web+spoolman:s-999") == 999);
    }

    SECTION("SM:SPOOL format") {
        REQUIRE(QrDecoder::parse_spoolman_id("SM:SPOOL=7") == 7);
        REQUIRE(QrDecoder::parse_spoolman_id("SM:SPOOL=123") == 123);
    }

    SECTION("URL format") {
        REQUIRE(QrDecoder::parse_spoolman_id("http://spoolman.local/spool/5") == 5);
        REQUIRE(QrDecoder::parse_spoolman_id("https://example.com/api/v1/spool/99/info") == 99);
    }

    SECTION("invalid formats return -1") {
        REQUIRE(QrDecoder::parse_spoolman_id("random text") == -1);
        REQUIRE(QrDecoder::parse_spoolman_id("") == -1);
        REQUIRE(QrDecoder::parse_spoolman_id("web+spoolman:s-") == -1);
        REQUIRE(QrDecoder::parse_spoolman_id("SM:SPOOL=abc") == -1);
    }
}
