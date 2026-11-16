// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_plan.h"

#include "core/text_io.h"

#include <algorithm>
#include <cmath>

namespace helix {

namespace {

constexpr std::string_view kPlanTag = "HELIX:PLAN";
constexpr size_t kMaxDroppedKept = 4;

bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

std::string_view skip_blanks(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_blank(s[i])) {
        ++i;
    }
    return s.substr(i);
}

bool consume_prefix(std::string_view& s, std::string_view prefix) {
    if (s.substr(0, prefix.size()) != prefix) {
        return false;
    }
    s.remove_prefix(prefix.size());
    return true;
}

/// Digits with at most one dot; exponents, signs and suffixes stay out of a deadline.
bool looks_like_seconds(std::string_view value) {
    int dots = 0;
    int digits = 0;
    for (const char c : value) {
        if (c == '.') {
            ++dots;
        } else if (c >= '0' && c <= '9') {
            ++digits;
        } else {
            return false;
        }
    }
    return digits > 0 && dots <= 1;
}

/// The phase and seconds of one NAME=VALUE token, nullopt when either is refused.
std::optional<std::pair<int, int>> parse_token(std::string_view token) {
    const size_t eq = token.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    const auto phase =
        print_start_phase_from_name(text_io::to_upper(std::string(token.substr(0, eq))));
    if (!phase || !phase_accepts_declared_duration(*phase)) {
        return std::nullopt;
    }
    const std::string_view value = token.substr(eq + 1);
    if (!looks_like_seconds(value)) {
        return std::nullopt;
    }
    const auto number = text_io::parse_leading<double>(value);
    if (!number) {
        return std::nullopt;
    }
    const auto seconds = declared_seconds_from_number(*number);
    if (!seconds) {
        return std::nullopt;
    }
    return std::make_pair(static_cast<int>(*phase), *seconds);
}

std::vector<std::string_view> split_tokens(std::string_view s) {
    std::vector<std::string_view> tokens;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_blank(s[i])) {
            ++i;
        }
        const size_t start = i;
        while (i < s.size() && !is_blank(s[i])) {
            ++i;
        }
        if (i > start) {
            tokens.push_back(s.substr(start, i - start));
        }
    }
    return tokens;
}

} // namespace

bool phase_accepts_declared_duration(PrintStartPhase phase) {
    return phase != PrintStartPhase::IDLE && phase != PrintStartPhase::INITIALIZING &&
           phase != PrintStartPhase::COMPLETE;
}

std::optional<int> declared_seconds_from_number(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return std::nullopt;
    }
    const double rounded = std::round(seconds);
    if (rounded > static_cast<double>(kMaxDeclaredSeconds)) {
        return std::nullopt;
    }
    return static_cast<int>(rounded);
}

std::optional<PrintStartPlanLine> parse_print_start_plan(std::string_view line) {
    std::string_view rest = skip_blanks(line);
    if (consume_prefix(rest, "echo:") || consume_prefix(rest, "//")) {
        rest = skip_blanks(rest);
    }
    if (!consume_prefix(rest, kPlanTag)) {
        return std::nullopt;
    }
    if (!rest.empty() && !is_blank(rest.front())) {
        return std::nullopt;
    }

    PrintStartPlanLine result;
    if (line.size() > kMaxPlanLineBytes) {
        result.rejected_whole = true;
        return result;
    }
    const auto tokens = split_tokens(rest);
    if (tokens.size() > kMaxPlanTokens) {
        result.rejected_whole = true;
        return result;
    }
    for (const std::string_view token : tokens) {
        if (const auto parsed = parse_token(token)) {
            result.durations[parsed->first] = parsed->second;
        } else if (result.dropped.size() < kMaxDroppedKept) {
            result.dropped.emplace_back(token);
        }
    }
    return result;
}

void merge_declared(PreprintDeclaredDurations& plan, const PreprintDeclaredDurations& incoming,
                    int current_phase, int current_elapsed_seconds,
                    const std::vector<int>& finished_phases) {
    for (const auto& [phase, seconds] : incoming) {
        if (phase == current_phase) {
            plan[phase] = seconds > 0 ? current_elapsed_seconds + seconds : 0;
        } else if (std::find(finished_phases.begin(), finished_phases.end(), phase) ==
                   finished_phases.end()) {
            plan[phase] = seconds;
        }
    }
}

int overtime_balance_seconds(const std::map<int, int>& expected,
                             const std::map<int, int>& actual_finished, int current_phase,
                             int current_elapsed_seconds) {
    int balance = 0;
    for (const auto& [phase, actual] : actual_finished) {
        if (const auto it = expected.find(phase); it != expected.end()) {
            balance += actual - it->second;
        }
    }
    if (const auto it = expected.find(current_phase); it != expected.end()) {
        balance += std::max(0, current_elapsed_seconds - it->second);
    }
    return balance;
}

bool declared_phase_running_over(const PreprintDeclaredDurations& declared, int current_phase,
                                 int current_elapsed_seconds) {
    const auto it = declared.find(current_phase);
    return it != declared.end() && it->second > 0 && current_elapsed_seconds > it->second;
}

void apply_declared_to_checklist(const PreprintDeclaredDurations& declared,
                                 std::vector<PrintStartPhase>& expected_order,
                                 std::map<int, int>& predicted_seconds) {
    for (const auto& [phase_int, seconds] : declared) {
        const auto phase = static_cast<PrintStartPhase>(phase_int);
        const auto listed = std::find(expected_order.begin(), expected_order.end(), phase);
        if (seconds <= 0) {
            predicted_seconds.erase(phase_int);
            if (listed != expected_order.end()) {
                expected_order.erase(listed);
            }
            continue;
        }
        predicted_seconds[phase_int] = seconds;
        if (listed == expected_order.end()) {
            const auto later = std::find_if(expected_order.begin(), expected_order.end(),
                                            [phase_int = phase_int](PrintStartPhase p) {
                                                return static_cast<int>(p) > phase_int;
                                            });
            expected_order.insert(later, phase);
        }
    }
}

} // namespace helix
