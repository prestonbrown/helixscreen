// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/log_redact.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(ESP_PLATFORM)
#include "esp_random.h"
#endif

namespace helix::redact {

namespace {

// Domain separators, so the same bytes hashed as an SSID and as a MAC produce
// different tokens. Without this a bundle reader could tell that some AP's
// BSSID matched some other field, which is itself a disclosure.
constexpr uint64_t kSsidDomain = 0x5353'4944'0000'0001ULL;
constexpr uint64_t kMacDomain = 0x4D41'4300'0000'0002ULL;
constexpr uint64_t kIpDomain = 0x4950'0000'0000'0003ULL;

// Per-boot salt, resolved once on first use. Regenerated every launch so
// tokens cannot be correlated across sessions, and so a precomputed table of
// common SSIDs ("linksys", "NETGEAR", the ISP default patterns) cannot be
// matched against a bundle. Thread-safe: the wifi backend calls this from its
// own thread, and C++11 guarantees the static initializer runs exactly once.
uint64_t boot_salt() {
    static const uint64_t value = [] {
        uint64_t v = 0;
#if defined(ESP_PLATFORM)
        v = (static_cast<uint64_t>(esp_random()) << 32) ^ static_cast<uint64_t>(esp_random());
#else
        try {
            std::random_device rd;
            v = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
        } catch (const std::exception&) {
            v = 0; // fall through to the clock below
        }
#endif
        if (v == 0) {
            // Some embedded libcs ship a std::random_device that returns a
            // constant. A predictable salt is still far better than none, but
            // never leave it zero.
            v = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) |
                1ULL;
        }
        return v;
    }();
    return value;
}

// FNV-1a, salted through the offset basis. Not cryptographic — it does not
// need to be. The salt is random per boot and never logged, so without it the
// truncated digest we emit is not invertible; with it, the only thing
// recoverable is which lines refer to the same network, which is the point.
uint64_t hash_with(std::string_view data, uint64_t domain, uint64_t salt) {
    uint64_t h = (salt ^ domain) ^ 0xcbf2'9ce4'8422'2325ULL;
    for (unsigned char c : data) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x0000'0100'0000'01b3ULL;
    }
    return h;
}

std::string token(std::string_view value, const char* prefix, uint64_t domain, uint64_t salt) {
    if (value.empty()) {
        return std::string(prefix) + "<none>";
    }
    // 24 bits distinguishes the handful of APs in one scan with a collision
    // chance around 1 in 400,000, while keeping the ring compact on a 473MB
    // device. 16 bits was small enough that a token could incidentally contain
    // a two-hex-digit fragment of its own input.
    const auto digest = static_cast<unsigned>(hash_with(value, domain, salt) & 0xFF'FFFFULL);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06x", digest);
    return std::string(prefix) + buf;
}

// =============================================================================
// IP address classification
//
// Hand-rolled rather than inet_pton() so the same translation unit still builds
// for the splash and watchdog binaries without dragging in a socket header, and
// hand-rolled rather than std::regex because the debug bundle runs this over
// every line of a multi-megabyte log tail.
// =============================================================================

int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool is_word_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '-';
}

// One decimal octet. Leading zeros are rejected: nothing writes an address that
// way, some resolvers read "010" as octal, and allowing them turns date-shaped
// text ("2026.08.25.1") into a false positive.
bool parse_octet(std::string_view s, uint8_t& out) {
    if (s.empty() || s.size() > 3)
        return false;
    if (s.size() > 1 && s[0] == '0')
        return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v > 255)
        return false;
    out = static_cast<uint8_t>(v);
    return true;
}

// Whole-string dotted quad. A fifth group or a trailing letter fails, because
// the last octet is parsed from everything after the third dot.
bool parse_ipv4(std::string_view s, uint8_t out[4]) {
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t dot = s.find('.', start);
        if (i < 3 && dot == std::string_view::npos)
            return false;
        const std::string_view part = (i == 3) ? s.substr(start) : s.substr(start, dot - start);
        if (!parse_octet(part, out[i]))
            return false;
        start = dot + 1;
    }
    return true;
}

