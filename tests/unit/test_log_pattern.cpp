// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Pure-string tests for the per-sink spdlog pattern decision. These guard the
// invariants that matter for debugging the async-delete crash family: every
// sink must carry the thread id (%t), console/file must carry a ms timestamp,
// the system sinks must NOT add their own time token (they would double-stamp
// the journal/syslog clock), and the console must keep its colored-level
// tokens. No real sinks are constructed — pattern_for_sink() is pure.
//
// The [1218] cases additionally exercise the monotonic column end-to-end
// through a real pattern_formatter, because a string-only assertion on "%*"
// passes just as happily when the custom flag was never registered and spdlog
// emits the two characters literally.

#include "logging_init.h"
#include "system/monotonic_ring_sink.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/pattern_formatter.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::logging::pattern_for_sink;
using helix::logging::SinkKind;

namespace {

// TEST_MIRROR_OK: contains() is a one-line assertion utility (substring search), not logic
// under test
bool contains(const char* hay, const char* needle) {
    return std::string(hay).find(needle) != std::string::npos;
}

// A pattern has a time token if it uses any of spdlog's clock fields.
bool has_time_token(const char* p) {
    return contains(p, "%H") || contains(p, "%e") || contains(p, "%T");
}

} // namespace

TEST_CASE("Every sink pattern includes the thread id", "[logging][pattern]") {
    // %t is the whole point of the change — losing it on any sink defeats the
    // main-thread-vs-background-thread diagnosis. This must FAIL if dropped.
    for (auto kind : {SinkKind::Console, SinkKind::File, SinkKind::Journald, SinkKind::Syslog,
                      SinkKind::Android, SinkKind::CrashBreadcrumb}) {
        INFO("kind index = " << static_cast<int>(kind));
        REQUIRE(contains(pattern_for_sink(kind), "%t"));
    }
}

TEST_CASE("Console and File patterns carry a ms timestamp", "[logging][pattern]") {
    // Nothing else stamps these streams, so they must include their own clock.
    REQUIRE(has_time_token(pattern_for_sink(SinkKind::Console)));
    REQUIRE(contains(pattern_for_sink(SinkKind::Console), "%e")); // ms precision
    REQUIRE(has_time_token(pattern_for_sink(SinkKind::File)));
    REQUIRE(contains(pattern_for_sink(SinkKind::File), "%e"));
}

TEST_CASE("System sinks do not add their own timestamp", "[logging][pattern]") {
    // journald/syslog/android stamp their own time; adding one would double up.
    REQUIRE_FALSE(has_time_token(pattern_for_sink(SinkKind::Journald)));
    REQUIRE_FALSE(has_time_token(pattern_for_sink(SinkKind::Syslog)));
    REQUIRE_FALSE(has_time_token(pattern_for_sink(SinkKind::Android)));
}

TEST_CASE("Console pattern preserves the colored level tokens", "[logging][pattern]") {
    const char* p = pattern_for_sink(SinkKind::Console);
    REQUIRE(contains(p, "%^"));
    REQUIRE(contains(p, "%$"));
}

TEST_CASE("System sinks keep level text for grep-ability", "[logging][pattern]") {
    // %l in syslog/journald is intentional (grep /var/log/messages by level).
    REQUIRE(contains(pattern_for_sink(SinkKind::Syslog), "%l"));
    REQUIRE(contains(pattern_for_sink(SinkKind::Journald), "%l"));
}

// ---------------------------------------------------------------------------
// #1218 — monotonic column, date field, and clock-step annotation
// ---------------------------------------------------------------------------

using helix::logging::format_monotonic;
using helix::logging::is_clock_step;
using helix::logging::make_formatter;
using helix::logging::monotonic_seconds;

namespace {

// Format one message through a real sink formatter and return the line.
std::string render(SinkKind kind, spdlog::log_clock::time_point when, const char* text = "hello") {
    auto fmt = make_formatter(kind);
    spdlog::details::log_msg msg{when, spdlog::source_loc{}, "helix", spdlog::level::info, text};
    spdlog::memory_buf_t buf;
    fmt->format(msg, buf);
    return std::string(buf.data(), buf.size());
}

spdlog::log_clock::time_point epoch_plus(double seconds) {
    return spdlog::log_clock::time_point{} +
           std::chrono::microseconds{static_cast<long long>(seconds * 1e6)};
}

} // namespace

TEST_CASE("Every sink pattern carries the monotonic column", "[logging][pattern][1218]") {
    // Wall clock alone is not enough: a clock step fabricates gaps in every
    // stream we ask users to upload, including syslog on AD5M/AD5X.
    for (auto kind : {SinkKind::Console, SinkKind::File, SinkKind::Journald, SinkKind::Syslog,
                      SinkKind::Android, SinkKind::CrashBreadcrumb}) {
        INFO("kind index = " << static_cast<int>(kind));
        REQUIRE(contains(pattern_for_sink(kind), "%*"));
    }
}

