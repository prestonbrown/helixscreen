#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: network-identifying values must not be logged above trace level.
#
# Background: the in-memory log ring (logging_init.cpp) is captured at debug
# level regardless of the user's configured verbosity, and leaves the machine
# three ways — the debug bundle, the crash reporter's automatic upload, and the
# `ctl log` RPC. `log_tail` bypasses the bundle's redaction pipeline entirely,
# and no downstream regex can fix that: an SSID is an arbitrary user-chosen
# string with no pattern to match.
#
# Why it matters: a set of nearby SSIDs with signal strengths is a geolocation
# fingerprint — public WiFi-positioning databases resolve it to a street
# address. A scan enumerates the *neighbours'* networks too, and they never
# consented to appearing in anyone's bug report. A BSSID geolocates directly.
#
# So SSIDs, BSSIDs and MACs may appear at trace (never captured by the ring),
# or as a helix::redact:: token at any level. Not raw at debug or above.
#
# Approved:
#
#   spdlog::trace("[WifiBackend] Parsed network: '{}'", ssid);
#   spdlog::debug("[WifiBackend] Status: ssid={}", helix::redact::ssid(status.ssid));
#   spdlog::info("[WiFiManager] Connecting to {}", redact::ssid(target));
#
# Banned (without a // PII_OK comment):
#
#   spdlog::debug("[WifiBackend] Status: ssid='{}'", status.ssid);
#   spdlog::info("[WiFiManager] Connected to '{}'", network.ssid);
#   spdlog::debug("[wifi_ui] MAC for '{}': {}", iface, mac_address);
#
# Per-line opt-out:
#
#   spdlog::debug("...", ssid); // PII_OK: mock backend, SSIDs are fixtures
#
# Usage:
#   ./scripts/check_wifi_pii_logging.py [files...]
#   ./scripts/check_wifi_pii_logging.py --staged-only
#   (no args = scan src/ and include/ recursively)

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

# Log calls that reach the ring buffer. spdlog::trace is deliberately absent:
# the ring's level is debug even when the logger is at trace, so trace lines
# never leave the machine.
LOG_CALL_RE = re.compile(
    r"\b(?:spdlog::(?:debug|info|warn|warning|error|critical)"
    r"|LOG_(?:ERROR|WARN)_INTERNAL"
    r"|NOTIFY_(?:ERROR|WARNING|INFO|SUCCESS)(?:_T|_MODAL)?)\s*\(",
)

# Identifiers whose value is network-identifying. Matched against the argument
# text of a log call after literals and comments have been stripped.
PII_PATTERNS = [
    # "ssid" is matched as a bare substring, deliberately. Member and parameter
    # names bury it inside a larger identifier -- connecting_ssid_, current_ssid_,
    # target_ssid, item_data->ssid -- and a \b-anchored pattern misses every one
    # of those, because '_' is a word character. Two real sites in
    # wifi_backend_macos.mm slipped through a \bssid\b version of this gate.
    # No other identifier in this codebase contains the letters "ssid", and
    # format strings and comments are stripped before matching, so the loose
    # match costs nothing.
    (re.compile(
        r"ssid(?!_(?:result|cmd|command|reply|response|status|len|length|size|count"
        r"|ok|err|error|found|present|valid|set|list)\b)",
        re.IGNORECASE), "SSID"),
    # "mac" cannot be matched loosely -- it is a substring of macro, machine,
    # format_mac, mac_os and plenty more. Anchor it to identifier boundaries,
    # where '_' counts as a boundary rather than a word character.
    (re.compile(r"(?:^|[^A-Za-z0-9_])mac(?:$|[^A-Za-z0-9_])", re.IGNORECASE), "MAC"),
    (re.compile(r"\bmac_addr(?:ess)?\b", re.IGNORECASE), "MAC"),
    (re.compile(r"\bget_mac_address\s*\(", re.IGNORECASE), "MAC"),
    (re.compile(r"[A-Za-z0-9]_mac(?:$|[^A-Za-z0-9])", re.IGNORECASE), "MAC"),
    # Deliberately no generic `mac_<something>` prefix rule: it matches
    # mac_os_version and similar. The only real prefixed spellings are
    # mac_addr/mac_address, which the rule above already covers.
]

# Calls whose result is already safe. Their arguments are removed before the
# PII scan so that redact::ssid(status.ssid) does not flag on `status.ssid`.
REDACT_CALL_RE = re.compile(r"(?:\bhelix::)?\bredact::(?:ssid|mac)\s*\(")

# Expressions that mention an identifier but yield its length or emptiness
# rather than its value. `mac.size()` and `bt_mac.empty()` disclose nothing, and
# both appear alongside a properly redacted value on the same log line.
NON_VALUE_ACCESSOR_RE = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*\s*(?:\.|->)\s*(?:empty|size|length)\s*\(\s*\)")

OPT_OUT_RE = re.compile(r"//\s*PII_OK\b")

SCAN_DIRS = ("src", "include", "firmware")
SCAN_SUFFIXES = (".cpp", ".cc", ".h", ".hpp", ".mm")

# firmware/native-audit is the Phase 0 ESP32 feasibility audit - self-described
# throwaway scaffolding kept only so the experiment can be reproduced. Its
# `overrides/` are frozen June 2026 snapshots of src/ files, so they predate
# fixes that have since landed upstream (the MAC redaction in
# ui_settings_label_printer.cpp among them). Re-fixing a frozen snapshot would
# defeat the point of freezing it, and none of it ships.
EXCLUDE_PREFIXES = ("firmware/native-audit/",)


def is_excluded(path) -> bool:
    posix = str(path).replace("\\", "/")
    return posix.startswith(EXCLUDE_PREFIXES)