// Colon-separated hex groups, optionally ending in an embedded dotted quad.
// Returns bytes written, or -1 on error.
int emit_groups(std::string_view part, uint8_t* dst) {
    if (part.empty())
        return 0;
    int written = 0;
    size_t start = 0;
    for (;;) {
        const size_t colon = part.find(':', start);
        const std::string_view g = (colon == std::string_view::npos)
                                       ? part.substr(start)
                                       : part.substr(start, colon - start);
        if (g.empty())
            return -1;
        if (g.find('.') != std::string_view::npos) {
            if (colon != std::string_view::npos)
                return -1; // an IPv4 tail may only be the final group
            if (written + 4 > 16)
                return -1;
            uint8_t quad[4];
            if (!parse_ipv4(g, quad))
                return -1;
            std::memcpy(dst + written, quad, 4);
            return written + 4;
        }
        if (g.size() > 4)
            return -1;
        uint16_t v = 0;
        for (char c : g) {
            const int d = hex_value(c);
            if (d < 0)
                return -1;
            v = static_cast<uint16_t>(v * 16 + d);
        }
        if (written + 2 > 16)
            return -1;
        dst[written++] = static_cast<uint8_t>(v >> 8);
        dst[written++] = static_cast<uint8_t>(v & 0xFF);
        if (colon == std::string_view::npos)
            return written;
        start = colon + 1;
    }
}

bool parse_ipv6(std::string_view s, uint8_t out[16]) {
    if (s.size() < 2 || s.find(':') == std::string_view::npos)
        return false;
    if (s.find(":::") != std::string_view::npos)
        return false;
    const size_t dc = s.find("::");
    if (dc != std::string_view::npos && s.find("::", dc + 1) != std::string_view::npos)
        return false;
    // A leading or trailing ':' is legal only as half of a "::".
    if (s.front() == ':' && s[1] != ':')
        return false;
    if (s.back() == ':' && s[s.size() - 2] != ':')
        return false;

    uint8_t head[16];
    uint8_t tail[16];
    const int head_len = emit_groups(dc == std::string_view::npos ? s : s.substr(0, dc), head);
    if (head_len < 0)
        return false;
    const int tail_len = (dc == std::string_view::npos) ? 0 : emit_groups(s.substr(dc + 2), tail);
    if (tail_len < 0)
        return false;

    if (dc == std::string_view::npos) {
        if (head_len != 16)
            return false;
        std::memcpy(out, head, 16);
        return true;
    }
    // "::" stands for at least one elided all-zero group.
    if (head_len + tail_len > 14)
        return false;
    std::memset(out, 0, 16);
    std::memcpy(out, head, static_cast<size_t>(head_len));
    std::memcpy(out + 16 - tail_len, tail, static_cast<size_t>(tail_len));
    return true;
}

IpClass class_v4(const uint8_t b[4]) {
    if (b[0] == 0) // 0.0.0.0/8 "this network"
        return IpClass::Unspecified;
    if (b[0] == 10) // RFC1918
        return IpClass::Private;
    if (b[0] == 127)
        return IpClass::Loopback;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) // 172.16/12
        return IpClass::Private;
    if (b[0] == 192 && b[1] == 168) // RFC1918
        return IpClass::Private;
    if (b[0] == 169 && b[1] == 254)
        return IpClass::LinkLocal;
    if (b[0] == 100 && (b[1] & 0xC0) == 64) // 100.64/10
        return IpClass::SharedCgnat;
    if (b[0] >= 224) // multicast, reserved, broadcast
        return IpClass::Multicast;
    return IpClass::Global;
}

IpClass class_v6(const uint8_t b[16]) {
    bool zero_prefix = true;
    for (int i = 0; i < 10; ++i) {
        if (b[i] != 0)
            zero_prefix = false;
    }
    if (zero_prefix) {
        // ::ffff:a.b.c.d (mapped) and ::a.b.c.d (compatible) are whatever the
        // IPv4 they carry is.
        if (b[10] == 0xFF && b[11] == 0xFF)
            return class_v4(b + 12);
        if (b[10] == 0 && b[11] == 0) {
            if (b[12] == 0 && b[13] == 0 && b[14] == 0)
                return b[15] == 1 ? IpClass::Loopback : IpClass::Unspecified;
            return class_v4(b + 12);
        }
    }
    if ((b[0] & 0xFE) == 0xFC) // fc00::/7
        return IpClass::UniqueLocal;
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) // fe80::/10
        return IpClass::LinkLocal;
    if (b[0] == 0xFF) // ff00::/8
        return IpClass::Multicast;
    return IpClass::Global;
}

// Candidate character classes for the free-text scanners. IPv6 candidates are
// maximal runs of hex/colon/dot; IPv4 candidates are maximal runs of digit/dot.
bool is_v6_char(char c) {
    return hex_value(c) >= 0 || c == ':' || c == '.';
}

bool is_v4_char(char c) {
    return (c >= '0' && c <= '9') || c == '.';
}

