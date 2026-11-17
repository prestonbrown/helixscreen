// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_control.h"

#include "random_bytes.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "hv/json.hpp"

namespace helix {

std::optional<TouchEvent> parse_touch_body(const std::string& body, int img_w, int img_h) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || img_w <= 0 || img_h <= 0) {
        return std::nullopt;
    }
    const auto type = j.find("type");
    const auto x = j.find("x");
    const auto y = j.find("y");
    if (type == j.end() || x == j.end() || y == j.end() || !type->is_string() ||
        !x->is_number_integer() || !y->is_number_integer()) {
        return std::nullopt;
    }
    TouchEvent e;
    const std::string t = type->get<std::string>();
    if (t == "down") {
        e.type = TouchEvent::Type::Down;
    } else if (t == "move") {
        e.type = TouchEvent::Type::Move;
    } else if (t == "up") {
        e.type = TouchEvent::Type::Up;
    } else {
        return std::nullopt;
    }
    const int64_t px = x->get<int64_t>();
    const int64_t py = y->get<int64_t>();
    if (px < 0 || py < 0 || px >= img_w || py >= img_h) {
        return std::nullopt;
    }
    e.x = static_cast<int32_t>(px);
    e.y = static_cast<int32_t>(py);
    return e;
}

namespace {

/// "http://host:port/..." -> "host:port".
std::string origin_authority(const std::string& origin) {
    const auto scheme = origin.find("://");
    std::string rest = scheme == std::string::npos ? origin : origin.substr(scheme + 3);
    const auto slash = rest.find('/');
    return slash == std::string::npos ? rest : rest.substr(0, slash);
}

/// The text before the first comma of a comma-separated header value.
std::string_view first_list_entry(std::string_view value) {
    return value.substr(0, value.find(','));
}

} // namespace

std::string trusted_forwarded_host(bool behind_nginx, const std::string& x_forwarded_host,
                                   const std::string& forwarded) {
    if (!behind_nginx) {
        return {};
    }
    // Each proxy on the way appends its own entry; the first is what the browser used.
    if (const auto first = text_io::trim(first_list_entry(x_forwarded_host)); !first.empty()) {
        return std::string(first);
    }
    // RFC 7239: `Forwarded: for=...;host=...;proto=..., <next proxy's element>`.
    const std::string_view element = first_list_entry(forwarded);
    size_t pos = 0;
    while (pos <= element.size()) {
        const size_t semi = std::min(element.find(';', pos), element.size());
        const std::string_view pair = text_io::trim(element.substr(pos, semi - pos));
        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos &&
            text_io::equals_ignore_case(text_io::trim(pair.substr(0, eq)), "host")) {
            std::string_view value = text_io::trim(pair.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return std::string(value);
        }
        pos = semi + 1;
    }
    return {};
}

TouchGate check_touch_request(bool control_allowed, bool lan_mode, const std::string& content_type,
                              const std::string& x_helix_touch, const std::string& origin,
                              const std::string& host, const std::string& forwarded_host,
                              bool paired) {
    if (!control_allowed) {
        return TouchGate::ControlOff;
    }
    if (content_type.rfind("application/json", 0) != 0) {
        return TouchGate::BadContentType;
    }
    if (x_helix_touch != "1") {
        return TouchGate::MissingHeader;
    }
    if (!origin.empty() && origin_authority(origin) != host &&
        (forwarded_host.empty() || origin_authority(origin) != forwarded_host)) {
        return TouchGate::OriginMismatch;
    }
    if (lan_mode && !paired) {
        return TouchGate::NotPaired;
    }
    return TouchGate::Allowed;
}

// ---------------------------------------------------------------------------
// PairingStore
// ---------------------------------------------------------------------------

std::string PairingStore::generate_code() {
    // Rejection sampling keeps every code equally likely.
    constexpr uint32_t kLimit = 4294000000u; // a multiple of 1,000,000 below 2^32
    uint32_t v = 0;
    do {
        unsigned char b[4];
        fill_random_bytes(b, sizeof(b));
        v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
            (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    } while (v >= kLimit);
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06u", v % 1000000u);
    return buf;
}

std::string PairingStore::generate_token() {
    unsigned char b[16];
    fill_random_bytes(b, sizeof(b));
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char c : b) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}

