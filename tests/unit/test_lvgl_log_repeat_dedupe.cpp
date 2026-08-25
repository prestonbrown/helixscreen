// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_lvgl_log_repeat_dedupe.cpp
 * @brief Repeated LVGL warnings must not reach the level a debug bundle keeps.
 *
 * The handler has always deduped its two high-frequency classes — per-frame
 * retries on a broken image asset, and a missing translation tag re-resolved on
 * every call that renders the string. It routed the repeats to DEBUG, which was
 * right when debug meant "off in the field".
 *
 * It stopped being right when the debug bundle grew its in-memory ring, which is
 * captured at debug (logging_init.cpp, and see log_redact.h). From then on the
 * dedupe quieted the console while still spending the ring on every repeat. In
 * bundle TPVTQKBM one untranslated tag — re-resolved on a temperature tick, so
 * ~5.5 times a second for the whole session — took 10,103 of the 13,310 lines
 * the bundle shipped, and pushed the print-start evidence we had asked that
 * reporter for out of the capture window entirely.
 *
 * Repeats belong at trace, which the ring never captures. The FIRST occurrence
 * must survive at its usual level: silencing the class outright would hide a
 * genuinely missing tag or a genuinely broken asset.
 */

#include "lvgl/lvgl.h"
#include "lvgl_log_handler.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Two capture sinks on the default logger, at the two thresholds the question
/// is actually about: `ring_` mirrors the debug bundle's in-memory ring (debug
/// floor), `all_` sees everything. Asking each what it kept is a direct model of
/// "would this have reached the bundle?" — no log-pattern parsing, so a change
/// to the format string cannot quietly turn these green.
class LevelCapture {
  public:
    LevelCapture()
        : ring_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)),
          all_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)) {
        logger_ = spdlog::default_logger();
        prev_level_ = logger_->level();
        ring_->set_level(spdlog::level::debug);
        all_->set_level(spdlog::level::trace);
        logger_->sinks().push_back(ring_);
        logger_->sinks().push_back(all_);
        logger_->set_level(spdlog::level::trace);
    }

    ~LevelCapture() {
        auto& sinks = logger_->sinks();
        for (auto* wanted : {&ring_, &all_}) {
            for (auto it = sinks.begin(); it != sinks.end(); ++it) {
                if (*it == *wanted) {
                    sinks.erase(it);
                    break;
                }
            }
        }
        logger_->set_level(prev_level_);
    }

    /// Lines a debug-floored sink kept — i.e. what a debug bundle would ship.
    int in_ring(const std::string& needle) const {
        return count(ring_, needle);
    }

    /// Lines recorded at any level, including trace.
    int total(const std::string& needle) const {
        return count(all_, needle);
    }

  private:
    static int count(const std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt>& sink,
                     const std::string& needle) {
        int n = 0;
        for (const auto& line : sink->last_formatted(256)) {
            if (line.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }

    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ring_;
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> all_;
    std::shared_ptr<spdlog::logger> logger_;
    spdlog::level::level_enum prev_level_;
};

/// The dedupe cache is process-global and survives across test cases, so every
/// case needs a body LVGL has never logged before or its "first occurrence"
/// assertion passes for the wrong reason.
std::string unique_tag(const char* discriminator) {
    return std::string("helix_dedupe_probe_") + discriminator;
}

// LV_LOG_WARN bakes LV_LOG_FILE / LV_LOG_LINE / __func__ into the message, so
// the deduped body is really (call site + text) — two warnings with identical
// text from different lines are different messages, and correctly so.
//
// Both emitters below therefore live in ONE function each, which is what
// production looks like: every missing-tag warning comes from lv_translation.c,
// and every retry from the one fs_open call in the decoder. Inlining these at
// the call sites would spread them across source lines and each repeat would
// read as a first occurrence.

void warn_missing_tag(const std::string& tag) {
    LV_LOG_WARN("`%s` tag is not found. Using the tag as translation.", tag.c_str());
}

void warn_asset_open_failed(const std::string& path) {
    LV_LOG_WARN("fs_open: Could not open file: %s", path.c_str());
}

} // namespace

TEST_CASE("LVGL log handler: a repeated missing-translation warning drops below the ring",
          "[logging][lvgl][bundle]") {
    helix::logging::register_lvgl_log_handler();
    helix::logging::set_suppress_translation_warnings(false);
    LevelCapture logs;

    const std::string tag = unique_tag("translation");
    for (int i = 0; i < 25; ++i) {
        warn_missing_tag(tag);
    }

    // Debug is the bundle ring's floor. Exactly one line may clear it, however
    // many times the tag is re-resolved.
    CHECK(logs.in_ring(tag) == 1);

    // The repeats are still recorded, just beneath the ring — a trace-level run
    // can still show every hit.
    CHECK(logs.total(tag) == 25);
}

TEST_CASE("LVGL log handler: a distinct missing tag still gets its own first hit",
          "[logging][lvgl][bundle]") {
    helix::logging::register_lvgl_log_handler();
    helix::logging::set_suppress_translation_warnings(false);
    LevelCapture logs;

    // Dedupe is by exact message body, and the body carries the tag. Two
    // untranslated strings must not collapse into one report, or filling the
    // first gap silently hides the second.
    const std::string first = unique_tag("distinct_a");
    const std::string second = unique_tag("distinct_b");
    warn_missing_tag(first);
    warn_missing_tag(second);
    warn_missing_tag(first);

    CHECK(logs.in_ring(first) == 1);
    CHECK(logs.in_ring(second) == 1);
}

TEST_CASE("LVGL log handler: a repeated broken-asset retry drops below the ring",
          "[logging][lvgl][bundle]") {
    helix::logging::register_lvgl_log_handler();
    LevelCapture logs;

    // The other high-frequency class, which LVGL retries on every render frame
    // that touches the broken image. It was deduped to debug before this, so it
    // was filling the ring in exactly the same way.
    const std::string path = unique_tag("asset") + ".png";
    for (int i = 0; i < 10; ++i) {
        warn_asset_open_failed(path);
    }

    CHECK(logs.in_ring(path) == 1);
    CHECK(logs.total(path) == 10);
}

TEST_CASE("LVGL log handler: init-time suppression does not spend a tag's one report",
          "[logging][lvgl][bundle]") {
    helix::logging::register_lvgl_log_handler();
    LevelCapture logs;

    const std::string tag = unique_tag("suppressed");

    // init_translations() resolves tags while suppression is on, so the same tag
    // can be hit before the UI ever renders it. If those hits enter the dedupe
    // cache they consume the one occurrence the ring was going to get, and the
    // first miss a user could actually see is filed as a repeat.
    helix::logging::set_suppress_translation_warnings(true);
    warn_missing_tag(tag);
    warn_missing_tag(tag);
    helix::logging::set_suppress_translation_warnings(false);
    CHECK(logs.in_ring(tag) == 0);

    warn_missing_tag(tag);
    CHECK(logs.in_ring(tag) == 1);
}