// Replace every globally routable IPv6 literal. Runs before the IPv4 pass so a
// mapped form (::ffff:203.0.113.5) is consumed whole instead of being
// half-matched on its dotted tail.
std::string redact_v6(std::string_view text, uint64_t salt, std::string_view fixed = {}) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        if (!is_v6_char(text[i])) {
            out += text[i];
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < n && is_v6_char(text[i]))
            ++i;
        std::string_view run = text.substr(start, i - start);
        const bool left_ok = start == 0 || !is_word_char(text[start - 1]);
        // A trailing '.' is sentence punctuation, never part of an address.
        while (!run.empty() && run.back() == '.')
            run.remove_suffix(1);
        uint8_t bytes[16];
        if (left_ok && run.find(':') != std::string_view::npos && parse_ipv6(run, bytes) &&
            (!fixed.empty() || class_v6(bytes) == IpClass::Global)) {
            if (fixed.empty())
                out += token(run, "ip#", kIpDomain, salt);
            else
                out += fixed;
            out.append(text.substr(start + run.size(), i - start - run.size()));
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

std::string redact_v4(std::string_view text, uint64_t salt, std::string_view fixed = {}) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        if (!is_v4_char(text[i])) {
            out += text[i];
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < n && is_v4_char(text[i]))
            ++i;
        std::string_view run = text.substr(start, i - start);
        // A version glued to a word is not an address: "v1.2.3.4" and
        // "1.2.3.4-rc1" are rejected on their boundaries, "1.2.3.4.5" fails to
        // parse. The right edge is tested against the character after the whole
        // digit/dot run, so a sentence-final "1.2.3.4." still matches.
        const bool left_ok = start == 0 || !is_word_char(text[start - 1]);
        const bool right_ok = i >= n || !is_word_char(text[i]);
        while (!run.empty() && run.back() == '.')
            run.remove_suffix(1);
        uint8_t quad[4];
        if (left_ok && right_ok && parse_ipv4(run, quad) &&
            (!fixed.empty() || class_v4(quad) == IpClass::Global)) {
            if (fixed.empty())
                out += token(run, "ip#", kIpDomain, salt);
            else
                out += fixed;
            out.append(text.substr(start + run.size(), i - start - run.size()));
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

char lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
}

std::string lowercase(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = lower_char(c);
    return out;
}

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

/// Every placeholder telemetry_text() writes.
constexpr std::string_view kPlaceholders[] = {"<ip>",  "<mac>",  "<id>",        "<path>",
                                              "<url>", "<host>", "<user@host>", "<redacted>"};

/// The placeholder `s` starts with, or empty.
std::string_view leading_placeholder(std::string_view s) {
    for (auto p : kPlaceholders)
        if (s.substr(0, p.size()) == p)
            return p;
    return {};
}

/// Every MAC address (six hex pairs joined by ':' or '-') replaced by `<mac>`.
std::string replace_macs(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        bool match = i + 17 <= n && (i == 0 || !is_word_char(text[i - 1])) &&
                     (i + 17 == n || !is_word_char(text[i + 17]));
        for (size_t g = 0; match && g < 6; ++g) {
            const size_t at = i + g * 3;
            match = hex_value(text[at]) >= 0 && hex_value(text[at + 1]) >= 0 &&
                    (g == 5 || text[at + 2] == ':' || text[at + 2] == '-');
        }
        if (match) {
            out += "<mac>";
            i += 17;
        } else {
            out += text[i++];
        }
    }
    return out;
}

/// Names too generic to identify a device, which also stand for platforms and
/// software in installer output.
bool is_generic_name(std::string_view lower) {
    static constexpr std::string_view kGeneric[] = {"pi",   "printer", "root",
                                                    "user", "admin",   "klipper"};
    for (auto g : kGeneric)
        if (lower == g)
            return true;
    return false;
}

/// `text` with each whole-word occurrence of an identifier, in any case,
/// replaced by `<id>`. Runs after the IP pass, so an all-digit identifier can
/// no longer split an address.
std::string replace_identifiers(std::string text, const std::vector<std::string>& identifiers) {
    for (const auto& raw : identifiers) {
        const std::string id = lowercase(raw);
        if (id.size() < 4 || is_generic_name(id))
            continue;
        bool all_digits = true;
        for (char c : id)
            all_digits = all_digits && is_digit(c);
        if (all_digits)
            continue;
        std::string lower = lowercase(text);
        size_t pos = 0;
        while ((pos = lower.find(id, pos)) != std::string::npos) {
            const size_t end = pos + id.size();
            const bool left_ok = pos == 0 || !is_word_char(lower[pos - 1]);
            const bool right_ok = end >= lower.size() || !is_word_char(lower[end]);
            if (left_ok && right_ok) {
                text.replace(pos, id.size(), "<id>");
                lower.replace(pos, id.size(), "<id>");
                pos += 4;
            } else {
                pos = end;
            }
        }
    }
    return text;
}