TEST_CASE("The File pattern carries a date so a midnight crossing is unambiguous",
          "[logging][pattern][1218]") {
    REQUIRE(contains(pattern_for_sink(SinkKind::File), "%Y-%m-%d"));
}

TEST_CASE("format_monotonic renders a fixed-width zero-padded offset", "[logging][pattern][1218]") {
    // Fixed width is what makes a real gap visible by eye when scanning a
    // bundle; the leading '+' marks it as an offset, not a clock reading.
    REQUIRE(format_monotonic(57.398) == "+00057.398");
    REQUIRE(format_monotonic(0.0) == "+00000.000");
    REQUIRE(format_monotonic(35390.5) == "+35390.500");
    // Past the padding width it grows rather than truncating.
    REQUIRE(format_monotonic(123456.789) == "+123456.789");
}

TEST_CASE("is_clock_step separates a stepped clock from NTP slew", "[logging][pattern][1218]") {
    // The XRK8KPTF bundle: wall advanced ~874 s more than monotonic did.
    REQUIRE(is_clock_step(877.1, 3.1));
    // Backwards steps are the dangerous ones — they interleave lines out of order.
    REQUIRE(is_clock_step(-871.0, 3.1));
    // Agreeing clocks, at both short and long intervals.
    REQUIRE_FALSE(is_clock_step(3.1, 3.1));
    REQUIRE_FALSE(is_clock_step(3600.0, 3600.0));
    // adjtime slew is bounded at ~500 ppm — 0.5 s over a 1000 s interval.
    REQUIRE_FALSE(is_clock_step(1000.5, 1000.0));
}

TEST_CASE("monotonic_seconds is monotonic and anchored at process start",
          "[logging][pattern][1218]") {
    const double a = monotonic_seconds();
    const double b = monotonic_seconds();
    REQUIRE(a >= 0.0);
    REQUIRE(b >= a);
    // Anchored at process start, not at the epoch — a raw CLOCK_MONOTONIC read
    // on a machine up for a week would be ~600000.
    REQUIRE(a < 86400.0);
}

TEST_CASE("A rendered line actually contains the monotonic offset", "[logging][pattern][1218]") {
    // Guards the flag REGISTRATION, not just the pattern string: an unregistered
    // custom flag makes spdlog emit "%*" verbatim, which the string tests above
    // would happily accept.
    //
    // The offset is pinned through the replay channel: the live clock reads
    // process age, so the rendered digits depend on how long the run has lasted.
    auto& replay = helix::logging::detail::monotonic_replay();
    replay.active = true;
    replay.value = 1234.5;
    const std::string line = render(SinkKind::File, spdlog::log_clock::now());
    replay = {};
    INFO(line);
    REQUIRE(line.find("%*") == std::string::npos);
    REQUIRE(line.find("+01234.500") != std::string::npos);
    REQUIRE(line.find("hello") != std::string::npos);
}

TEST_CASE("A clock step is annotated on the line where it happens", "[logging][pattern][1218]") {
    // One formatter, two messages whose wall times are 874 s apart while real
    // monotonic time barely moves — exactly the XRK8KPTF signature.
    auto fmt = make_formatter(SinkKind::File);
    spdlog::memory_buf_t buf;

    spdlog::details::log_msg first{epoch_plus(100.0), spdlog::source_loc{}, "helix",
                                   spdlog::level::info, "before"};
    fmt->format(first, buf);
    const std::string line1(buf.data(), buf.size());
    INFO("line1=" << line1);
    REQUIRE(line1.find("CLOCK_STEP") == std::string::npos); // nothing to compare against yet

    buf.clear();
    spdlog::details::log_msg second{epoch_plus(974.0), spdlog::source_loc{}, "helix",
                                    spdlog::level::info, "after"};
    fmt->format(second, buf);
    const std::string line2(buf.data(), buf.size());
    INFO("line2=" << line2);
    REQUIRE(line2.find("CLOCK_STEP") != std::string::npos);

    // And a well-behaved next line goes back to clean output.
    buf.clear();
    spdlog::details::log_msg third{epoch_plus(974.0), spdlog::source_loc{}, "helix",
                                   spdlog::level::info, "settled"};
    fmt->format(third, buf);
    const std::string line3(buf.data(), buf.size());
    INFO("line3=" << line3);
    REQUIRE(line3.find("CLOCK_STEP") == std::string::npos);
}

