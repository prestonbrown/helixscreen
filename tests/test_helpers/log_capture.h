// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// RAII spdlog captures, for tests that assert on what the log actually said.
///
/// Three types rather than one with a mode, because how a capture installs
/// itself is a behavioural choice the assertions depend on, and naming it at
/// the declaration is what keeps it from being made by accident:
///
///   LogCapture           adds a ring-buffer sink to the default logger and
///                        raises it to trace. Every other sink still receives
///                        records, so the console keeps printing.
///   ExclusiveLogCapture  swaps a private ring-buffer logger into the default
///                        slot. Nothing else logs anywhere while it is alive.
///                        Required when other threads log concurrently: pushing
///                        onto the shared logger's sink vector races their
///                        sink_it_ iteration.
///   TextLogCapture       swaps in a logger that writes message text only
///                        ("%v" -- no timestamp, level or logger name) into one
///                        unbounded string, for assertions on exact text and
///                        for proving a string appears in NO line at all.
///
/// The two ring captures format with spdlog's default pattern, so their lines
/// carry a timestamp and a level; a text capture's do not. A test cannot be
/// moved between the two without rereading what it matches on.

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace helix {

namespace detail {

/// The ring of formatted lines and the queries over it. Both ring captures
/// install it differently and then answer exactly these questions.
class LogRing {
  public:
    LogRing(const LogRing&) = delete;
    LogRing& operator=(const LogRing&) = delete;

    /// The captured lines still in the ring, oldest first.
    [[nodiscard]] std::vector<std::string> lines() const {
        return sink_->last_formatted(capacity_);
    }

    /// How many captured lines contain @p needle.
    [[nodiscard]] int count_containing(const std::string& needle) const {
        int n = 0;
        for (const auto& line : lines()) {
            if (line.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }

    /// True when some captured line contains every one of @p needles.
    [[nodiscard]] bool has_line_with(const std::vector<std::string>& needles) const {
        for (const auto& line : lines()) {
            bool all = true;
            for (const auto& needle : needles) {
                if (line.find(needle) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
        }
        return false;
    }

    /// Levels of every captured record whose text contains @p needle, oldest
    /// first. Reads the raw records rather than the formatted lines, so a test
    /// can assert that something is logged at debug rather than at warn.
    [[nodiscard]] std::vector<spdlog::level::level_enum>
    levels_for(const std::string& needle) const {
        std::vector<spdlog::level::level_enum> out;
        for (const auto& msg : sink_->last_raw(capacity_)) {
            const std::string text(msg.payload.data(), msg.payload.size());
            if (text.find(needle) != std::string::npos) {
                out.push_back(msg.level);
            }
        }
        return out;
    }

  protected:
    explicit LogRing(size_t capacity)
        : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(capacity)),
          capacity_(capacity) {
        sink_->set_level(spdlog::level::trace);
    }

    /// Non-virtual: these are stack guards, never owned through a base pointer.
    ~LogRing() = default;

    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    size_t capacity_;
};

} // namespace detail

/// Adds a ring-buffer sink to the default logger and raises the logger to
/// trace, restoring its level and removing the sink on scope exit. The logger's
/// other sinks keep receiving records.
class LogCapture : public detail::LogRing {
  public:
    explicit LogCapture(size_t capacity = 256) : LogRing(capacity) {
        logger_ = spdlog::default_logger();
        prev_level_ = logger_->level();
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);
    }

    ~LogCapture() {
        auto& sinks = logger_->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == sink_) {
                sinks.erase(it);
                break;
            }
        }
        logger_->set_level(prev_level_);
    }

  private:
    std::shared_ptr<spdlog::logger> logger_;
    spdlog::level::level_enum prev_level_;
};

/// Swaps a private ring-buffer logger into the default slot, restoring the
/// previous default logger on scope exit. No other sink sees anything while it
/// is alive, which is what makes it safe alongside threads that log: the shared
/// logger's sink vector is never mutated.
class ExclusiveLogCapture : public detail::LogRing {
  public:
    explicit ExclusiveLogCapture(size_t capacity = 256) : LogRing(capacity) {
        logger_ = std::make_shared<spdlog::logger>("log_capture", sink_);
        logger_->set_level(spdlog::level::trace);
        original_ = spdlog::default_logger();
        spdlog::set_default_logger(logger_);
    }

    ~ExclusiveLogCapture() {
        spdlog::set_default_logger(original_);
    }

  private:
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<spdlog::logger> original_;
};

/// Swaps in a logger that writes message text only into one unbounded string,
/// restoring the previous default logger on scope exit. It runs at trace, the
/// most permissive level a real build could ever use, so "this string reaches
/// no log line" is an assertion about every level at once.
class TextLogCapture {
  public:
    TextLogCapture() {
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured_);
        sink->set_pattern("%v");
        logger_ = std::make_shared<spdlog::logger>("log_capture", sink);
        logger_->set_level(spdlog::level::trace);
        original_ = spdlog::default_logger();
        spdlog::set_default_logger(logger_);
    }

    ~TextLogCapture() {
        spdlog::set_default_logger(original_);
    }

    TextLogCapture(const TextLogCapture&) = delete;
    TextLogCapture& operator=(const TextLogCapture&) = delete;

    /// Everything logged so far, as one blob.
    [[nodiscard]] std::string get_captured() const {
        return captured_.str();
    }

    /// True when @p text appears anywhere in the captured blob.
    [[nodiscard]] bool contains(const std::string& text) const {
        return captured_.str().find(text) != std::string::npos;
    }

  private:
    std::ostringstream captured_;
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<spdlog::logger> original_;
};

} // namespace helix