def strip_noise(text: str) -> str:
    """Blank out comments and string-literal bodies, preserving offsets.

    Every removed character becomes a space and newlines are kept, so byte
    offsets and line numbers still map back to the original file. This is what
    stops the gate matching a comment that *describes* the anti-pattern, or a
    format string that merely contains the word "ssid" -- the mistake the L081
    gate shipped with (see tests/shell/test_l081_gate.bats).
    """
    out = list(text)
    i, n = 0, len(text)
    state = None  # None | 'line' | 'block' | 'str' | 'char' | 'raw'
    raw_delim = ""

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state is None:
            if c == "/" and nxt == "/":
                state = "line"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            # Raw string: R"delim( ... )delim"
            if c == "R" and nxt == '"':
                m = re.match(r'R"([^(]{0,16})\(', text[i:])
                if m:
                    raw_delim = ")" + m.group(1) + '"'
                    state = "raw"
                    for k in range(i, i + m.end()):
                        out[k] = " "
                    i += m.end()
                    continue
            if c == '"':
                state = "str"
                i += 1  # keep the opening quote so the token still parses
                continue
            if c == "'":
                state = "char"
                i += 1
                continue
            i += 1
            continue

        if state == "line":
            if c == "\n":
                state = None
            else:
                out[i] = " "
            i += 1
            continue

        if state == "block":
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = None
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue

        if state == "raw":
            if text.startswith(raw_delim, i):
                for k in range(i, i + len(raw_delim)):
                    out[k] = " "
                i += len(raw_delim)
                state = None
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue

        # 'str' or 'char'
        if c == "\\":
            out[i] = " "
            if i + 1 < n and text[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
            continue
        if (state == "str" and c == '"') or (state == "char" and c == "'"):
            state = None
            i += 1
            continue
        if c != "\n":
            out[i] = " "
        i += 1

    return "".join(out)


def call_span(text: str, open_paren: int) -> int:
    """Return the index just past the ')' matching the '(' at open_paren."""
    depth = 0
    i = open_paren
    n = len(text)
    while i < n:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def remove_redacted_args(body: str) -> str:
    """Drop already-safe sub-expressions from a log call's argument text.

    Two kinds: redact::ssid(...) / redact::mac(...) wrappers, and accessors that
    yield only a length or emptiness. Removing both first means the PII scan
    sees just the argument text that still carries a raw value.
    """
    while True:
        m = REDACT_CALL_RE.search(body)
        if not m:
            break
        end = call_span(body, m.end() - 1)
        body = body[: m.start()] + " " + body[end:]
    return NON_VALUE_ACCESSOR_RE.sub(" ", body)


def check_file(path: Path) -> list[tuple[int, str, str]]:
    try:
        original = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []

    if "spdlog::" not in original and "_INTERNAL" not in original:
        return []

    code = strip_noise(original)
    lines = original.splitlines()
    findings: list[tuple[int, str, str]] = []

    for m in LOG_CALL_RE.finditer(code):
        open_paren = m.end() - 1
        end = call_span(code, open_paren)
        body = remove_redacted_args(code[open_paren:end])

        hit = None
        for pattern, kind in PII_PATTERNS:
            if pattern.search(body):
                hit = kind
                break
        if not hit:
            continue

        start_line = code.count("\n", 0, m.start()) + 1
        end_line = code.count("\n", 0, end) + 1

        # Opt-out may sit on any line of the call, in the ORIGINAL text --
        # the comment was blanked out of `code`.
        if any(
            OPT_OUT_RE.search(lines[i - 1])
            for i in range(start_line, min(end_line, len(lines)) + 1)
        ):
            continue

        snippet = lines[start_line - 1].strip() if start_line <= len(lines) else ""
        findings.append((start_line, hit, snippet))

    return findings


def iter_targets(explicit: list[str]) -> Iterable[Path]:
    if explicit:
        for f in explicit:
            p = Path(f)
            if p.suffix in SCAN_SUFFIXES and p.is_file() and not is_excluded(p):
                yield p
        return
    for d in SCAN_DIRS:
        root = Path(d)
        if not root.is_dir():
            continue
        for suffix in SCAN_SUFFIXES:
            for p in root.rglob(f"*{suffix}"):
                if not is_excluded(p):
                    yield p


def staged_files() -> list[str]:
    try:
        out = subprocess.run(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
            capture_output=True, text=True, check=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    return [f for f in out.splitlines() if f.endswith(SCAN_SUFFIXES)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="*", help="files to scan (default: src/ include/)")
    ap.add_argument("--staged-only", action="store_true", help="scan staged files only")
    args = ap.parse_args()

    targets = staged_files() if args.staged_only else args.files
    if args.staged_only and not targets:
        return 0

    total = 0
    for path in sorted(set(iter_targets(targets))):
        for line, kind, snippet in check_file(path):
            total += 1
            print(f"{path}:{line}: {kind} logged above trace level")
            print(f"    {snippet}")

    if total:
        print()
        print(f"❌ {total} site(s) log network-identifying values above trace level.")
        print()
        print("   The log ring is captured at debug regardless of verbosity and is")
        print("   uploaded by the debug bundle, the crash reporter, and `ctl log`.")
        print("   A set of nearby SSIDs is a geolocation fingerprint, and a scan")
        print("   enumerates the neighbours' networks too.")
        print()
        print("   Fix: wrap with helix::redact::ssid() / ::mac() (see include/log_redact.h),")
        print("   or drop the call to spdlog::trace, which the ring never captures.")
        print("   Deliberate exception: append  // PII_OK: <reason>")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
