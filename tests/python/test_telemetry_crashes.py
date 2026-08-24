#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Tests for telemetry-crashes.py -- symbol fetch URLs, Thumb-bit masking, and
anomaly grouping.

Each case here pins behaviour that previously degraded silently: a wrong
fallback URL, an unmasked Thumb return address, and an ASLR-randomized
group key all produce a plausible-looking report rather than an error.
"""

import importlib
import sys
from pathlib import Path

import pytest

scripts_dir = Path(__file__).parent.parent.parent / "scripts"
sys.path.insert(0, str(scripts_dir))

tc = importlib.import_module("telemetry-crashes")


# ---------------------------------------------------------------------------
# Symbol fetch URLs
# ---------------------------------------------------------------------------

def test_github_fallback_uses_release_asset_name(monkeypatch, tmp_path):
    """GitHub release assets are symbols-<platform>.sym.zst, not <platform>.sym.zst.

    The flat layout only exists on R2. Using it for the GitHub fallback made
    every version missing from R2 fall back to raw hex addresses.
    """
    attempted = []

    def fake_download(url, dest):
        attempted.append(url)
        return 404  # every candidate misses; we only care about the URLs tried

    monkeypatch.setattr(tc, "_http_download", fake_download)
    monkeypatch.setattr(tc.shutil, "which", lambda _: "/usr/bin/zstd")

    cache = tc.SymbolCache()
    cache._fetch_symbols("0.99.106", "pi32", tmp_path / "pi32.sym")

    gh = [u for u in attempted if "github.com" in u]
    assert gh, "no GitHub fallback attempted"
    # Newer releases prefix the asset with symbols-; older ones don't. Both
    # naming schemes are live across the release history we still resolve.
    assert gh[0].endswith("/v0.99.106/symbols-pi32.sym.zst")
    assert any(u.endswith("/v0.99.106/pi32.sym.zst") for u in gh), gh


def test_r2_urls_keep_flat_layout(monkeypatch, tmp_path):
    """R2 really does use the flat <platform>.sym.zst layout -- don't 'fix' it."""
    attempted = []
    monkeypatch.setattr(tc, "_http_download", lambda url, dest: attempted.append(url) or 404)
    monkeypatch.setattr(tc.shutil, "which", lambda _: "/usr/bin/zstd")

    tc.SymbolCache()._fetch_symbols("0.99.106", "pi32", tmp_path / "pi32.sym")

    r2 = [u for u in attempted if "releases.helixscreen.org" in u]
    assert r2[0].endswith("/symbols/v0.99.106/pi32.sym.zst")


# ---------------------------------------------------------------------------
# Thumb-bit masking
# ---------------------------------------------------------------------------

@pytest.fixture
def symbols():
    # foo() at 0x1000, bar() at 0x2000, helix_lvgl_anomaly at 0x3000
    return tc.SymbolTable([(0x1000, "foo()"), (0x2000, "bar()"), (0x3000, "helix_lvgl_anomaly")])


def test_thumb_bit_masked_on_arm32_frames(symbols):
    """A Thumb LR has bit 0 set; nm addresses never do.

    Unmasked, an address at a function's first instruction resolves into the
    *previous* symbol -- and every offset in the report reads one byte high.
    """
    frames = tc.resolve_backtrace(["0x2001"], "pi32", symbols, load_base="0x0")
    assert frames[0]["resolved"] == "bar()"


def test_thumb_bit_not_masked_on_other_platforms(symbols):
    """aarch64/MIPS addresses are byte-exact -- masking there would corrupt them."""
    frames = tc.resolve_backtrace(["0x2001"], "pi", symbols, load_base="0x0")
    assert frames[0]["resolved"] == "bar()+0x1"


def test_thumb_platform_set_is_arm32_only():
    assert tc.THUMB_PLATFORMS == {"ad5m", "cc1", "pi32"}
    assert "k1" not in tc.THUMB_PLATFORMS  # MIPS
    assert "pi" not in tc.THUMB_PLATFORMS  # aarch64


# ---------------------------------------------------------------------------
# Anomaly grouping
# ---------------------------------------------------------------------------

