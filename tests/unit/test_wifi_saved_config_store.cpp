// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_dir_guard.h"
#include "data_root_resolver.h"
#include "wifi_saved_config.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "../catch_amalgamated.hpp"

/**
 * A HelixScreen-owned credential store, independent of wpa_supplicant's own
 * config file.
 *
 * A real Adventurer 5M logged a SAVE_CONFIG that replied "OK" and still never
 * reached wpa_supplicant's config file — with no symlink in sight for
 * mirror_to_persistent() to copy onto. We cannot tell, without the device in
 * hand, whether the wrong daemon's config was verified or the write landed
 * somewhere non-persistent. This store sidesteps the question: every
 * successful connect is recorded here too, so a startup reconciliation pass
 * can restore it into wpa_supplicant regardless of what happened to the
 * vendor's own file.
 *
 * These tests pin: round-trip, dedup-by-SSID on save, removal, exact file
 * mode, graceful handling of a missing/corrupt file, and that SSIDs
 * containing quote/backslash characters (rejected by wpa_supplicant's own
 * command protocol, but legal as SSIDs) survive the JSON round trip intact.
 */

using helix::ConfigDirGuard;
using helix::wifi::store::SavedNetwork;

namespace {

/// RAII spdlog capture (mirrors tests/unit/test_widget_helpers.cpp's
/// LogCapture): swaps in a logger that writes formatted messages only (no
/// timestamp/level noise) into an in-memory buffer, restoring the previous
/// default logger on scope exit. Used here to prove a secret never appears
/// in ANY log line produced while it is in scope, at any level — the capture
/// logger is set to trace, the most permissive level a real build could ever
/// run at.
class LogCapture {
  public:
    LogCapture() {
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured_);
        sink->set_pattern("%v");

        capture_logger_ = std::make_shared<spdlog::logger>("test_capture", sink);
        capture_logger_->set_level(spdlog::level::trace);

        original_logger_ = spdlog::default_logger();
        spdlog::set_default_logger(capture_logger_);
    }

    ~LogCapture() {
        spdlog::set_default_logger(original_logger_);
    }

    std::string get_captured() const {
        return captured_.str();
    }

    bool contains(const std::string& text) const {
        return captured_.str().find(text) != std::string::npos;
    }

  private:
    std::ostringstream captured_;
    std::shared_ptr<spdlog::logger> capture_logger_;
    std::shared_ptr<spdlog::logger> original_logger_;
};

} // namespace

TEST_CASE("A saved network round-trips through the store", "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_roundtrip");

    CHECK(helix::wifi::store::load().empty());

    REQUIRE(helix::wifi::store::save({"MyHomeNet", "supersecretpsk"}));

    auto nets = helix::wifi::store::load();
    REQUIRE(nets.size() == 1);
    CHECK(nets[0].ssid == "MyHomeNet");
    CHECK(nets[0].psk == "supersecretpsk");
}

TEST_CASE("Saving an existing SSID replaces rather than duplicates",
          "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_dedup");

    REQUIRE(helix::wifi::store::save({"MyHomeNet", "oldpassword"}));
    REQUIRE(helix::wifi::store::save({"MyHomeNet", "newpassword"}));
    REQUIRE(helix::wifi::store::save({"OtherNet", "otherpass"}));

    auto nets = helix::wifi::store::load();
    REQUIRE(nets.size() == 2);

    bool found_home = false;
    for (const auto& n : nets) {
        if (n.ssid == "MyHomeNet") {
            found_home = true;
            CHECK(n.psk == "newpassword"); // replaced, not appended
        }
    }
    CHECK(found_home);
}

TEST_CASE("Removing a network deletes it from the store", "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_remove");

    REQUIRE(helix::wifi::store::save({"NetA", "pskA"}));
    REQUIRE(helix::wifi::store::save({"NetB", "pskB"}));

    REQUIRE(helix::wifi::store::remove("NetA"));

    auto nets = helix::wifi::store::load();
    REQUIRE(nets.size() == 1);
    CHECK(nets[0].ssid == "NetB");

    // Removing an SSID that was never present is not an error — the store
    // already reflects the desired end state.
    CHECK(helix::wifi::store::remove("NeverSaved"));
    CHECK(helix::wifi::store::load().size() == 1);
}

TEST_CASE("The store file mode is exactly 0600", "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_mode");

    REQUIRE(helix::wifi::store::save({"NetA", "pskA"}));

    const std::string path = helix::wifi::store::store_path();
    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);

    // Assert the ACTUAL mode bits, not just that save() ran. 0600 = rw for
    // the owner, nothing for group or other — this file holds every PSK on
    // the device.
    CHECK((st.st_mode & 07777) == 0600);
}

