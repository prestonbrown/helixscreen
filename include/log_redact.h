// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file log_redact.h
 * @brief Privacy-preserving identifiers for network names in log output.
 *
 * The in-memory log ring (see logging_init.cpp) is captured at debug level
 * regardless of the user's configured verbosity, and is uploaded verbatim by
 * the debug bundle, the crash reporter, and the `ctl log` RPC. Anything logged
 * at debug or above leaves the machine.
 *
 * A WiFi SSID is not just a string: a set of nearby SSIDs with signal strengths
 * resolves to a street address through public WiFi-positioning databases, and
 * the neighbouring networks in a scan belong to people who never consented to
 * being in anyone's bug report. A BSSID geolocates even more directly.
 *
 * Downstream scrubbing cannot fix this — an SSID is an arbitrary user-chosen
 * string with no pattern for a regex to match. So the redaction happens here,
 * at the call site, before the text ever reaches a sink.
 *
 * These functions return a short, stable, per-boot token: enough to correlate
 * "the same network" across lines within one session, which is all diagnostics
 * actually need, and useless for identifying the network or its location. The
 * salt is regenerated every boot, so tokens cannot be correlated across
 * sessions or compared against a precomputed table of common SSIDs.
 *
 * Use the raw value only at trace level, which the ring never captures.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace helix::redact {

/**
 * @brief Reduce an SSID to a non-identifying, session-stable token.
 *
 * @param ssid Network name (may be empty).
 * @return Token of the form `net#a3f1`, or `net#<none>` when @p ssid is empty.
 *
 * The same SSID yields the same token for the lifetime of the process and a
 * different one after a restart. Safe to log at any level.
 */
std::string ssid(std::string_view ssid);

/**
 * @brief Reduce a MAC or BSSID to a non-identifying, session-stable token.
 *
 * @param mac Hardware address in any format (may be empty).
 * @return Token of the form `mac#7b2c`, or `mac#<none>` when @p mac is empty.
 *
 * Applies to the adapter's own MAC and to AP BSSIDs alike. A BSSID is the most
 * directly geolocatable field in a scan result — never log the raw value above
 * trace.
 */
std::string mac(std::string_view mac);

/**
 * @brief Scope of a textual IP address, for redaction decisions.
 *
 * The distinction that matters for privacy is not IPv4-vs-IPv6, it is
 * private-vs-routable. `192.168.1.50` describes a LAN topology that is
 * identical in millions of houses and is exactly what a "can the screen reach
 * Moonraker" diagnosis needs. A globally routable address is the opposite:
 * an ISP-allocated prefix plus a stable interface ID resolves to a household,
 * and it answers no diagnostic question that the private address does not.
 */
enum class IpScope {
    NotAnIp, ///< Not a syntactically valid IPv4 or IPv6 literal.
    Local,   ///< Private, loopback, link-local, CGNAT, ULA, or multicast.
    Global,  ///< Globally routable — identifies a household or a business.
};

/**
 * @brief Classify a single IP literal.
 *
 * @param addr One address, with nothing around it (no port, no scope id, no
 *             brackets). Anything that does not parse is @ref IpScope::NotAnIp.
 *
 * Local covers IPv4 10/8, 172.16/12, 192.168/16, 127/8, 169.254/16, 100.64/10,
 * 0/8, 224/4 and above; and IPv6 ::, ::1, fe80::/10, fc00::/7, ff00::/8, plus
 * the IPv4-mapped/-compatible forms classified by their embedded IPv4.
 *
 * The documentation ranges (192.0.2/24, 198.51.100/24, 203.0.113/24,
 * 2001:db8::/32) are deliberately Global: they are routable-shaped, and tests
 * need a routable fixture that is not somebody's real address.
 */
IpScope ip_scope(std::string_view addr);

/**
 * @brief Reduce a routable IP address to a non-identifying token.
 *
 * @param addr One address (see @ref ip_scope).
 * @return @p addr verbatim when it is Local or not an address at all;
 *         otherwise a token of the form `ip#4f2a91`.
 *
 * Same session-stable, salted-per-boot token as ssid()/mac(), for the same
 * reason: "the same peer across these lines" is the whole diagnostic value,
 * and it survives redaction while the address itself does not.
 */
std::string ip(std::string_view addr);

/**
 * @brief Apply ip() to every address literal embedded in free text.
 *
 * @param text A log line, a URL, a JSON string value — anything.
 * @return @p text with each globally routable IPv4/IPv6 literal replaced by its
 *         ip() token, and every private/local literal left untouched.
 *
 * Boundary-checked so dotted-quad lookalikes survive: `v1.2.3.4`, `1.2.3.4.5`
 * and `2026.08.25.1` are not addresses and are not rewritten. A port suffix
 * (`203.0.113.5:7125`) and a scope id (`fe80::1%wlan0`) are handled — the
 * address is matched, the suffix is left alone.
 */
std::string ips_in_text(std::string_view text);

/**
 * @brief Deterministic variants that take an explicit salt.
 *
 * `ssid()` and `mac()` are these, applied to the process-wide per-boot salt.
 * Use these directly only when reproducibility matters — resolving an old
 * bundle against a known salt, or asserting on a token in a test. Passing a
 * fixed salt in production would defeat the point of salting.
 */
std::string ssid_with_salt(std::string_view ssid, uint64_t salt);
std::string mac_with_salt(std::string_view mac, uint64_t salt);
std::string ip_with_salt(std::string_view addr, uint64_t salt);

} // namespace helix::redact