def _anomaly(lr, anchor, device="dev1", ts="2026-08-13T00:00:00Z"):
    return {
        "event": "error_encountered",
        "code": "bg_tok_expired_check",
        "category": "display",
        "device_id": device,
        "timestamp": ts,
        "uptime_sec": 1,
        "context": f"lr={lr} tid=0x1 (likely L081 Mechanism C) | runtime_anchor={anchor}",
    }


class _NoSymbols(tc.SymbolCache):
    def get(self, version, platform):
        return None


def test_unresolved_anomalies_group_by_anchor_delta():
    """Same callsite, three launches, three ASLR bases -> one group.

    Keying on the raw LR reported N events from one callsite as N distinct
    callsites, which reads as a scattered problem instead of a single one.
    """
    # lr - anchor == -0x1000 in all three, under different load bases.
    anomalies = [
        _anomaly("0xaaaa1000", "0xaaaa2000"),
        _anomaly("0xbbbb1000", "0xbbbb2000"),
        _anomaly("0xcccc1000", "0xcccc2000"),
    ]
    result = tc.analyze_anomalies(anomalies, [], _NoSymbols(), platform_override="pi")

    assert result["total_anomalies"] == 3
    assert result["total_groups"] == 1
    assert result["groups"][0]["count"] == 3


def test_unresolved_distinct_callsites_stay_distinct():
    """The delta key must not over-merge -- different callsites stay apart."""
    anomalies = [
        _anomaly("0xaaaa1000", "0xaaaa2000"),  # delta -0x1000
        _anomaly("0xbbbb1800", "0xbbbb2000"),  # delta -0x800
    ]
    result = tc.analyze_anomalies(anomalies, [], _NoSymbols(), platform_override="pi")
    assert result["total_groups"] == 2


def test_unresolved_group_key_is_version_and_platform_scoped():
    """Offsets are only comparable within one binary, so the key says which."""
    result = tc.analyze_anomalies(
        [_anomaly("0xaaaa1000", "0xaaaa2000")], [], _NoSymbols(), platform_override="pi32"
    )
    key = result["groups"][0]["lr_symbol"]
    assert "pi32" in key and "unresolved" in key


def test_anomaly_without_anchor_falls_back_to_raw_lr():
    """Pre-v0.99.60 bundles have no anchor -- raw LR is all there is."""
    ev = _anomaly("0xaaaa1000", "0x0")
    ev["context"] = "lr=0xaaaa1000 tid=0x1 (likely L081 Mechanism C)"
    result = tc.analyze_anomalies([ev], [], _NoSymbols(), platform_override="pi")
    assert result["groups"][0]["lr_symbol"] == "0xaaaa1000"


def test_gh_cli_fallback_tried_when_http_fails(monkeypatch, tmp_path):
    """Draft releases 404 on the public URL but download fine through gh."""
    monkeypatch.setattr(tc, "_http_download", lambda url, dest: 404)
    monkeypatch.setattr(tc.shutil, "which", lambda name: f"/usr/bin/{name}")

    calls = []

    def fake_run(argv, **kwargs):
        calls.append(argv)
        if argv[0] == "gh":
            Path(argv[argv.index("--output") + 1]).write_bytes(b"stub")
            return None
        # zstd decompress -> write the .sym the caller expects
        Path(argv[argv.index("-o") + 1]).write_text("00001000 T foo()\n")
        return None

    monkeypatch.setattr(tc.subprocess, "run", fake_run)

    assert tc.SymbolCache()._fetch_symbols("0.99.96", "pi32", tmp_path / "pi32.sym") is True
    gh_calls = [c for c in calls if c[0] == "gh"]
    assert gh_calls, "gh fallback never attempted"
    assert gh_calls[0][:4] == ["gh", "release", "download", "v0.99.96"]


def test_gh_cli_fallback_skipped_when_gh_absent(monkeypatch, tmp_path):
    monkeypatch.setattr(tc, "_http_download", lambda url, dest: 404)
    monkeypatch.setattr(tc.shutil, "which", lambda name: None if name == "gh" else "/usr/bin/zstd")

    cache = tc.SymbolCache()
    assert cache._fetch_symbols("0.99.96", "pi32", tmp_path / "pi32.sym") is False
    assert any("symbols not available" in w for w in cache.warnings)