TEST_CASE("load() on a missing file returns empty, not an exception",
          "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_missing");

    // Nothing has been saved yet — the file does not exist at all.
    CHECK(helix::wifi::store::load().empty());
}

TEST_CASE("load() on a corrupt file returns empty, not an exception",
          "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_corrupt");

    const std::string path = helix::wifi::store::store_path();
    {
        std::ofstream out(path);
        out << "{ this is not valid json [[[";
    }

    CHECK(helix::wifi::store::load().empty());

    // A corrupt file also must not crash a subsequent save().
    CHECK(helix::wifi::store::save({"NetA", "pskA"}));
    auto nets = helix::wifi::store::load();
    REQUIRE(nets.size() == 1);
    CHECK(nets[0].ssid == "NetA");
}

TEST_CASE("load() on a JSON file that is not an array returns empty",
          "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_wrong_shape");

    const std::string path = helix::wifi::store::store_path();
    {
        std::ofstream out(path);
        out << R"({"ssid": "NotAnArray"})";
    }

    CHECK(helix::wifi::store::load().empty());
}

TEST_CASE("An SSID containing a quote or backslash round-trips intact",
          "[network][wifi][savedconfig][store]") {
    // wpa_supplicant's own SET_NETWORK protocol rejects these characters (see
    // validate_wpa_string() in wifi_backend_wpa_supplicant.cpp) because they
    // would break out of the quoted command string. That restriction belongs
    // to the wire protocol, not to the store: a network legitimately named
    // `Joe's "Cafe"` or `back\slash` must still survive being written to and
    // read back from our own JSON file untouched.
    ConfigDirGuard guard("store_escaping");

    const std::string quoted_ssid = R"(Joe's "Cafe" WiFi)";
    const std::string backslash_ssid = R"(back\slash\net)";
    const std::string tricky_psk = R"(p"a\s\sw"ord)";

    REQUIRE(helix::wifi::store::save({quoted_ssid, tricky_psk}));
    REQUIRE(helix::wifi::store::save({backslash_ssid, "plainpass"}));

    auto nets = helix::wifi::store::load();
    REQUIRE(nets.size() == 2);

    bool found_quoted = false, found_backslash = false;
    for (const auto& n : nets) {
        if (n.ssid == quoted_ssid) {
            found_quoted = true;
            CHECK(n.psk == tricky_psk);
        }
        if (n.ssid == backslash_ssid) {
            found_backslash = true;
            CHECK(n.psk == "plainpass");
        }
    }
    CHECK(found_quoted);
    CHECK(found_backslash);
}

TEST_CASE("The store path lives under the user config dir", "[network][wifi][savedconfig][store]") {
    ConfigDirGuard guard("store_path");

    CHECK(helix::wifi::store::store_path() == (guard.dir / "wifi_networks.json").string());
}

TEST_CASE("A file truncated mid-PSK does not leak the PSK into the log",
          "[network][wifi][savedconfig][store]") {
    // nlohmann's parse_error::what() embeds a "last read: '<token>'" fragment
    // — the raw text the lexer had buffered when it hit end of input. If the
    // on-disk store is ever truncated or corrupted partway through a "psk"
    // string value (the exact failure class this feature exists to work
    // around: the reporting device's own vendor atomic-write already proved
    // unreliable), that fragment IS a chunk of a real cleartext password.
    // parse_store()'s catch block must never pass e.what() to spdlog.
    ConfigDirGuard guard("store_truncated_psk");

    const std::string secret_psk = "TotallySecretPassphrase987654XYZ";
    const std::string path = helix::wifi::store::store_path();
    {
        std::ofstream out(path, std::ios::binary);
        // Well-formed up to and including the opening quote of the psk
        // value, then cut off mid-string — no closing quote, no closing
        // braces or brackets. The lexer is scanning the psk string literal
        // itself at the moment it hits EOF.
        out << "[\n  {\n    \"ssid\": \"TestNet\",\n    \"psk\": \"" << secret_psk;
    }

    LogCapture log;
    const auto nets = helix::wifi::store::load();
    CHECK(nets.empty()); // corrupt file still degrades to "no networks", not a throw

    INFO("Captured log: " << log.get_captured());
    CHECK_FALSE(log.contains(secret_psk));
    // Guard against a partial-token leak too, not just the full string — the
    // lexer's buffered fragment need not be the whole value to still be
    // dangerous.
    CHECK_FALSE(log.contains(secret_psk.substr(secret_psk.size() - 10)));
}

