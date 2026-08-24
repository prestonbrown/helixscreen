// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

// Boot-splash lifetime + status policy.
//
// On slow devices the launcher/init gate waits for Moonraker (up to ~120s)
// BEFORE helix-screen launches. helix-splash is pre-started by the init script,
// so without help it would hit its fixed self-timeout and leave a blank screen
// until the gate clears. The gate now writes a heartbeat/status file while it
// waits; the splash treats each observed change to that file as a heartbeat and
// stays alive (showing the status message) until the gate hands off or an
// absolute backstop trips.
//
// All lifetime math runs in the MONOTONIC clock domain. File mtime (wall clock)
// is used ONLY to detect that the file changed — never to measure age — because
// these devices routinely sync NTP mid-boot and jump the wall clock, which would
// otherwise make a live heartbeat look stale and reintroduce the blank screen.
namespace helix::splash {

struct SplashLifetimePolicy {
    int default_cap_sec = 30;   // no-heartbeat behavior (unchanged legacy cap)
    int absolute_max_sec = 180; // hard backstop regardless of heartbeats
};

// Should the splash keep running?
//   start_mono / now_mono       monotonic seconds
//   last_heartbeat_mono         monotonic time of the last observed heartbeat,
//                               or < 0 if no heartbeat has ever been seen.
//
// Once the gate has driven us at least once (a heartbeat was seen), we stay up
// until helix-screen sends SIGUSR1 (it does so when discovery completes / it is
// ready to paint) or the absolute backstop trips. We deliberately do NOT fall
// back to the short default cap after the heartbeats stop: the gap between the
// gate finishing and helix-screen's first paint is ~20s on a cold K2, and
// helix-screen suppresses its own rendering until the splash exits — so exiting
// early there leaves a blank screen. The default cap applies ONLY when no
// heartbeat was ever seen (gate-less platforms with no SIGUSR1 driver writing
// the status file).
inline bool splash_should_continue(const SplashLifetimePolicy& p, long start_mono, long now_mono,
                                   long last_heartbeat_mono) {
    const long age = now_mono - start_mono;
    if (age >= p.absolute_max_sec) {
        return false; // hard backstop wins over everything
    }
    if (last_heartbeat_mono >= 0) {
        return true; // gate drove us; wait for SIGUSR1 (or the backstop)
    }
    return age < p.default_cap_sec; // no heartbeat ever: original behavior
}

// Parse `MemAvailable` (in KiB) from the contents of /proc/meminfo.
// Returns -1 if the field is absent (caller must then NOT enforce a floor —
// we never guess memory pressure).
inline long parse_meminfo_available_kb(const std::string& meminfo) {
    static const char* KEY = "MemAvailable:";
    const size_t key_at = meminfo.find(KEY);
    if (key_at == std::string::npos) {
        return -1;
    }
    size_t i = key_at + 13; // strlen("MemAvailable:")
    while (i < meminfo.size() && (meminfo[i] == ' ' || meminfo[i] == '\t')) {
        ++i;
    }
    long val = 0;
    bool any = false;
    while (i < meminfo.size() && meminfo[i] >= '0' && meminfo[i] <= '9') {
        val = val * 10 + (meminfo[i] - '0');
        ++i;
        any = true;
    }
    return any ? val : -1;
}

// Memory safety valve. The splash is the lightweight stand-in that lets
// Moonraker boot before helix-screen launches; on tight-RAM devices (e.g.
// AD5M Forge-X ~107 MB) it must never be the process that tips the system into
// OOM. If free memory falls below the floor the splash voluntarily exits,
// freeing its own ~10-15 MB. A floor of <= 0, or an unknown reading (< 0),
// disables the valve.
inline bool splash_memory_ok(long mem_available_kb, long min_free_kb) {
    if (min_free_kb <= 0 || mem_available_kb < 0) {
        return true;
    }
    return mem_available_kb >= min_free_kb;
}

// Normalize a status message read from the heartbeat file for display:
// strip a single trailing newline and any trailing whitespace, and clamp to a
// sane length so a malformed file can't blow up the label.
inline std::string sanitize_splash_message(std::string msg, size_t max_len = 96) {
    while (!msg.empty() &&
           (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ' || msg.back() == '\t')) {
        msg.pop_back();
    }
    // Keep only the first line — the file's mtime is the heartbeat, the first
    // line is the message.
    const size_t nl = msg.find('\n');
    if (nl != std::string::npos) {
        msg.erase(nl);
    }
    if (msg.size() > max_len) {
        msg.resize(max_len);
    }
    return msg;
}

// Compose the status line actually shown on the splash: the gate-provided label
// plus a continuously-rising elapsed-seconds counter that the splash owns from
// its own monotonic start. The gates write only the label (e.g. "Starting
// Klipper…") — the seconds are appended here so the count keeps climbing through
// helix-screen's own startup phase too, after the gate has handed off and
// stopped rewriting the file. An empty label yields an empty line (the splash
// shows no bare counter before it has any status to report).
inline std::string compose_splash_status(const std::string& label, long elapsed_sec) {
    if (label.empty()) {
        return std::string();
    }
    if (elapsed_sec < 0) {
        elapsed_sec = 0;
    }
    return label + " " + std::to_string(elapsed_sec) + "s";
}

} // namespace helix::splash