/// A dotted name ending in one of these is a file, not a host.
bool is_file_extension(std::string_view label) {
    static constexpr std::string_view kExtensions[] = {
        "sh",   "json", "cfg",  "conf", "env",     "log", "txt", "gz",      "tar",      "tgz",
        "zip",  "xz",   "bz2",  "py",   "service", "so",  "crt", "pem",     "ini",      "toml",
        "yaml", "yml",  "xml",  "bin",  "img",     "tmp", "pid", "lock",    "download", "old",
        "bak",  "md",   "html", "js",   "css",     "png", "jpg", "partial", "socket",   "sock",
        "d",    "cpp",  "h",    "lua",  "gcode",   "mk",  "db",  "list",    "new",      "orig"};
    for (auto ext : kExtensions)
        if (label == ext)
            return true;
    return false;
}

/// `name.name[.name...]` with an optional `:port`, whose last label is
/// alphabetic, at least two letters and not a file extension.
bool looks_like_host(std::string_view core) {
    if (const size_t colon = core.rfind(':'); colon != std::string_view::npos) {
        const std::string_view port = core.substr(colon + 1);
        bool digits = !port.empty();
        for (char c : port)
            digits = digits && is_digit(c);
        if (!digits)
            return false;
        core = core.substr(0, colon);
    }
    if (core.empty() || !is_alnum(core.front()))
        return false;
    for (char c : core)
        if (!is_alnum(c) && c != '-' && c != '.' && c != '_')
            return false;
    const size_t dot = core.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= core.size())
        return false;
    const std::string_view last = core.substr(dot + 1);
    if (last.size() < 2)
        return false;
    for (char c : last)
        if (!is_alpha(c))
            return false;
    return !is_file_extension(lowercase(last));
}

/// `key=value` or `key:value` whose key names a secret: the length of the key
/// and separator, or 0.
size_t credential_key_length(std::string_view core) {
    static constexpr std::string_view kSecretWords[] = {
        "pass", "pwd", "token", "secret", "auth", "key", "credential", "cookie", "session"};
    const size_t sep = core.find_first_of("=:");
    if (sep == std::string_view::npos || sep == 0 || sep + 1 >= core.size())
        return 0;
    const std::string key = lowercase(core.substr(0, sep));
    for (char c : key)
        if (!is_word_char(c) && c != '.')
            return 0;
    for (auto w : kSecretWords)
        if (key.find(w) != std::string::npos)
            return sep + 1;
    return 0;
}

/// A word with a '/' that names no path: a step counter ("2/6") or a phrase.
bool is_safe_slash_word(std::string_view core) {
    static constexpr std::string_view kSafe[] = {"and/or", "n/a", "i/o", "tcp/ip", "read/write"};
    const std::string lower = lowercase(core);
    for (auto s : kSafe)
        if (lower == s)
            return true;
    const size_t slash = core.find('/');
    if (slash == 0 || slash + 1 >= core.size() || core.find('/', slash + 1) != std::string::npos)
        return false;
    for (size_t i = 0; i < core.size(); ++i)
        if (i != slash && !is_digit(core[i]))
            return false;
    return true;
}

/// One whitespace-delimited word with its surrounding punctuation already split off.
std::string scrub_core(std::string_view core) {
    // An address or name already replaced keeps only its placeholder: a port,
    // path or query after it can carry anything.
    if (auto p = leading_placeholder(core); !p.empty())
        return std::string(p);
    if (core.find("://") != std::string_view::npos)
        return "<url>";
    const size_t at = core.find('@');
    if (at != std::string_view::npos && at > 0 && at + 1 < core.size())
        return "<user@host>";
    if (const size_t key = credential_key_length(core))
        return std::string(core.substr(0, key)) + "<redacted>";
    if (core.find('/') != std::string_view::npos || core.front() == '~') {
        if (is_safe_slash_word(core))
            return std::string(core);
        // `key=/path` and `key:/path` keep their key.
        const size_t slash = core.find_first_of("/~");
        const std::string_view prefix = core.substr(0, slash);
        bool plain_key = !prefix.empty() && (prefix.back() == '=' || prefix.back() == ':');
        for (size_t i = 0; plain_key && i + 1 < prefix.size(); ++i)
            plain_key = is_word_char(prefix[i]) || prefix[i] == '.';
        return plain_key ? std::string(prefix) + "<path>" : "<path>";
    }
    if (looks_like_host(core))
        return "<host>";
    return std::string(core);
}

