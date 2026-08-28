#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Tests for telemetry-analyze.py -- TelemetryAnalyzer class.

Covers event loading, adoption metrics, print metrics, crash metrics,
and edge cases with missing/malformed data.
"""

import json
import sys
from pathlib import Path

import pytest

# Add scripts directory to path
scripts_dir = Path(__file__).parent.parent.parent / "scripts"
sys.path.insert(0, str(scripts_dir))

# Import after path setup -- module uses dashes so we need importlib
import importlib

telemetry_analyze = importlib.import_module("telemetry-analyze")
TelemetryAnalyzer = telemetry_analyze.TelemetryAnalyzer
bucket_value = telemetry_analyze.bucket_value
RAM_BUCKETS = telemetry_analyze.RAM_BUCKETS
UPTIME_BUCKETS = telemetry_analyze.UPTIME_BUCKETS
NOZZLE_TEMP_BUCKETS = telemetry_analyze.NOZZLE_TEMP_BUCKETS
BED_TEMP_BUCKETS = telemetry_analyze.BED_TEMP_BUCKETS


# =============================================================================
# Helpers
# =============================================================================


def make_session_event(
    device_id="dev-001",
    timestamp="2026-02-09T10:00:00Z",
    version="1.2.0",
    platform="linux-aarch64",
    arch="aarch64",
    ram_mb=1024,
    printer_model="Voron 2.4",
    kinematics="corexy",
    features=None,
    display="800x480",
    locale="en",
    theme="dark",
    klipper_version="0.12.0",
):
    """Build a session event dict matching the schema the analyzer expects."""
    return {
        "schema_version": 2,
        "event": "session",
        "device_id": device_id,
        "timestamp": timestamp,
        "app": {
            "version": version,
            "platform": platform,
            "display": display,
            "locale": locale,
            "theme": theme,
        },
        "host": {
            "arch": arch,
            "os": "linux",
            "ram_total_mb": ram_mb,
        },
        "printer": {
            "detected_model": printer_model,
            "kinematics": kinematics,
            "klipper_version": klipper_version,
        },
        "features": features if features is not None else ["telemetry"],
    }


def make_print_event(
    device_id="dev-001",
    timestamp="2026-02-09T12:00:00Z",
    outcome="success",
    duration_sec=3600,
    filament_type="PLA",
    nozzle_temp=210,
    bed_temp=60,
    phases_completed=10,
):
    """Build a print_outcome event dict."""
    return {
        "schema_version": 2,
        "event": "print_outcome",
        "device_id": device_id,
        "timestamp": timestamp,
        "outcome": outcome,
        "duration_sec": duration_sec,
        "filament_type": filament_type,
        "nozzle_temp": nozzle_temp,
        "bed_temp": bed_temp,
        "phases_completed": phases_completed,
    }


def make_crash_event(
    device_id="dev-001",
    timestamp="2026-02-09T14:00:00Z",
    signal=11,
    signal_name="SIGSEGV",
    app_version="1.2.0",
    uptime_sec=7200,
):
    """Build a crash event dict."""
    return {
        "schema_version": 2,
        "event": "crash",
        "device_id": device_id,
        "timestamp": timestamp,
        "signal": signal,
        "signal_name": signal_name,
        "app_version": app_version,
        "uptime_sec": uptime_sec,
    }


def write_event_file(tmp_path, events, year="2026", month="02", day="09", name="batch"):
    """Write a batch of events as a JSON file in the R2 directory structure."""
    event_dir = tmp_path / year / month / day
    event_dir.mkdir(parents=True, exist_ok=True)
    fpath = event_dir / f"{name}.json"
    fpath.write_text(json.dumps(events))
    return fpath


# =============================================================================
# Test: bucket_value helper
# =============================================================================


class TestBucketValue:
    """Tests for the bucket_value() helper function."""

    def test_ram_bucket_low(self):
        """256MB RAM falls into <512MB bucket."""
        assert bucket_value(256, RAM_BUCKETS) == "<512MB"

    def test_ram_bucket_mid(self):
        """1024MB RAM falls into 1-2GB bucket."""
        assert bucket_value(1024, RAM_BUCKETS) == "1-2GB"

    def test_ram_bucket_high(self):
        """16384MB RAM falls into 8GB+ bucket."""
        assert bucket_value(16384, RAM_BUCKETS) == "8GB+"

    def test_bucket_none_returns_unknown(self):
        """None value returns 'unknown'."""
        assert bucket_value(None, RAM_BUCKETS) == "unknown"

    def test_bucket_nan_returns_unknown(self):
        """NaN value returns 'unknown'."""
        import math
        assert bucket_value(float("nan"), RAM_BUCKETS) == "unknown"

    def test_uptime_bucket_short(self):
        """30s uptime falls into <1min bucket."""
        assert bucket_value(30, UPTIME_BUCKETS) == "<1min"

    def test_uptime_bucket_long(self):
        """20000s uptime falls into 4hr+ bucket."""
        assert bucket_value(20000, UPTIME_BUCKETS) == "4hr+"

    def test_nozzle_temp_bucket(self):
        """210C nozzle temp falls into 200-220C bucket."""
        assert bucket_value(210, NOZZLE_TEMP_BUCKETS) == "200-220C"

    def test_bed_temp_bucket(self):
        """60C bed temp falls into 60-80C bucket."""
        assert bucket_value(60, BED_TEMP_BUCKETS) == "60-80C"


# =============================================================================
# Test: Event Loading
# =============================================================================


class TestLoadEvents:
    """Tests for TelemetryAnalyzer.load_events()."""

    def test_loads_json_files_from_directory(self, tmp_path):
        """Events are loaded from JSON files in nested directory structure."""
        events = [
            make_session_event(),
            make_print_event(),
            make_crash_event(),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        assert len(analyzer.all_events) == 3
        assert len(analyzer.sessions) == 1
        assert len(analyzer.prints) == 1
        assert len(analyzer.crashes) == 1

    def test_flattens_session_fields(self, tmp_path):
        """Session events have nested app/host/printer fields flattened."""
        events = [make_session_event(version="2.0.0", arch="x86_64", printer_model="Prusa MK4")]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        row = analyzer.sessions.iloc[0]
        assert row["app.version"] == "2.0.0"
        assert row["host.arch"] == "x86_64"
        assert row["printer.detected_model"] == "Prusa MK4"

    def test_handles_batch_files(self, tmp_path):
        """JSON files containing arrays of events are handled correctly."""
        batch = [
            make_session_event(device_id="dev-001"),
            make_session_event(device_id="dev-002"),
            make_session_event(device_id="dev-003"),
        ]
        write_event_file(tmp_path, batch)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        assert len(analyzer.sessions) == 3
        device_ids = set(analyzer.sessions["device_id"])
        assert device_ids == {"dev-001", "dev-002", "dev-003"}

    def test_handles_single_event_file(self, tmp_path):
        """A JSON file containing a single event dict (not array) is loaded."""
        event_dir = tmp_path / "2026" / "02" / "09"
        event_dir.mkdir(parents=True)
        fpath = event_dir / "single.json"
        fpath.write_text(json.dumps(make_session_event()))

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        assert len(analyzer.sessions) == 1

    def test_skips_malformed_json(self, tmp_path):
        """Malformed JSON files are skipped without crashing."""
        event_dir = tmp_path / "2026" / "02" / "09"
        event_dir.mkdir(parents=True)
        (event_dir / "good.json").write_text(json.dumps([make_session_event()]))
        (event_dir / "bad.json").write_text("{not valid json!!!")

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        assert len(analyzer.sessions) == 1

    def test_empty_directory_yields_empty_dataframes(self, tmp_path):
        """Empty data directory results in empty DataFrames."""
        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        assert analyzer.all_events.empty
        assert analyzer.sessions.empty
        assert analyzer.prints.empty
        assert analyzer.crashes.empty

    def test_nonexistent_directory_yields_empty_dataframes(self, tmp_path):
        """Non-existent data directory results in empty DataFrames."""
        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path / "does_not_exist"))

        assert analyzer.all_events.empty

    def test_since_filter(self, tmp_path):
        """Events before --since date are excluded."""
        events = [
            make_session_event(timestamp="2026-01-01T00:00:00Z", device_id="old"),
            make_session_event(timestamp="2026-02-09T00:00:00Z", device_id="new"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path), since="2026-02-01")

        assert len(analyzer.sessions) == 1
        assert analyzer.sessions.iloc[0]["device_id"] == "new"

    def test_until_filter(self, tmp_path):
        """Events after --until date are excluded (inclusive of until date)."""
        events = [
            make_session_event(timestamp="2026-02-09T10:00:00Z", device_id="in-range"),
            make_session_event(timestamp="2026-02-11T10:00:00Z", device_id="out-of-range"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path), until="2026-02-09")

        assert len(analyzer.sessions) == 1
        assert analyzer.sessions.iloc[0]["device_id"] == "in-range"

    def test_multiple_files_across_directories(self, tmp_path):
        """Events from multiple date directories are aggregated."""
        write_event_file(
            tmp_path,
            [make_session_event(device_id="d1")],
            year="2026", month="02", day="08", name="batch1",
        )
        write_event_file(
            tmp_path,
            [make_session_event(device_id="d2")],
            year="2026", month="02", day="09", name="batch2",
        )

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        assert len(analyzer.sessions) == 2

    def test_features_field_preserved_as_list(self, tmp_path):
        """The features field from session events is preserved as a list."""
        events = [make_session_event(features=["telemetry", "auto_update"])]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))

        feat = analyzer.sessions.iloc[0]["features"]
        assert isinstance(feat, list)
        assert "telemetry" in feat
        assert "auto_update" in feat


# =============================================================================
# Test: Adoption Metrics
# =============================================================================


class TestAdoptionMetrics:
    """Tests for TelemetryAnalyzer.compute_adoption_metrics()."""

    def test_no_sessions_returns_note(self):
        """Empty sessions DataFrame returns a note, not metrics."""
        analyzer = TelemetryAnalyzer()
        result = analyzer.compute_adoption_metrics()
        assert result == {"note": "No session data"}

    def test_unique_device_count(self, tmp_path):
        """Counts distinct device_id values."""
        events = [
            make_session_event(device_id="dev-001"),
            make_session_event(device_id="dev-002"),
            make_session_event(device_id="dev-001"),  # duplicate
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        assert result["total_unique_devices"] == 2

    def test_daily_active_devices(self, tmp_path):
        """Daily active devices are computed per calendar date."""
        events = [
            make_session_event(device_id="d1", timestamp="2026-02-08T10:00:00Z"),
            make_session_event(device_id="d2", timestamp="2026-02-08T11:00:00Z"),
            make_session_event(device_id="d1", timestamp="2026-02-09T10:00:00Z"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        daily = result["active_devices_daily"]
        assert daily["2026-02-08"] == 2
        assert daily["2026-02-09"] == 1

    def test_platform_distribution(self, tmp_path):
        """Platform distribution counts each platform occurrence."""
        events = [
            make_session_event(device_id="d1", platform="linux-aarch64"),
            make_session_event(device_id="d2", platform="linux-aarch64"),
            make_session_event(device_id="d3", platform="linux-x86_64"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        dist = result["platform_distribution"]
        assert dist["linux-aarch64"] == 2
        assert dist["linux-x86_64"] == 1

    def test_version_distribution(self, tmp_path):
        """App version distribution counts occurrences per version."""
        events = [
            make_session_event(device_id="d1", version="1.0.0"),
            make_session_event(device_id="d2", version="1.0.0"),
            make_session_event(device_id="d3", version="1.1.0"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        assert result["app_version_distribution"]["1.0.0"] == 2
        assert result["app_version_distribution"]["1.1.0"] == 1

    def test_printer_model_distribution(self, tmp_path):
        """Printer model top-20 distribution is computed."""
        events = [
            make_session_event(device_id="d1", printer_model="Voron 2.4"),
            make_session_event(device_id="d2", printer_model="Voron 2.4"),
            make_session_event(device_id="d3", printer_model="Prusa MK4"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        assert result["printer_model_top20"]["Voron 2.4"] == 2
        assert result["printer_model_top20"]["Prusa MK4"] == 1

    def test_ram_bucketing(self, tmp_path):
        """RAM values are bucketed into labeled ranges."""
        events = [
            make_session_event(device_id="d1", ram_mb=512),
            make_session_event(device_id="d2", ram_mb=1024),
            make_session_event(device_id="d3", ram_mb=4096),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        ram = result["ram_distribution"]
        assert ram["512MB-1GB"] == 1
        assert ram["1-2GB"] == 1
        assert ram["4-8GB"] == 1

    def test_feature_adoption_rates(self, tmp_path):
        """Feature adoption rates are computed as percentages."""
        events = [
            make_session_event(device_id="d1", features=["telemetry", "auto_update"]),
            make_session_event(device_id="d2", features=["telemetry"]),
            make_session_event(device_id="d3", features=["telemetry", "auto_update"]),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        rates = result["feature_adoption_rates"]
        assert rates["telemetry"] == 100.0
        assert rates["auto_update"] == pytest.approx(66.7, abs=0.1)

    def test_new_devices_per_day(self, tmp_path):
        """New devices per day tracks first-seen date for each device."""
        events = [
            make_session_event(device_id="d1", timestamp="2026-02-08T10:00:00Z"),
            make_session_event(device_id="d1", timestamp="2026-02-09T10:00:00Z"),
            make_session_event(device_id="d2", timestamp="2026-02-09T10:00:00Z"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        new_per_day = result["new_devices_per_day"]
        # d1 first seen on 2/8, d2 first seen on 2/9
        assert new_per_day["2026-02-08"] == 1
        assert new_per_day["2026-02-09"] == 1

    def test_host_arch_distribution(self, tmp_path):
        """Host architecture distribution is computed."""
        events = [
            make_session_event(device_id="d1", arch="aarch64"),
            make_session_event(device_id="d2", arch="x86_64"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        assert result["host_arch_distribution"]["aarch64"] == 1
        assert result["host_arch_distribution"]["x86_64"] == 1


# =============================================================================
# Test: Print Metrics
# =============================================================================


class TestPrintMetrics:
    """Tests for TelemetryAnalyzer.compute_print_metrics()."""

    def test_no_prints_returns_note(self):
        """Empty prints DataFrame returns a note."""
        analyzer = TelemetryAnalyzer()
        result = analyzer.compute_print_metrics()
        assert result == {"note": "No print data"}

    def test_outcome_counts_and_rates(self, tmp_path):
        """Outcome counts and percentage rates are computed correctly."""
        events = [
            make_print_event(device_id="d1", outcome="success"),
            make_print_event(device_id="d2", outcome="success"),
            make_print_event(device_id="d3", outcome="failure"),
            make_print_event(device_id="d4", outcome="cancelled"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        assert result["total_prints"] == 4
        assert result["outcome_counts"]["success"] == 2
        assert result["outcome_counts"]["failure"] == 1
        assert result["outcome_counts"]["cancelled"] == 1
        assert result["outcome_rates"]["success"] == 50.0
        assert result["outcome_rates"]["failure"] == 25.0

    def test_success_rate_by_model_requires_session_join(self, tmp_path):
        """Success rate by printer model joins print events with session data."""
        events = [
            # Sessions to provide model info
            make_session_event(device_id="d1", printer_model="Voron 2.4"),
            make_session_event(device_id="d2", printer_model="Voron 2.4"),
            make_session_event(device_id="d3", printer_model="Voron 2.4"),
            make_session_event(device_id="d4", printer_model="Voron 2.4"),
            make_session_event(device_id="d5", printer_model="Voron 2.4"),
            # 5 prints for Voron 2.4 (meeting min_count=5): 4 success, 1 failure
            make_print_event(device_id="d1", outcome="success"),
            make_print_event(device_id="d2", outcome="success"),
            make_print_event(device_id="d3", outcome="success"),
            make_print_event(device_id="d4", outcome="success"),
            make_print_event(device_id="d5", outcome="failure"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        by_model = result["success_rate_by_model"]
        assert "Voron 2.4" in by_model
        assert by_model["Voron 2.4"]["rate"] == 80.0
        assert by_model["Voron 2.4"]["total"] == 5

    def test_model_below_min_count_excluded(self, tmp_path):
        """Printer models with fewer than 5 prints are excluded from rate-by-model."""
        events = [
            make_session_event(device_id="d1", printer_model="RareBot"),
            make_print_event(device_id="d1", outcome="success"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        by_model = result.get("success_rate_by_model", {})
        assert "RareBot" not in by_model

    def test_phase_completion_distribution(self, tmp_path):
        """Phase completion counts how many prints reached each phase."""
        events = [
            make_print_event(device_id="d1", phases_completed=10),
            make_print_event(device_id="d2", phases_completed=10),
            make_print_event(device_id="d3", phases_completed=5),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        phases = result["phase_completion_distribution"]
        assert phases["10"] == 2
        assert phases["5"] == 1

    def test_filament_type_distribution(self, tmp_path):
        """Filament type distribution counts each type."""
        events = [
            make_print_event(device_id="d1", filament_type="PLA"),
            make_print_event(device_id="d2", filament_type="PLA"),
            make_print_event(device_id="d3", filament_type="PETG"),
            make_print_event(device_id="d4", filament_type="ABS"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        ft = result["filament_type_distribution"]
        assert ft["PLA"] == 2
        assert ft["PETG"] == 1
        assert ft["ABS"] == 1

    def test_average_duration_by_outcome(self, tmp_path):
        """Average print duration is grouped by outcome."""
        events = [
            make_print_event(device_id="d1", outcome="success", duration_sec=3600),
            make_print_event(device_id="d2", outcome="success", duration_sec=7200),
            make_print_event(device_id="d3", outcome="failure", duration_sec=600),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        avg = result["avg_duration_by_outcome_sec"]
        assert avg["success"] == pytest.approx(5400.0, abs=0.1)
        assert avg["failure"] == pytest.approx(600.0, abs=0.1)

    def test_nozzle_temp_distribution(self, tmp_path):
        """Nozzle temperatures are bucketed into ranges."""
        events = [
            make_print_event(device_id="d1", nozzle_temp=210),
            make_print_event(device_id="d2", nozzle_temp=250),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        nt = result["nozzle_temp_distribution"]
        assert nt["200-220C"] == 1
        assert nt["250-280C"] == 1

    def test_bed_temp_distribution(self, tmp_path):
        """Bed temperatures are bucketed into ranges."""
        events = [
            make_print_event(device_id="d1", bed_temp=60),
            make_print_event(device_id="d2", bed_temp=100),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        bt = result["bed_temp_distribution"]
        assert bt["60-80C"] == 1
        assert bt["100-120C"] == 1

    def test_success_rate_by_kinematics(self, tmp_path):
        """Success rate by kinematics is computed (min_count=1)."""
        events = [
            make_session_event(device_id="d1", kinematics="corexy"),
            make_session_event(device_id="d2", kinematics="cartesian"),
            make_print_event(device_id="d1", outcome="success"),
            make_print_event(device_id="d2", outcome="failure"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_print_metrics()

        by_kin = result["success_rate_by_kinematics"]
        assert by_kin["corexy"]["rate"] == 100.0
        assert by_kin["cartesian"]["rate"] == 0.0


# =============================================================================
# Test: Crash Metrics
# =============================================================================


class TestCrashMetrics:
    """Tests for TelemetryAnalyzer.compute_crash_metrics()."""

    def test_no_crashes_returns_note(self):
        """Empty crashes DataFrame returns a note."""
        analyzer = TelemetryAnalyzer()
        result = analyzer.compute_crash_metrics()
        assert result == {"note": "No crash data"}

    def test_crash_rate_relative_to_sessions(self, tmp_path):
        """Crash rate = total crashes / total sessions."""
        events = [
            make_session_event(device_id="d1"),
            make_session_event(device_id="d2"),
            make_session_event(device_id="d3"),
            make_session_event(device_id="d4"),
            make_crash_event(device_id="d1"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        assert result["total_crashes"] == 1
        assert result["total_sessions"] == 4
        assert result["crash_rate"] == 0.25

    def test_signal_distribution(self, tmp_path):
        """Crashes are grouped by signal name."""
        events = [
            make_session_event(device_id="d1"),
            make_crash_event(device_id="d1", signal_name="SIGSEGV"),
            make_crash_event(device_id="d1", signal_name="SIGSEGV"),
            make_crash_event(device_id="d1", signal_name="SIGABRT"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        by_signal = result["crashes_by_signal"]
        assert by_signal["SIGSEGV"] == 2
        assert by_signal["SIGABRT"] == 1

    def test_crash_rate_per_version(self, tmp_path):
        """Crash rate is computed per app version (crashes/sessions for that version)."""
        events = [
            make_session_event(device_id="d1", version="1.0.0"),
            make_session_event(device_id="d2", version="1.0.0"),
            make_session_event(device_id="d3", version="1.1.0"),
            make_crash_event(device_id="d1", app_version="1.0.0"),
            make_crash_event(device_id="d2", app_version="1.0.0"),
            make_crash_event(device_id="d3", app_version="1.1.0"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        rates = result["crash_rate_per_version"]
        assert rates["1.0.0"] == 1.0  # 2 crashes / 2 sessions
        assert rates["1.1.0"] == 1.0  # 1 crash / 1 session

    def test_uptime_bucketing(self, tmp_path):
        """Uptime before crash is bucketed into labeled ranges."""
        events = [
            make_session_event(device_id="d1"),
            make_crash_event(device_id="d1", uptime_sec=30),    # <1min
            make_crash_event(device_id="d1", uptime_sec=7200),  # 1-4hr
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        uptime_dist = result["uptime_distribution_before_crash"]
        assert uptime_dist["<1min"] == 1
        assert uptime_dist["1-4hr"] == 1

    def test_mean_and_median_uptime(self, tmp_path):
        """Mean and median uptime before crash are computed."""
        events = [
            make_session_event(device_id="d1"),
            make_crash_event(device_id="d1", uptime_sec=100),
            make_crash_event(device_id="d1", uptime_sec=200),
            make_crash_event(device_id="d1", uptime_sec=300),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        assert result["mean_uptime_before_crash_sec"] == 200.0
        assert result["median_uptime_before_crash_sec"] == 200.0

    def test_crashes_by_platform_via_session_join(self, tmp_path):
        """Crash platform is determined by joining with session data."""
        events = [
            make_session_event(device_id="d1", platform="linux-aarch64"),
            make_session_event(device_id="d2", platform="linux-x86_64"),
            make_crash_event(device_id="d1"),
            make_crash_event(device_id="d2"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        by_platform = result["crashes_by_platform"]
        assert by_platform["linux-aarch64"] == 1
        assert by_platform["linux-x86_64"] == 1

    def test_crashes_by_version(self, tmp_path):
        """Crashes are grouped by app_version field."""
        events = [
            make_session_event(device_id="d1"),
            make_crash_event(device_id="d1", app_version="1.0.0"),
            make_crash_event(device_id="d1", app_version="1.0.0"),
            make_crash_event(device_id="d1", app_version="1.1.0"),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        by_ver = result["crashes_by_version"]
        assert by_ver["1.0.0"] == 2
        assert by_ver["1.1.0"] == 1


# =============================================================================
# Test: Edge Cases
# =============================================================================


class TestEdgeCases:
    """Edge cases and boundary conditions."""

    def test_crash_with_no_sessions(self, tmp_path):
        """Crash metrics handle case where there are crashes but no sessions."""
        events = [make_crash_event()]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_crash_metrics()

        assert result["total_crashes"] == 1
        assert result["total_sessions"] == 0
        assert result["crash_rate"] is None

    def test_compute_all_returns_all_sections(self, tmp_path):
        """compute_all() returns adoption, print_reliability, and crash_analysis."""
        events = [
            make_session_event(),
            make_print_event(),
            make_crash_event(),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_all()

        assert "generated_at" in result
        assert "event_counts" in result
        assert "adoption" in result
        assert "print_reliability" in result
        assert "crash_analysis" in result
        assert result["event_counts"]["sessions"] == 1
        assert result["event_counts"]["prints"] == 1
        assert result["event_counts"]["crashes"] == 1
        assert result["event_counts"]["total"] == 3

    def test_empty_features_list(self, tmp_path):
        """Sessions with empty features list produce 0% adoption rates."""
        events = [make_session_event(features=[])]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        result = analyzer.compute_adoption_metrics()

        # Empty list means total_with_features=0 (empty list IS a list, so it counts)
        # Actually [] is a list, so total_with_features=1, but no features counted
        assert result["feature_adoption_rates"] == {}



# =============================================================================
# Test: Output Formatters
# =============================================================================


class TestOutputFormatters:
    """Tests for format_terminal, format_json, format_html."""

    @pytest.fixture
    def known_dataset(self, tmp_path):
        """1 session, 2 prints (one success, one failed), 1 crash.

        Every figure the formatter tests assert is derived from THIS data, so a
        formatter that emits each section heading with zero rows under it fails
        rather than passing on its own hardcoded chrome.
        """
        events = [
            make_session_event(),
            make_print_event(outcome="success"),
            make_print_event(outcome="failed", timestamp="2026-02-09T13:00:00Z"),
            make_crash_event(),
        ]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        return analyzer

    def test_format_terminal_renders_computed_metrics(self, known_dataset):
        """Terminal output carries the computed figures, not just the headings."""
        output = known_dataset.format_terminal(known_dataset.compute_all())

        assert "HELIXSCREEN TELEMETRY REPORT" in output
        assert "ADOPTION METRICS" in output
        assert "PRINT RELIABILITY" in output
        assert "CRASH ANALYSIS" in output

        # The computed half: 1 of 2 prints succeeded, 1 crash over 1 session.
        assert "  Total unique devices: 1" in output
        assert "  Total prints: 2" in output
        assert "    success: 50.0% (1)" in output
        assert "    failed: 50.0% (1)" in output
        assert "  Total crashes: 1" in output
        assert "  Crash rate: 100.00%" in output

    def test_format_json_is_valid_json(self, tmp_path):
        """JSON output is parseable."""
        events = [make_session_event(), make_print_event()]
        write_event_file(tmp_path, events)

        analyzer = TelemetryAnalyzer()
        analyzer.load_events(str(tmp_path))
        metrics = analyzer.compute_all()
        json_str = analyzer.format_json(metrics)

        parsed = json.loads(json_str)
        assert "adoption" in parsed
        assert "print_reliability" in parsed

    def test_format_html_embeds_computed_metrics(self, known_dataset, tmp_path):
        """HTML carries the same computed figures in both of its tabs."""
        html_path = str(tmp_path / "report.html")
        known_dataset.format_html(known_dataset.compute_all(), html_path)

        content = Path(html_path).read_text()
        assert "<!DOCTYPE html>" in content
        assert "HelixScreen Telemetry Report" in content
        assert "tab-summary" in content
        assert "tab-json" in content

        # The summary tab embeds the terminal report...
        assert "    success: 50.0% (1)" in content
        # ...and the JSON tab carries the same figure as data.
        json_payload = content.split('<div id="tab-json"', 1)[1]
        assert '"total_prints": 2' in json_payload
        assert '"success": 50.0' in json_payload

    def test_fmt_duration_seconds(self):
        """Duration formatter handles seconds."""
        assert TelemetryAnalyzer._fmt_duration(45) == "45s"

    def test_fmt_duration_minutes(self):
        """Duration formatter handles minutes."""
        assert TelemetryAnalyzer._fmt_duration(120) == "2.0min"

    def test_fmt_duration_hours(self):
        """Duration formatter handles hours."""
        assert TelemetryAnalyzer._fmt_duration(7200) == "2.0hr"