PairingStore::PairingStore(std::string code, std::vector<std::string> tokens, Persist persist,
                           Clock clock)
    : code_(std::move(code)), tokens_(std::move(tokens)), persist_(std::move(persist)),
      clock_(std::move(clock)) {
    if (code_.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        rotate_code_locked();
    }
    if (tokens_.size() > kMaxPairedBrowsers) {
        tokens_.erase(tokens_.begin(),
                      tokens_.begin() + static_cast<long>(tokens_.size() - kMaxPairedBrowsers));
    }
}

std::chrono::steady_clock::time_point PairingStore::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

void PairingStore::rotate_code_locked() {
    code_ = generate_code();
    if (persist_.save_code) {
        persist_.save_code(code_);
    }
}

PairingStore::PairResult PairingStore::try_pair(const std::string& code, std::string& token_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto t = now();
    const auto window = std::chrono::milliseconds(kPairWindowMs);
    recent_failures_.erase(std::remove_if(recent_failures_.begin(), recent_failures_.end(),
                                          [&](auto at) { return t - at >= window; }),
                           recent_failures_.end());
    if (static_cast<int>(recent_failures_.size()) >= kPairAttemptsPerWindow) {
        return PairResult::RateLimited;
    }
    if (code.size() != code_.size() || code != code_) {
        recent_failures_.push_back(t);
        if (++total_failures_ >= kPairFailuresBeforeRotate) {
            spdlog::warn("[RemoteScreen] {} wrong pairing codes; the code has been changed",
                         total_failures_);
            total_failures_ = 0;
            rotate_code_locked();
        }
        return PairResult::WrongCode;
    }
    token_out = generate_token();
    tokens_.push_back(token_out);
    if (tokens_.size() > kMaxPairedBrowsers) {
        tokens_.erase(tokens_.begin());
    }
    if (persist_.save_tokens) {
        persist_.save_tokens(tokens_);
    }
    spdlog::info("[RemoteScreen] A browser paired for remote control");
    return PairResult::Paired;
}

bool PairingStore::is_paired(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tokens_.begin(); it != tokens_.end(); ++it) {
        if (it->size() != token.size()) {
            continue;
        }
        // Compare every byte, so timing says nothing about how much matched.
        unsigned char diff = 0;
        for (size_t i = 0; i < token.size(); ++i) {
            diff |= static_cast<unsigned char>((*it)[i] ^ token[i]);
        }
        if (diff == 0) {
            if (std::next(it) != tokens_.end()) {
                std::string used = *it;
                tokens_.erase(it);
                tokens_.push_back(std::move(used));
            }
            return true;
        }
    }
    return false;
}

void PairingStore::forget_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_.clear();
    if (persist_.save_tokens) {
        persist_.save_tokens(tokens_);
    }
    recent_failures_.clear();
    total_failures_ = 0;
    rotate_code_locked();
    spdlog::info("[RemoteScreen] Forgot every paired browser");
}

std::string PairingStore::code() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return code_;
}

int PairingStore::retry_after_seconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(recent_failures_.size()) < kPairAttemptsPerWindow) {
        return 0;
    }
    const auto t = now();
    const auto oldest = *std::min_element(recent_failures_.begin(), recent_failures_.end());
    const auto left = std::chrono::milliseconds(kPairWindowMs) - (t - oldest);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(left).count();
    return secs > 0 ? static_cast<int>(secs) + 1 : 0;
}

bool remote_control_active(std::optional<std::chrono::steady_clock::time_point> last_touch,
                           std::chrono::steady_clock::time_point now) {
    return last_touch && now - *last_touch < std::chrono::milliseconds(kRemoteControlActiveMs);
}

// ---------------------------------------------------------------------------
// TouchSession
// ---------------------------------------------------------------------------

bool TouchSession::on_event(const TouchEvent& e, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (e.type == TouchEvent::Type::Move && !pressed_) {
        return false;
    }
    pressed_ = e.type != TouchEvent::Type::Up;
    last_ = e;
    last_at_ = now;
    return true;
}

std::optional<TouchEvent>
TouchSession::take_stale_release(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pressed_ || !last_at_ || now - *last_at_ < std::chrono::milliseconds(kTouchStaleMs)) {
        return std::nullopt;
    }
    pressed_ = false;
    return TouchEvent{TouchEvent::Type::Up, last_.x, last_.y};
}

std::optional<std::chrono::steady_clock::time_point> TouchSession::last_event() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_at_;
}

void TouchSession::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pressed_ = false;
    last_at_.reset();
}

} // namespace helix
