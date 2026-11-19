// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// A press held this long with no further event is released: a browser that went
/// away mid-drag must not leave the screen pressed.
constexpr int64_t kTouchStaleMs = 3000;
/// How long after the last accepted touch the screen shows that a browser is driving it.
constexpr int64_t kRemoteControlActiveMs = 10000;
/// Wrong pairing codes allowed per kPairWindowMs before requests are refused.
constexpr int kPairAttemptsPerWindow = 5;
constexpr int64_t kPairWindowMs = 60000;
/// Cumulative wrong codes after which the code itself changes.
constexpr int kPairFailuresBeforeRotate = 50;
/// Browsers remembered at once; the least recently used is forgotten first.
constexpr size_t kMaxPairedBrowsers = 8;
/// Header a paired browser presents its token in.
constexpr const char* kPairHeaderName = "X-Helix-Pair";

/// One pointer event from the control page, in served-image pixels, which are the
/// display's logical coordinates.
struct TouchEvent {
    enum class Type { Down, Move, Up };
    Type type = Type::Down;
    int32_t x = 0;
    int32_t y = 0;
};

/// Parse `{"type":"down|move|up","x":int,"y":int}`. nullopt for anything else,
/// including a point outside the @p img_w x @p img_h image.
std::optional<TouchEvent> parse_touch_body(const std::string& body, int img_w, int img_h);

enum class TouchGate {
    Allowed,
    ControlOff,     ///< 403
    BadContentType, ///< 403: only application/json, which a cross-site form cannot send
    MissingHeader,  ///< 403: X-Helix-Touch: 1, which forces a CORS preflight we never answer
    OriginMismatch, ///< 403
    NotPaired,      ///< 401: facing the LAN without a paired browser's token
};

/// The host a reverse proxy in front of the installer's nginx route says the browser
/// addressed: the first `X-Forwarded-Host` entry, else the `host=` of the first
/// `Forwarded` element. Empty unless @p behind_nginx: only then does every request arrive
/// through that route (the server binds 127.0.0.1), which passes the client's headers on
/// unchanged. A server facing the LAN takes these headers from anyone, so it ignores them.
std::string trusted_forwarded_host(bool behind_nginx, const std::string& x_forwarded_host,
                                   const std::string& forwarded);

/// Whether a touch POST may drive the screen. Pure. @p origin may be empty (a
/// same-origin request from some browsers); when present its host:port must be
/// @p host or a non-empty @p forwarded_host (from trusted_forwarded_host). Pairing is
/// only consulted in @p lan_mode: behind nginx, auth_request already authenticated the
/// request.
TouchGate check_touch_request(bool control_allowed, bool lan_mode, const std::string& content_type,
                              const std::string& x_helix_touch, const std::string& origin,
                              const std::string& host, const std::string& forwarded_host,
                              bool paired);

/**
 * @brief Pairing for a screen server that faces the LAN.
 *
 * The device shows a 6-digit code; a browser that sends it gets a random token
 * it keeps in its own storage and sends with every request. Thread-safe. Persistence goes through
 * @p persist, so the store itself touches no config.
 */
class PairingStore {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    struct Persist {
        std::function<void(const std::string& code)> save_code;
        std::function<void(const std::vector<std::string>& tokens)> save_tokens;
    };
    enum class PairResult { Paired, WrongCode, RateLimited };

    /// An empty @p code is replaced by a generated one (and saved).
    PairingStore(std::string code, std::vector<std::string> tokens, Persist persist,
                 Clock clock = {});

    /// On Paired, @p token_out is the new browser token.
    PairResult try_pair(const std::string& code, std::string& token_out);
    bool is_paired(const std::string& token);
    /// Forget every browser and change the code.
    void forget_all();
    std::string code() const;
    /// Seconds until a rate-limited browser may try again (0 when not limited).
    int retry_after_seconds() const;

    /// Six digits from a CSPRNG, leading zeros kept.
    static std::string generate_code();
    /// 32 hex digits from a CSPRNG.
    static std::string generate_token();

  private:
    std::chrono::steady_clock::time_point now() const;
    void rotate_code_locked();

    mutable std::mutex mutex_;
    std::string code_;
    std::vector<std::string> tokens_; ///< Most recently used last.
    Persist persist_;
    Clock clock_;
    std::vector<std::chrono::steady_clock::time_point> recent_failures_;
    int total_failures_ = 0;
};

/// True while a browser touched the screen within kRemoteControlActiveMs of @p now.
bool remote_control_active(std::optional<std::chrono::steady_clock::time_point> last_touch,
                           std::chrono::steady_clock::time_point now);

/**
 * @brief Releases a press nobody finished.
 *
 * Fed every accepted touch; asked periodically whether a press has been held
 * past kTouchStaleMs with nothing following it.
 */
class TouchSession {
  public:
    /// Record an accepted touch. False for a move with nothing pressed (the stale press
    /// was already released), which must be dropped rather than start a new press.
    bool on_event(const TouchEvent& e, std::chrono::steady_clock::time_point now);
    /// The point to release at, once; nullopt while nothing is stale.
    std::optional<TouchEvent> take_stale_release(std::chrono::steady_clock::time_point now);
    /// When the last accepted touch arrived, for the remote-control indicator.
    std::optional<std::chrono::steady_clock::time_point> last_event() const;
    /// Forget everything: the server stopped, so no earlier touch is still in progress.
    void reset();

  private:
    mutable std::mutex mutex_;
    bool pressed_ = false;
    TouchEvent last_{};
    std::optional<std::chrono::steady_clock::time_point> last_at_;
};

} // namespace helix