TEST_CASE("Non-ASCII and invalid-UTF-8 SSID bytes do not crash save()/load()",
          "[network][wifi][savedconfig][store]") {
    // Two distinct cases:
    //   1. Genuinely non-ASCII but VALID UTF-8 (e.g. an SSID with accented or
    //      CJK characters) — must round-trip byte-for-byte, same as any other
    //      legal SSID.
    //   2. Invalid UTF-8 (a raw high byte that starts a multi-byte sequence
    //      but is never completed) — real routers broadcast arbitrary octet
    //      SSIDs with no UTF-8 guarantee, and validate_wpa_string()'s
    //      `c < 32` control-character check only rejects high bytes when
    //      `char` is signed (not guaranteed — GCC on ARM defaults it
    //      unsigned), so such bytes CAN reach store::save(). nlohmann's
    //      dump() defaults to throwing on this, which would unwind out of
    //      connect_network() and permanently hang the connect UI. It must not
    //      throw — and write_store()'s escape_bytes() encoding means exact
    //      byte preservation IS required (and achieved) even for invalid
    //      UTF-8: see the M12 regression test below for why silently mangling
    //      it (the old U+FFFD-substitution behaviour) is itself a bug, not an
    //      acceptable fallback.
    ConfigDirGuard guard("store_utf8");

    const std::string valid_non_ascii_ssid =
        "Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac WiFi"; // "Café 日本 WiFi"
    REQUIRE(helix::wifi::store::save({valid_non_ascii_ssid, "plainpass"}));
    {
        auto nets = helix::wifi::store::load();
        REQUIRE(nets.size() == 1);
        CHECK(nets[0].ssid == valid_non_ascii_ssid); // valid UTF-8 is untouched
    }

    // \xFF is not valid UTF-8 in any position; \xC0 starts a 2-byte sequence
    // that is never completed. Neither save() nor the subsequent load() may
    // throw or crash.
    const std::string invalid_utf8_ssid = std::string("BadRouter") + "\xff" + "\xc0" + "Net";
    bool save_threw = false;
    bool save_ok = false;
    try {
        save_ok = helix::wifi::store::save({invalid_utf8_ssid, "otherpass"});
    } catch (...) {
        save_threw = true;
    }
    CHECK_FALSE(save_threw);
    CHECK(save_ok);

    std::vector<SavedNetwork> nets;
    bool load_threw = false;
    try {
        nets = helix::wifi::store::load();
    } catch (...) {
        load_threw = true;
    }
    CHECK_FALSE(load_threw);

    // The valid-UTF-8 network from the first save must still be intact —
    // sanitising the invalid entry must not corrupt the rest of the file.
    bool found_valid = false, found_invalid = false;
    for (const auto& n : nets) {
        if (n.ssid == valid_non_ascii_ssid)
            found_valid = true;
        // Exact byte preservation, not just "some entry with a non-empty
        // ssid": escape_bytes()/unescape_bytes() round-trip the raw bytes
        // losslessly, so the invalid-UTF-8 SSID must come back byte-for-byte
        // identical, not mangled.
        if (n.ssid == invalid_utf8_ssid)
            found_invalid = true;
    }
    CHECK(found_valid);
    CHECK(found_invalid);
}

TEST_CASE("Reconnecting to a non-UTF-8 SSID does not grow the store unbounded",
          "[network][wifi][savedconfig][store][1229]") {
    // M12 regression. Before escape_bytes()/unescape_bytes(), write_store()
    // asked nlohmann's dump() to substitute U+FFFD for any invalid-UTF-8 byte
    // in an SSID (real routers broadcast arbitrary octets; see the test
    // above for why such SSIDs reach here at all). That mangled text, once
    // read back by parse_store(), never string-equals the RAW SSID
    // save() is called with on every subsequent connect — so the dedup-by-
    // SSID check in save() silently stopped matching and appended a fresh
    // duplicate cleartext-PSK record on every single reconnect attempt,
    // forever. Simulating "reconnect N times" as N sequential save() calls
    // with the same raw, invalid-UTF-8 SSID — exactly what connect_network()
    // does on each successful connection — the store must still hold exactly
    // one entry for it, the same guarantee save()'s dedup already gives every
    // ordinary (valid-UTF-8) SSID.
    ConfigDirGuard guard("store_utf8_dedup");

    const std::string invalid_utf8_ssid = std::string("Router") + "\xff" + "\xc0" + "Net";

    for (int attempt = 0; attempt < 5; ++attempt) {
        REQUIRE(helix::wifi::store::save({invalid_utf8_ssid, "pass" + std::to_string(attempt)}));
    }

    auto nets = helix::wifi::store::load();
    REQUIRE(nets.size() == 1);
    CHECK(nets[0].ssid == invalid_utf8_ssid);
    CHECK(nets[0].psk == "pass4"); // last write replaced, rather than duplicated
}