TEST_CASE("Each sink formatter tracks its own clock-step state", "[logging][pattern][1218]") {
    // Sinks format independently under their own mutexes; one sink seeing a step
    // must not consume the annotation for another.
    auto a = make_formatter(SinkKind::File);
    auto b = make_formatter(SinkKind::File);

    spdlog::memory_buf_t buf;
    spdlog::details::log_msg first{epoch_plus(100.0), spdlog::source_loc{}, "helix",
                                   spdlog::level::info, "x"};
    spdlog::details::log_msg second{epoch_plus(974.0), spdlog::source_loc{}, "helix",
                                    spdlog::level::info, "y"};

    a->format(first, buf);
    buf.clear();
    b->format(first, buf);
    buf.clear();

    a->format(second, buf);
    REQUIRE(std::string(buf.data(), buf.size()).find("CLOCK_STEP") != std::string::npos);
    buf.clear();
    b->format(second, buf);
    REQUIRE(std::string(buf.data(), buf.size()).find("CLOCK_STEP") != std::string::npos);
}

// ============================================================================
// MonotonicRingSink — the bundle's log_tail source
// ============================================================================
//
// spdlog's stock ringbuffer_sink stores raw messages and formats them in
// last_formatted(), so `%*` read the DUMP instant and every line in a 2000-line
// tail carried the same offset. The step detector then compared a real wall
// delta against a monotonic delta of ~0 and fired on every idle gap over a
// second: one debug bundle shipped 97 CLOCK_STEP annotations summing to 8978 s,
// which is the entire session. These pin the stamp to log time.
//
// The clock is injected, so none of these sleep.

namespace {

std::shared_ptr<helix::logging::MonotonicRingSink> make_ring(std::vector<double> stamps) {
    auto seq = std::make_shared<size_t>(0);
    auto values = std::make_shared<std::vector<double>>(std::move(stamps));
    auto sink = std::make_shared<helix::logging::MonotonicRingSink>(16, [seq, values]() -> double {
        return *seq < values->size() ? (*values)[(*seq)++] : values->back();
    });
    sink->set_formatter(helix::logging::make_formatter(helix::logging::SinkKind::File));
    return sink;
}

void push(const std::shared_ptr<helix::logging::MonotonicRingSink>& sink, double wall,
          const char* text) {
    spdlog::details::log_msg msg{epoch_plus(wall), spdlog::source_loc{}, "helix",
                                 spdlog::level::info, text};
    sink->log(msg);
}

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines)
        out += l;
    return out;
}

} // namespace

TEST_CASE("Ring sink stamps each entry when logged, not when dumped",
          "[logging][pattern][1218][ring]") {
    // Three lines a second apart. The stock sink gave all three the same
    // column; each must now carry its own.
    auto sink = make_ring({100.0, 101.0, 102.0});
    push(sink, 1000.0, "a");
    push(sink, 1001.0, "b");
    push(sink, 1002.0, "c");

    auto lines = sink->last_formatted();
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0].find("+00100.000") != std::string::npos);
    REQUIRE(lines[1].find("+00101.000") != std::string::npos);
    REQUIRE(lines[2].find("+00102.000") != std::string::npos);
}

TEST_CASE("Ring dump does not fabricate CLOCK_STEP across an idle gap",
          "[logging][pattern][1218][ring]") {
    // Wall and monotonic both advance 30 s: a quiet app, not a clock step.
    auto sink = make_ring({100.0, 130.0});
    push(sink, 1000.0, "a");
    push(sink, 1030.0, "b");

    REQUIRE(join(sink->last_formatted()).find("CLOCK_STEP") == std::string::npos);
}

TEST_CASE("Ring dump still reports a genuine clock step", "[logging][pattern][1218][ring]") {
    // Wall jumps 100 s while monotonic advances 1 s — a real step, and the
    // whole reason the column exists. Must survive the replay path.
    auto sink = make_ring({100.0, 101.0});
    push(sink, 1000.0, "a");
    push(sink, 1100.0, "b");

    REQUIRE(join(sink->last_formatted()).find("CLOCK_STEP") != std::string::npos);
}

TEST_CASE("Ring dump is idempotent", "[logging][pattern][1218][ring]") {
    // debug_bundle_collector probes the ring with a 1-line tail before
    // log_collector takes the real dump. Carrying step state across dumps made
    // the real dump's first line diff against the probe's, opening every bundle
    // with a fabricated step.
    auto sink = make_ring({100.0, 130.0, 160.0});
    push(sink, 1000.0, "a");
    push(sink, 1030.0, "b");
    push(sink, 1060.0, "c");

    auto probe = sink->last_formatted(1); // the collector's emptiness check
    REQUIRE(probe.size() == 1);

    auto first = sink->last_formatted();
    auto second = sink->last_formatted();
    REQUIRE(first == second);
    REQUIRE(join(first).find("CLOCK_STEP") == std::string::npos);
}