std::string scrub_word(std::string_view word) {
    static constexpr std::string_view kLead = "\"'([{<,";
    static constexpr std::string_view kTrail = "\"')]}>,;:.!?";
    size_t b = 0;
    size_t e = word.size();
    while (b < e && leading_placeholder(word.substr(b)).empty() &&
           kLead.find(word[b]) != std::string_view::npos)
        ++b;
    while (e > b && kTrail.find(word[e - 1]) != std::string_view::npos &&
           !(word[e - 1] == '>' && !leading_placeholder(word.substr(b, e - b)).empty()))
        --e;
    if (b == e)
        return std::string(word);
    return std::string(word.substr(0, b)) + scrub_core(word.substr(b, e - b)) +
           std::string(word.substr(e));
}

/// `text` cut to `max_len` bytes, backed off to a character boundary and out of
/// any placeholder the cut would split.
std::string cap_length(std::string text, size_t max_len) {
    if (text.size() <= max_len)
        return text;
    size_t cut = max_len;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    text.resize(cut);
    const size_t open = text.rfind('<');
    if (open != std::string::npos && text.find('>', open) == std::string::npos) {
        const std::string_view tail = std::string_view(text).substr(open);
        for (auto p : kPlaceholders) {
            if (p.substr(0, tail.size()) == tail) {
                text.resize(open);
                break;
            }
        }
    }
    return text;
}

} // namespace

std::string telemetry_text(std::string_view text, const std::vector<std::string>& identifiers,
                           size_t max_len) {
    std::string pass = redact_v4(redact_v6(text, 0, "<ip>"), 0, "<ip>");
    pass = replace_macs(pass);
    pass = replace_identifiers(std::move(pass), identifiers);
    std::string out;
    out.reserve(pass.size());
    // Words still to mask after a bare secret key ("Authorization: Bearer x").
    int mask_next = 0;
    size_t i = 0;
    while (i < pass.size()) {
        if (is_space(pass[i])) {
            out += pass[i++];
            continue;
        }
        const size_t start = i;
        while (i < pass.size() && !is_space(pass[i]))
            ++i;
        const std::string_view word = std::string_view(pass).substr(start, i - start);
        if (mask_next > 0) {
            --mask_next;
            const std::string lower = lowercase(word);
            if (lower == "bearer" || lower == "basic" || lower == "token")
                mask_next = 1;
            out += mask_next > 0 ? std::string(word) : "<redacted>";
            continue;
        }
        if ((word.back() == ':' || word.back() == '=') &&
            credential_key_length(std::string(word) + "x") == word.size())
            mask_next = 1;
        out += scrub_word(word);
    }
    return cap_length(std::move(out), max_len);
}

std::string ssid(std::string_view ssid) {
    return token(ssid, "net#", kSsidDomain, boot_salt());
}

std::string mac(std::string_view mac) {
    return token(mac, "mac#", kMacDomain, boot_salt());
}

std::string ssid_with_salt(std::string_view ssid, uint64_t salt) {
    return token(ssid, "net#", kSsidDomain, salt);
}

std::string mac_with_salt(std::string_view mac, uint64_t salt) {
    return token(mac, "mac#", kMacDomain, salt);
}

IpClass ip_class(std::string_view addr) {
    if (addr.empty())
        return IpClass::NotAnIp;
    if (addr.find(':') != std::string_view::npos) {
        uint8_t bytes[16];
        if (!parse_ipv6(addr, bytes))
            return IpClass::NotAnIp;
        return class_v6(bytes);
    }
    uint8_t quad[4];
    if (!parse_ipv4(addr, quad))
        return IpClass::NotAnIp;
    return class_v4(quad);
}

IpScope ip_scope(std::string_view addr) {
    switch (ip_class(addr)) {
    case IpClass::NotAnIp:
        return IpScope::NotAnIp;
    case IpClass::Global:
        return IpScope::Global;
    default:
        return IpScope::Local;
    }
}

std::string ip(std::string_view addr) {
    return ip_with_salt(addr, boot_salt());
}

std::string ip_with_salt(std::string_view addr, uint64_t salt) {
    if (ip_scope(addr) != IpScope::Global)
        return std::string(addr);
    return token(addr, "ip#", kIpDomain, salt);
}

std::string ips_in_text(std::string_view text) {
    if (text.empty())
        return {};
    const uint64_t salt = boot_salt();
    return redact_v4(redact_v6(text, salt), salt);
}

} // namespace helix::redact
