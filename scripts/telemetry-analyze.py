#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
HelixScreen Telemetry Analytics

Reads telemetry event JSON files, computes adoption, print reliability,
and crash metrics. Outputs as terminal text, JSON, or self-contained HTML.

Usage:
    telemetry-analyze.py                  # Terminal summary
    telemetry-analyze.py --json           # JSON output
    telemetry-analyze.py --html report.html  # HTML report
    telemetry-analyze.py --since 2026-01-01 --until 2026-02-10
    telemetry-analyze.py --data-dir /path/to/events
"""

import argparse
import json
import sys
import warnings
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

import pandas as pd

# Suppress pandas timezone-to-period conversion warnings (expected behavior)
warnings.filterwarnings("ignore", message="Converting to PeriodArray")


def bucket_value(value, buckets: list[tuple[float, float, str]]) -> str:
    """Place a value into a labeled bucket."""
    if value is None or pd.isna(value):
        return "unknown"
    for low, high, label in buckets:
        if low <= value < high:
            return label
    return "unknown"


RAM_BUCKETS = [
    (0, 512, "<512MB"),
    (512, 1024, "512MB-1GB"),
    (1024, 2048, "1-2GB"),
    (2048, 4096, "2-4GB"),
    (4096, 8192, "4-8GB"),
    (8192, float("inf"), "8GB+"),
]

NOZZLE_TEMP_BUCKETS = [
    (0, 180, "<180C"),
    (180, 200, "180-200C"),
    (200, 220, "200-220C"),
    (220, 250, "220-250C"),
    (250, 280, "250-280C"),
    (280, 310, "280-310C"),
    (310, float("inf"), "310C+"),
]

BED_TEMP_BUCKETS = [
    (0, 40, "<40C"),
    (40, 60, "40-60C"),
    (60, 80, "60-80C"),
    (80, 100, "80-100C"),
    (100, 120, "100-120C"),
    (120, float("inf"), "120C+"),
]

UPTIME_BUCKETS = [
    (0, 60, "<1min"),
    (60, 300, "1-5min"),
    (300, 900, "5-15min"),
    (900, 3600, "15min-1hr"),
    (3600, 14400, "1-4hr"),
    (14400, float("inf"), "4hr+"),
]


class TelemetryAnalyzer:
    """Loads telemetry events and computes analytics."""

    def __init__(self):
        self.sessions: pd.DataFrame = pd.DataFrame()
        self.prints: pd.DataFrame = pd.DataFrame()
        self.crashes: pd.DataFrame = pd.DataFrame()
        self.update_failures: pd.DataFrame = pd.DataFrame()
        self.update_successes: pd.DataFrame = pd.DataFrame()
        self.memory_snapshots: pd.DataFrame = pd.DataFrame()
        self.hardware_profiles: pd.DataFrame = pd.DataFrame()
        self.settings_snapshots: pd.DataFrame = pd.DataFrame()
        self.panel_usage: pd.DataFrame = pd.DataFrame()
        self.connection_stability: pd.DataFrame = pd.DataFrame()
        self.print_starts: pd.DataFrame = pd.DataFrame()
        self.errors: pd.DataFrame = pd.DataFrame()
        self.all_events: pd.DataFrame = pd.DataFrame()

    def load_events(
        self,
        data_dir: str,
        since: Optional[str] = None,
        until: Optional[str] = None,
    ) -> None:
        """Load all JSON event files from data_dir (recursively) and split by type."""
        data_path = Path(data_dir)
        if not data_path.exists():
            print(f"Data directory not found: {data_path}", file=sys.stderr)
            return

        all_events: list[dict] = []
        json_files = sorted(data_path.rglob("*.json"))
        if not json_files:
            print(f"No JSON files found in {data_path}", file=sys.stderr)
            return

        for fpath in json_files:
            try:
                with open(fpath, "r") as f:
                    data = json.load(f)
                if isinstance(data, list):
                    all_events.extend(data)
                elif isinstance(data, dict):
                    all_events.append(data)
            except (json.JSONDecodeError, OSError) as e:
                print(f"Warning: skipping {fpath}: {e}", file=sys.stderr)

        if not all_events:
            print("No events loaded.", file=sys.stderr)
            return

        print(
            f"Loaded {len(all_events)} events from {len(json_files)} files",
            file=sys.stderr,
        )

        # Flatten nested fields for session events
        flat_events = []
        for ev in all_events:
            flat = {
                "event": ev.get("event"),
                "device_id": ev.get("device_id"),
                "timestamp": ev.get("timestamp"),
                "schema_version": ev.get("schema_version"),
            }

            if ev.get("event") == "session":
                for prefix in ("app", "host", "printer"):
                    sub = ev.get(prefix, {})
                    if isinstance(sub, dict):
                        for k, v in sub.items():
                            flat[f"{prefix}.{k}"] = v
                flat["features"] = ev.get("features", [])

            elif ev.get("event") == "print_outcome":
                for k in (
                    "outcome",
                    "duration_sec",
                    "phases_completed",
                    "filament_used_mm",
                    "filament_type",
                    "nozzle_temp",
                    "bed_temp",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "crash":
                for k in (
                    "signal",
                    "signal_name",
                    "app_version",
                    "uptime_sec",
                    "backtrace",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "update_failed":
                for k in (
                    "reason",
                    "version",
                    "from_version",
                    "platform",
                    "http_code",
                    "file_size",
                    "exit_code",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "update_success":
                for k in (
                    "version",
                    "from_version",
                    "platform",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "memory_snapshot":
                for k in (
                    "trigger",
                    "uptime_sec",
                    "rss_kb",
                    "vm_size_kb",
                    "vm_data_kb",
                    "vm_swap_kb",
                    "vm_peak_kb",
                    "vm_hwm_kb",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "hardware_profile":
                # Flatten nested sections with dot notation
                for section in (
                    "printer",
                    "mcus",
                    "build_volume",
                    "extruders",
                    "fans",
                    "steppers",
                    "leds",
                    "sensors",
                    "probe",
                    "capabilities",
                    "ams",
                    "tools",
                    "macros",
                    "plugins",
                ):
                    sub = ev.get(section, {})
                    if isinstance(sub, dict):
                        for k, v in sub.items():
                            flat[f"{section}.{k}"] = v
                flat["display_backend"] = ev.get("display_backend")

            elif ev.get("event") == "settings_snapshot":
                for k in (
                    "theme",
                    "brightness_pct",
                    "screensaver_timeout_sec",
                    "screen_blank_timeout_sec",
                    "locale",
                    "sound_enabled",
                    "auto_update_channel",
                    "animations_enabled",
                    "time_format",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "panel_usage":
                flat["session_duration_sec"] = ev.get(
                    "session_duration_sec"
                )
                flat["overlay_open_count"] = ev.get("overlay_open_count")
                for section in ("panel_time_sec", "panel_visits"):
                    sub = ev.get(section, {})
                    if isinstance(sub, dict):
                        for k, v in sub.items():
                            flat[f"{section}.{k}"] = v

            elif ev.get("event") == "connection_stability":
                for k in (
                    "session_duration_sec",
                    "connect_count",
                    "disconnect_count",
                    "total_connected_sec",
                    "total_disconnected_sec",
                    "longest_disconnect_sec",
                    "klippy_error_count",
                    "klippy_shutdown_count",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "print_start_context":
                for k in (
                    "source",
                    "has_thumbnail",
                    "file_size_bucket",
                    "estimated_duration_bucket",
                    "slicer",
                    "tool_count_used",
                    "ams_active",
                ):
                    flat[k] = ev.get(k)

            elif ev.get("event") == "error_encountered":
                for k in ("category", "code", "context", "uptime_sec"):
                    flat[k] = ev.get(k)

            flat_events.append(flat)

        df = pd.DataFrame(flat_events)

        # Parse timestamps
        if "timestamp" in df.columns:
            df["timestamp"] = pd.to_datetime(
                df["timestamp"], errors="coerce", utc=True
            )

            if since:
                since_dt = pd.to_datetime(since, utc=True)
                df = df[df["timestamp"] >= since_dt]
            if until:
                until_dt = pd.to_datetime(until, utc=True) + pd.Timedelta(
                    days=1
                )
                df = df[df["timestamp"] < until_dt]

        self.all_events = df
        self.sessions = df[df["event"] == "session"].copy()
        self.prints = df[df["event"] == "print_outcome"].copy()
        self.crashes = df[df["event"] == "crash"].copy()
        self.update_failures = df[df["event"] == "update_failed"].copy()
        self.update_successes = df[df["event"] == "update_success"].copy()
        self.memory_snapshots = df[df["event"] == "memory_snapshot"].copy()
        self.hardware_profiles = df[
            df["event"] == "hardware_profile"
        ].copy()
        self.settings_snapshots = df[
            df["event"] == "settings_snapshot"
        ].copy()
        self.panel_usage = df[df["event"] == "panel_usage"].copy()
        self.connection_stability = df[
            df["event"] == "connection_stability"
        ].copy()
        self.print_starts = df[
            df["event"] == "print_start_context"
        ].copy()
        self.errors = df[df["event"] == "error_encountered"].copy()

    # -- Adoption Metrics --------------------------------------------------

    def compute_adoption_metrics(self) -> dict:
        result: dict[str, Any] = {}
        s = self.sessions

        if s.empty:
            return {"note": "No session data"}

        result["total_unique_devices"] = s["device_id"].nunique()

        # Active devices by period
        if "timestamp" in s.columns and s["timestamp"].notna().any():
            ts = s.dropna(subset=["timestamp"])
            result["active_devices_daily"] = {
                str(k): v
                for k, v in ts.groupby(ts["timestamp"].dt.date)["device_id"]
                .nunique()
                .to_dict()
                .items()
            }
            result["active_devices_weekly"] = {
                str(k): v
                for k, v in ts.groupby(ts["timestamp"].dt.to_period("W"))[
                    "device_id"
                ]
                .nunique()
                .to_dict()
                .items()
            }
            result["active_devices_monthly"] = {
                str(k): v
                for k, v in ts.groupby(ts["timestamp"].dt.to_period("M"))[
                    "device_id"
                ]
                .nunique()
                .to_dict()
                .items()
            }

            # New devices per period (first seen date)
            first_seen = ts.groupby("device_id")["timestamp"].min().dt.date
            new_per_day = first_seen.value_counts().sort_index()
            result["new_devices_per_day"] = {
                str(k): int(v) for k, v in new_per_day.items()
            }

        # Distributions
        result["platform_distribution"] = self._distribution(s, "app.platform")
        result["app_version_distribution"] = self._distribution(
            s, "app.version"
        )
        result["printer_model_top20"] = self._distribution(
            s, "printer.detected_model", top=20
        )
        result["kinematics_distribution"] = self._distribution(
            s, "printer.kinematics"
        )
        result["display_resolution_distribution"] = self._distribution(
            s, "app.display"
        )
        result["locale_distribution"] = self._distribution(s, "app.locale")
        result["theme_distribution"] = self._distribution(s, "app.theme")
        result["klipper_version_distribution"] = self._distribution(
            s, "printer.klipper_version"
        )
        result["host_arch_distribution"] = self._distribution(s, "host.arch")

        # RAM distribution (bucketed)
        if "host.ram_total_mb" in s.columns:
            ram_vals = pd.to_numeric(s["host.ram_total_mb"], errors="coerce")
            ram_bucketed = ram_vals.apply(
                lambda v: bucket_value(v, RAM_BUCKETS)
            )
            result["ram_distribution"] = ram_bucketed.value_counts().to_dict()

        # Feature adoption rates
        if "features" in s.columns:
            feature_counts: dict[str, int] = {}
            total_with_features = 0
            for feat_list in s["features"]:
                if isinstance(feat_list, list):
                    total_with_features += 1
                    for f in feat_list:
                        feature_counts[f] = feature_counts.get(f, 0) + 1
            if total_with_features > 0:
                result["feature_adoption_rates"] = {
                    k: round(v / total_with_features * 100, 1)
                    for k, v in sorted(
                        feature_counts.items(), key=lambda x: -x[1]
                    )
                }
            else:
                result["feature_adoption_rates"] = {}

        return result

    # -- Print Reliability -------------------------------------------------

    def compute_print_metrics(self) -> dict:
        result: dict[str, Any] = {}
        p = self.prints

        if p.empty:
            return {"note": "No print data"}

        total = len(p)

        # Overall rates
        if "outcome" in p.columns:
            outcome_counts = p["outcome"].value_counts().to_dict()
            result["total_prints"] = total
            result["outcome_counts"] = outcome_counts
            result["outcome_rates"] = {
                k: round(v / total * 100, 1)
                for k, v in outcome_counts.items()
            }

            # Success rate over time (weekly)
            if "timestamp" in p.columns and p["timestamp"].notna().any():
                ts = p.dropna(subset=["timestamp"])
                weekly = ts.groupby(ts["timestamp"].dt.to_period("W"))
                success_weekly = {}
                for period, group in weekly:
                    n = len(group)
                    successes = (group["outcome"] == "success").sum()
                    success_weekly[str(period)] = round(
                        successes / n * 100, 1
                    )
                result["success_rate_weekly"] = success_weekly

            # Success rate by printer model (min 5 prints)
            # Join with session data to get printer model
            if not self.sessions.empty:
                p_with_model = self._join_session_field(
                    p, "printer.detected_model"
                )
                result["success_rate_by_model"] = self._success_rate_by(
                    p_with_model, "printer.detected_model", min_count=5
                )

                p_with_kin = self._join_session_field(
                    p, "printer.kinematics"
                )
                result["success_rate_by_kinematics"] = self._success_rate_by(
                    p_with_kin, "printer.kinematics", min_count=1
                )

        # Phase completion distribution
        if "phases_completed" in p.columns:
            phases = pd.to_numeric(
                p["phases_completed"], errors="coerce"
            ).dropna()
            if not phases.empty:
                result["phase_completion_distribution"] = {
                    str(k): v
                    for k, v in phases.astype(int)
                    .value_counts()
                    .sort_index()
                    .to_dict()
                    .items()
                }

        # Average duration by outcome
        if "duration_sec" in p.columns and "outcome" in p.columns:
            durations = p.copy()
            durations["duration_sec"] = pd.to_numeric(
                durations["duration_sec"], errors="coerce"
            )
            avg_dur = (
                durations.groupby("outcome")["duration_sec"]
                .mean()
                .round(1)
                .to_dict()
            )
            result["avg_duration_by_outcome_sec"] = avg_dur

        # Filament type popularity
        if "filament_type" in p.columns:
            ft = p["filament_type"].dropna()
            ft = ft[ft != ""]  # Filter empty strings
            if not ft.empty:
                result["filament_type_distribution"] = (
                    ft.value_counts().to_dict()
                )

        # Temperature distributions (bucketed)
        if "nozzle_temp" in p.columns:
            nt = pd.to_numeric(p["nozzle_temp"], errors="coerce")
            nt = nt[nt > 0]  # Filter zero/unset temps
            if not nt.empty:
                bucketed = nt.apply(
                    lambda v: bucket_value(v, NOZZLE_TEMP_BUCKETS)
                )
                result["nozzle_temp_distribution"] = (
                    bucketed.value_counts().to_dict()
                )

        if "bed_temp" in p.columns:
            bt = pd.to_numeric(p["bed_temp"], errors="coerce")
            bt = bt[bt > 0]
            if not bt.empty:
                bucketed = bt.apply(
                    lambda v: bucket_value(v, BED_TEMP_BUCKETS)
                )
                result["bed_temp_distribution"] = (
                    bucketed.value_counts().to_dict()
                )

        return result

    # -- Crash Analysis ----------------------------------------------------

    def compute_crash_metrics(self) -> dict:
        result: dict[str, Any] = {}
        c = self.crashes

        if c.empty:
            return {"note": "No crash data"}

        total_crashes = len(c)
        total_sessions = len(self.sessions) if not self.sessions.empty else 0

        result["total_crashes"] = total_crashes
        result["total_sessions"] = total_sessions
        result["crash_rate"] = (
            round(total_crashes / total_sessions, 4)
            if total_sessions > 0
            else None
        )

        # Crashes by signal type
        if "signal_name" in c.columns:
            result["crashes_by_signal"] = (
                c["signal_name"].value_counts().to_dict()
            )

        # Crashes by platform - join with session data
        if (
            not self.sessions.empty
            and "app.platform" in self.sessions.columns
        ):
            device_platform = self.sessions.groupby("device_id")[
                "app.platform"
            ].first()
            c_with_platform = c.copy()
            c_with_platform["platform"] = c_with_platform["device_id"].map(
                device_platform
            )
            result["crashes_by_platform"] = (
                c_with_platform["platform"]
                .fillna("unknown")
                .value_counts()
                .to_dict()
            )

        # Crashes by app version
        if "app_version" in c.columns:
            result["crashes_by_version"] = (
                c["app_version"].value_counts().to_dict()
            )

            # Crash rate per version
            if (
                not self.sessions.empty
                and "app.version" in self.sessions.columns
            ):
                sessions_by_ver = (
                    self.sessions["app.version"].value_counts().to_dict()
                )
                crash_rate_by_ver = {}
                for ver, crash_count in (
                    c["app_version"].value_counts().items()
                ):
                    sess_count = sessions_by_ver.get(ver, 0)
                    if sess_count > 0:
                        crash_rate_by_ver[ver] = round(
                            crash_count / sess_count, 4
                        )
                    else:
                        crash_rate_by_ver[ver] = None
                result["crash_rate_per_version"] = crash_rate_by_ver

        # Uptime before crash
        if "uptime_sec" in c.columns:
            uptime = pd.to_numeric(c["uptime_sec"], errors="coerce").dropna()
            if not uptime.empty:
                result["mean_uptime_before_crash_sec"] = round(
                    uptime.mean(), 1
                )
                result["median_uptime_before_crash_sec"] = round(
                    uptime.median(), 1
                )
                bucketed = uptime.apply(
                    lambda v: bucket_value(v, UPTIME_BUCKETS)
                )
                result["uptime_distribution_before_crash"] = (
                    bucketed.value_counts().to_dict()
                )

        return result

    # -- Update Analysis ---------------------------------------------------

    def compute_update_metrics(self) -> dict:
        result: dict[str, Any] = {}
        n_fail = len(self.update_failures)
        n_success = len(self.update_successes)
        n_total = n_fail + n_success

        if n_total == 0:
            return {"note": "No update data"}

        rate = n_success / n_total * 100 if n_total > 0 else 0
        result["total_attempts"] = n_total
        result["successes"] = n_success
        result["failures"] = n_fail
        result["success_rate"] = round(rate, 1)

        if n_fail > 0:
            if "reason" in self.update_failures.columns:
                result["failure_reasons"] = (
                    self.update_failures["reason"]
                    .dropna()
                    .value_counts()
                    .to_dict()
                )
            if "platform" in self.update_failures.columns:
                result["failures_by_platform"] = (
                    self.update_failures["platform"]
                    .dropna()
                    .value_counts()
                    .to_dict()
                )

        return result

    # -- Memory Analysis ---------------------------------------------------

    def compute_memory_metrics(self) -> dict:
        result: dict[str, Any] = {}
        m = self.memory_snapshots
        if m.empty:
            return {"note": "No memory snapshot data"}
        result["total_snapshots"] = len(m)
        for col in ("rss_kb", "vm_size_kb", "vm_peak_kb", "vm_hwm_kb"):
            if col in m.columns:
                vals = pd.to_numeric(m[col], errors="coerce").dropna()
                if not vals.empty:
                    result[f"{col}_mean"] = round(vals.mean(), 1)
                    result[f"{col}_max"] = round(vals.max(), 1)
                    result[f"{col}_p95"] = round(vals.quantile(0.95), 1)
        return result

    # -- Hardware Analysis -------------------------------------------------

    def compute_hardware_metrics(self) -> dict:
        result: dict[str, Any] = {}
        h = self.hardware_profiles
        if h.empty:
            return {"note": "No hardware profile data"}
        result["total_profiles"] = len(h)
        result["printer_model_distribution"] = self._distribution(
            h, "printer.detected_model", top=20
        )
        result["kinematics_distribution"] = self._distribution(
            h, "printer.kinematics"
        )
        result["primary_mcu_distribution"] = self._distribution(
            h, "mcus.primary", top=10
        )
        result["extruder_count_distribution"] = self._distribution(
            h, "extruders.count"
        )
        # Bool capability adoption rates
        for cap in (
            "capabilities.has_chamber",
            "capabilities.has_accelerometer",
            "capabilities.has_firmware_retraction",
            "capabilities.has_exclude_object",
            "capabilities.has_timelapse",
            "capabilities.has_klippain_shaketune",
            "probe.has_probe",
            "probe.has_bed_mesh",
            "probe.has_qgl",
        ):
            if cap in h.columns:
                vals = h[cap].dropna()
                true_count = (
                    vals.sum()
                    if vals.dtype == bool
                    else (vals == True).sum()  # noqa: E712
                )
                result[f"{cap}_pct"] = (
                    round(true_count / len(vals) * 100, 1)
                    if len(vals) > 0
                    else 0
                )
        result["ams_type_distribution"] = self._distribution(
            h, "ams.type"
        )
        result["display_backend_distribution"] = self._distribution(
            h, "display_backend"
        )
        return result

    # -- Settings Analysis -------------------------------------------------

    def compute_settings_metrics(self) -> dict:
        result: dict[str, Any] = {}
        s = self.settings_snapshots
        if s.empty:
            return {"note": "No settings snapshot data"}
        result["total_snapshots"] = len(s)
        result["theme_distribution"] = self._distribution(s, "theme")
        result["locale_distribution"] = self._distribution(s, "locale")
        if "brightness_pct" in s.columns:
            vals = pd.to_numeric(
                s["brightness_pct"], errors="coerce"
            ).dropna()
            if not vals.empty:
                result["brightness_mean"] = round(vals.mean(), 1)
        result["time_format_distribution"] = self._distribution(
            s, "time_format"
        )
        return result

    # -- Panel Usage -------------------------------------------------------

    def compute_panel_usage_metrics(self) -> dict:
        result: dict[str, Any] = {}
        p = self.panel_usage
        if p.empty:
            return {"note": "No panel usage data"}
        result["total_sessions"] = len(p)
        # Aggregate panel time across sessions
        time_cols = [
            c for c in p.columns if c.startswith("panel_time_sec.")
        ]
        if time_cols:
            panel_totals = {}
            for col in time_cols:
                panel_name = col.replace("panel_time_sec.", "")
                vals = pd.to_numeric(p[col], errors="coerce").fillna(0)
                panel_totals[panel_name] = int(vals.sum())
            result["total_time_by_panel_sec"] = dict(
                sorted(panel_totals.items(), key=lambda x: -x[1])
            )
        visit_cols = [
            c for c in p.columns if c.startswith("panel_visits.")
        ]
        if visit_cols:
            panel_visits = {}
            for col in visit_cols:
                panel_name = col.replace("panel_visits.", "")
                vals = pd.to_numeric(p[col], errors="coerce").fillna(0)
                panel_visits[panel_name] = int(vals.sum())
            result["total_visits_by_panel"] = dict(
                sorted(panel_visits.items(), key=lambda x: -x[1])
            )
        if "session_duration_sec" in p.columns:
            dur = pd.to_numeric(
                p["session_duration_sec"], errors="coerce"
            ).dropna()
            if not dur.empty:
                result["avg_session_duration_sec"] = round(dur.mean(), 1)
                result["median_session_duration_sec"] = round(
                    dur.median(), 1
                )
        return result

    # -- Connection Stability ----------------------------------------------

    def compute_connection_metrics(self) -> dict:
        result: dict[str, Any] = {}
        c = self.connection_stability
        if c.empty:
            return {"note": "No connection stability data"}
        result["total_sessions"] = len(c)
        for col in (
            "connect_count",
            "disconnect_count",
            "klippy_error_count",
            "klippy_shutdown_count",
        ):
            if col in c.columns:
                vals = pd.to_numeric(c[col], errors="coerce").dropna()
                if not vals.empty:
                    result[f"{col}_total"] = int(vals.sum())
                    result[f"{col}_mean"] = round(vals.mean(), 2)
        if (
            "total_connected_sec" in c.columns
            and "session_duration_sec" in c.columns
        ):
            connected = pd.to_numeric(
                c["total_connected_sec"], errors="coerce"
            ).fillna(0)
            duration = pd.to_numeric(
                c["session_duration_sec"], errors="coerce"
            ).fillna(0)
            total_dur = duration.sum()
            if total_dur > 0:
                result["overall_connected_pct"] = round(
                    connected.sum() / total_dur * 100, 1
                )
        if "longest_disconnect_sec" in c.columns:
            vals = pd.to_numeric(
                c["longest_disconnect_sec"], errors="coerce"
            ).dropna()
            if not vals.empty:
                result["longest_disconnect_max_sec"] = round(
                    vals.max(), 1
                )
                result["longest_disconnect_mean_sec"] = round(
                    vals.mean(), 1
                )
        return result

    # -- Print Start Context -----------------------------------------------

    def compute_print_start_metrics(self) -> dict:
        result: dict[str, Any] = {}
        p = self.print_starts
        if p.empty:
            return {"note": "No print start data"}
        result["total_print_starts"] = len(p)
        result["source_distribution"] = self._distribution(p, "source")
        result["slicer_distribution"] = self._distribution(
            p, "slicer", top=10
        )
        result["file_size_distribution"] = self._distribution(
            p, "file_size_bucket"
        )
        result["duration_estimate_distribution"] = self._distribution(
            p, "estimated_duration_bucket"
        )
        if "has_thumbnail" in p.columns:
            vals = p["has_thumbnail"].dropna()
            true_count = (
                vals.sum()
                if vals.dtype == bool
                else (vals == True).sum()  # noqa: E712
            )
            result["thumbnail_pct"] = (
                round(true_count / len(vals) * 100, 1)
                if len(vals) > 0
                else 0
            )
        if "ams_active" in p.columns:
            vals = p["ams_active"].dropna()
            true_count = (
                vals.sum()
                if vals.dtype == bool
                else (vals == True).sum()  # noqa: E712
            )
            result["ams_active_pct"] = (
                round(true_count / len(vals) * 100, 1)
                if len(vals) > 0
                else 0
            )
        return result

    # -- Error Analysis ----------------------------------------------------

    def compute_error_metrics(self) -> dict:
        result: dict[str, Any] = {}
        e = self.errors
        if e.empty:
            return {"note": "No error data"}
        result["total_errors"] = len(e)
        result["errors_by_category"] = self._distribution(e, "category")
        result["errors_by_code"] = self._distribution(e, "code", top=10)
        result["errors_by_context"] = self._distribution(
            e, "context", top=10
        )
        return result

    # -- Aggregate ---------------------------------------------------------

    def compute_all(self) -> dict:
        return {
            "generated_at": datetime.now(tz=timezone.utc).isoformat(),
            "event_counts": {
                "sessions": len(self.sessions),
                "prints": len(self.prints),
                "crashes": len(self.crashes),
                "update_failures": len(self.update_failures),
                "update_successes": len(self.update_successes),
                "memory_snapshots": len(self.memory_snapshots),
                "hardware_profiles": len(self.hardware_profiles),
                "settings_snapshots": len(self.settings_snapshots),
                "panel_usage": len(self.panel_usage),
                "connection_stability": len(self.connection_stability),
                "print_starts": len(self.print_starts),
                "errors": len(self.errors),
                "total": len(self.all_events),
            },
            "adoption": self.compute_adoption_metrics(),
            "print_reliability": self.compute_print_metrics(),
            "crash_analysis": self.compute_crash_metrics(),
            "update_analysis": self.compute_update_metrics(),
            "memory_analysis": self.compute_memory_metrics(),
            "hardware_analysis": self.compute_hardware_metrics(),
            "settings_analysis": self.compute_settings_metrics(),
            "panel_usage_analysis": self.compute_panel_usage_metrics(),
            "connection_analysis": self.compute_connection_metrics(),
            "print_start_analysis": self.compute_print_start_metrics(),
            "error_analysis": self.compute_error_metrics(),
        }

    # -- Output Formatters -------------------------------------------------

    def format_terminal(self, metrics: dict) -> str:
        lines: list[str] = []
        sep = "=" * 60

        lines.append(sep)
        lines.append("  HELIXSCREEN TELEMETRY REPORT")
        lines.append(f"  Generated: {metrics.get('generated_at', 'N/A')}")
        lines.append(sep)

        ec = metrics.get("event_counts", {})
        lines.append(
            f"\n  Events loaded: {ec.get('total', 0)} "
            f"(sessions={ec.get('sessions', 0)}, "
            f"prints={ec.get('prints', 0)}, "
            f"crashes={ec.get('crashes', 0)}, "
            f"updates={ec.get('update_successes', 0) + ec.get('update_failures', 0)}, "
            f"memory={ec.get('memory_snapshots', 0)}, "
            f"hardware={ec.get('hardware_profiles', 0)}, "
            f"settings={ec.get('settings_snapshots', 0)}, "
            f"panel_usage={ec.get('panel_usage', 0)}, "
            f"connection={ec.get('connection_stability', 0)}, "
            f"print_starts={ec.get('print_starts', 0)}, "
            f"errors={ec.get('errors', 0)})"
        )

        # Adoption
        lines.append(f"\n{sep}")
        lines.append("  ADOPTION METRICS")
        lines.append(sep)
        adoption = metrics.get("adoption", {})
        if adoption.get("note"):
            lines.append(f"  {adoption['note']}")
        else:
            lines.append(
                f"  Total unique devices: "
                f"{adoption.get('total_unique_devices', 0)}"
            )
            self._fmt_distribution(
                lines, "Platform", adoption.get("platform_distribution")
            )
            self._fmt_distribution(
                lines, "App version", adoption.get("app_version_distribution")
            )
            self._fmt_distribution(
                lines,
                "Printer model (top 20)",
                adoption.get("printer_model_top20"),
            )
            self._fmt_distribution(
                lines, "Kinematics", adoption.get("kinematics_distribution")
            )
            self._fmt_distribution(
                lines,
                "Display",
                adoption.get("display_resolution_distribution"),
            )
            self._fmt_distribution(
                lines, "Locale", adoption.get("locale_distribution")
            )
            self._fmt_distribution(
                lines, "Theme", adoption.get("theme_distribution")
            )
            self._fmt_distribution(
                lines,
                "Klipper version",
                adoption.get("klipper_version_distribution"),
            )
            self._fmt_distribution(
                lines, "Host arch", adoption.get("host_arch_distribution")
            )
            self._fmt_distribution(
                lines, "RAM", adoption.get("ram_distribution")
            )
            self._fmt_percentages(
                lines,
                "Feature adoption (%)",
                adoption.get("feature_adoption_rates"),
            )

        # Print reliability
        lines.append(f"\n{sep}")
        lines.append("  PRINT RELIABILITY")
        lines.append(sep)
        pr = metrics.get("print_reliability", {})
        if pr.get("note"):
            lines.append(f"  {pr['note']}")
        else:
            lines.append(f"  Total prints: {pr.get('total_prints', 0)}")
            rates = pr.get("outcome_rates", {})
            for outcome, rate in rates.items():
                count = pr.get("outcome_counts", {}).get(outcome, 0)
                lines.append(f"    {outcome}: {rate}% ({count})")

            self._fmt_distribution(
                lines, "Filament type", pr.get("filament_type_distribution")
            )

            avg_dur = pr.get("avg_duration_by_outcome_sec", {})
            if avg_dur:
                lines.append("\n  Avg duration by outcome:")
                for outcome, dur in avg_dur.items():
                    lines.append(
                        f"    {outcome}: {self._fmt_duration(dur)}"
                    )

            self._fmt_distribution(
                lines,
                "Print start phases completed",
                pr.get("phase_completion_distribution"),
            )
            self._fmt_distribution(
                lines, "Nozzle temp", pr.get("nozzle_temp_distribution")
            )
            self._fmt_distribution(
                lines, "Bed temp", pr.get("bed_temp_distribution")
            )

            sr_weekly = pr.get("success_rate_weekly", {})
            if sr_weekly:
                lines.append("\n  Success rate (weekly):")
                for period, rate in sr_weekly.items():
                    lines.append(f"    {period}: {rate}%")

            sr_model = pr.get("success_rate_by_model", {})
            if sr_model:
                lines.append(
                    "\n  Success rate by printer model (min 5 prints):"
                )
                for model, info in sr_model.items():
                    lines.append(
                        f"    {model}: {info['rate']}% "
                        f"({info['total']} prints)"
                    )

            sr_kin = pr.get("success_rate_by_kinematics", {})
            if sr_kin:
                lines.append("\n  Success rate by kinematics:")
                for kin, info in sr_kin.items():
                    lines.append(
                        f"    {kin}: {info['rate']}% ({info['total']} prints)"
                    )

        # Crash analysis
        lines.append(f"\n{sep}")
        lines.append("  CRASH ANALYSIS")
        lines.append(sep)
        ca = metrics.get("crash_analysis", {})
        if ca.get("note"):
            lines.append(f"  {ca['note']}")
        else:
            lines.append(
                f"  Total crashes: {ca.get('total_crashes', 0)}"
            )
            lines.append(
                f"  Total sessions: {ca.get('total_sessions', 0)}"
            )
            cr = ca.get("crash_rate")
            lines.append(
                f"  Crash rate: {f'{cr:.2%}' if cr is not None else 'N/A'}"
            )

            mean_up = ca.get("mean_uptime_before_crash_sec")
            if mean_up is not None:
                lines.append(
                    f"  Mean uptime before crash: "
                    f"{self._fmt_duration(mean_up)}"
                )
            med_up = ca.get("median_uptime_before_crash_sec")
            if med_up is not None:
                lines.append(
                    f"  Median uptime before crash: "
                    f"{self._fmt_duration(med_up)}"
                )

            self._fmt_distribution(
                lines, "Crashes by signal", ca.get("crashes_by_signal")
            )
            self._fmt_distribution(
                lines,
                "Crashes by platform",
                ca.get("crashes_by_platform"),
            )
            self._fmt_distribution(
                lines,
                "Crashes by version",
                ca.get("crashes_by_version"),
            )

            cr_ver = ca.get("crash_rate_per_version", {})
            if cr_ver:
                lines.append("\n  Crash rate per version:")
                for ver, rate in cr_ver.items():
                    lines.append(
                        f"    {ver}: "
                        f"{f'{rate:.2%}' if rate is not None else 'N/A'}"
                    )

            self._fmt_distribution(
                lines,
                "Uptime before crash",
                ca.get("uptime_distribution_before_crash"),
            )

        # Updates
        ua = metrics.get("update_analysis", {})
        if not ua.get("note"):
            lines.append(f"\n{sep}")
            lines.append(
                f"  UPDATES: {ua.get('total_attempts', 0)} attempts, "
                f"{ua.get('successes', 0)} succeeded, "
                f"{ua.get('failures', 0)} failed "
                f"({ua.get('success_rate', 0):.0f}% success rate)"
            )
            lines.append(sep)
            self._fmt_distribution(
                lines,
                "Failure reasons",
                ua.get("failure_reasons"),
            )
            self._fmt_distribution(
                lines,
                "Failures by platform",
                ua.get("failures_by_platform"),
            )

        # Memory analysis
        mem = metrics.get("memory_analysis", {})
        if not mem.get("note"):
            lines.append(f"\n{sep}")
            lines.append("  MEMORY ANALYSIS")
            lines.append(sep)
            lines.append(
                f"  Total snapshots: {mem.get('total_snapshots', 0)}"
            )
            for col in (
                "rss_kb",
                "vm_size_kb",
                "vm_peak_kb",
                "vm_hwm_kb",
            ):
                mean = mem.get(f"{col}_mean")
                mx = mem.get(f"{col}_max")
                p95 = mem.get(f"{col}_p95")
                if mean is not None:
                    lines.append(
                        f"  {col}: mean={mean:.0f}"
                        f" max={mx:.0f} p95={p95:.0f}"
                    )

        # Hardware analysis
        hw = metrics.get("hardware_analysis", {})
        if not hw.get("note"):
            lines.append(f"\n{sep}")
            lines.append("  HARDWARE ANALYSIS")
            lines.append(sep)
            lines.append(
                f"  Total profiles: {hw.get('total_profiles', 0)}"
            )
            self._fmt_distribution(
                lines,
                "Printer model (top 20)",
                hw.get("printer_model_distribution"),
            )
            self._fmt_distribution(
                lines,
                "Kinematics",
                hw.get("kinematics_distribution"),
            )
            self._fmt_distribution(
                lines,
                "Primary MCU (top 10)",
                hw.get("primary_mcu_distribution"),
            )
            self._fmt_distribution(
                lines,
                "Extruder count",
                hw.get("extruder_count_distribution"),
            )
            self._fmt_distribution(
                lines,
                "AMS type",
                hw.get("ams_type_distribution"),
            )
            self._fmt_distribution(
                lines,
                "Display backend",
                hw.get("display_backend_distribution"),
            )
            # Capability adoption percentages
            cap_lines = []
            for cap in (
                "capabilities.has_chamber",
                "capabilities.has_accelerometer",
                "capabilities.has_firmware_retraction",
                "capabilities.has_exclude_object",
                "capabilities.has_timelapse",
                "capabilities.has_klippain_shaketune",
                "probe.has_probe",
                "probe.has_bed_mesh",
                "probe.has_qgl",
            ):
                pct = hw.get(f"{cap}_pct")
                if pct is not None:
                    cap_lines.append(
                        f"    {cap.split('.')[-1]}: {pct}%"
                    )
            if cap_lines:
                lines.append("\n  Capability adoption (%):")
                lines.extend(cap_lines)

        # Settings analysis
        sa = metrics.get("settings_analysis", {})
        if not sa.get("note"):
            lines.append(f"\n{sep}")
            lines.append("  SETTINGS ANALYSIS")
            lines.append(sep)
            lines.append(
                f"  Total snapshots: {sa.get('total_snapshots', 0)}"
            )
            self._fmt_distribution(
                lines, "Theme", sa.get("theme_distribution")
            )
            self._fmt_distribution(
                lines, "Locale", sa.get("locale_distribution")
            )
            self._fmt_distribution(
                lines,
                "Time format",
                sa.get("time_format_distribution"),
            )
            bm = sa.get("brightness_mean")
            if bm is not None:
                lines.append(f"\n  Average brightness: {bm:.0f}%")

        # Panel usage
        pu = metrics.get("panel_usage_analysis", {})
        if not pu.get("note"):
            lines.append(f"\n{sep}")
            lines.append("  PANEL USAGE")
            lines.append(sep)
            lines.append(
                f"  Sessions with usage data: "
                f"{pu.get('total_sessions', 0)}"
            )
            avg_dur = pu.get("avg_session_duration_sec")
            if avg_dur:
                lines.append(
                    f"  Avg session duration: "
                    f"{self._fmt_duration(avg_dur)}"
                )
            self._fmt_distribution(
                lines,
                "Total time by panel (sec)",
                pu.get("total_time_by_panel_sec"),
            )
            self._fmt_distribution(
                lines,
                "Total visits by panel",
                pu.get("total_visits_by_panel"),
            )

        # Connection stability
        cs = metrics.get("connection_analysis", {})
        if not cs.get("note"):
            lines.append(f"\n{sep}")
            lines.append("  CONNECTION STABILITY")
            lines.append(sep)
            lines.append(
                f"  Sessions: {cs.get('total_sessions', 0)}"
            )
            pct = cs.get("overall_connected_pct")
            if pct is not None:
                lines.append(f"  Overall connected: {pct:.1f}%")
            for col in (
                "connect_count",
                "disconnect_count",
                "klippy_error_count",
                "klippy_shutdown_count",
            ):
                total = cs.get(f"{col}_total")
                mean = cs.get(f"{col}_mean")
                if total is not None:
                    lines.append(
                        f"  {col}: total={total}"
                        f" mean={mean:.2f}/session"
                    )
            ld = cs.get("longest_disconnect_max_sec")
            if ld is not None:
                lines.append(
                    f"  Longest disconnect: "
                    f"{self._fmt_duration(ld)}"
                )

        # Print start context
        ps = metrics.get("print_start_analysis", {})
        if not ps.get("note"):
            lines.append(f"\n{sep}")
            lines.append("  PRINT START CONTEXT")
            lines.append(sep)
            lines.append(
                f"  Total print starts: "
                f"{ps.get('total_print_starts', 0)}"
            )
            tp = ps.get("thumbnail_pct")
            if tp is not None:
                lines.append(f"  Has thumbnail: {tp:.1f}%")
            ap = ps.get("ams_active_pct")
            if ap is not None:
                lines.append(f"  AMS active: {ap:.1f}%")
            self._fmt_distribution(
                lines, "Source", ps.get("source_distribution")
            )
            self._fmt_distribution(
                lines,
                "Slicer (top 10)",
                ps.get("slicer_distribution"),
            )
            self._fmt_distribution(
                lines,
                "File size",
                ps.get("file_size_distribution"),
            )
            self._fmt_distribution(
                lines,
                "Estimated duration",
                ps.get("duration_estimate_distribution"),
            )

        # Error analysis
        ea = metrics.get("error_analysis", {})
        if not ea.get("note"):
            lines.append(f"\n{sep}")
            lines.append("  ERROR ANALYSIS")
            lines.append(sep)
            lines.append(
                f"  Total errors: {ea.get('total_errors', 0)}"
            )
            self._fmt_distribution(
                lines,
                "By category",
                ea.get("errors_by_category"),
            )
            self._fmt_distribution(
                lines,
                "By code (top 10)",
                ea.get("errors_by_code"),
            )
            self._fmt_distribution(
                lines,
                "By context (top 10)",
                ea.get("errors_by_context"),
            )

        lines.append(f"\n{sep}")
        return "\n".join(lines)

    def format_json(self, metrics: dict) -> str:
        return json.dumps(metrics, indent=2, default=str)

    def format_html(self, metrics: dict, output_path: str) -> None:
        json_str = self.format_json(metrics)
        terminal_str = self.format_terminal(metrics)
        generated_at = metrics.get("generated_at", "")
        html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HelixScreen Telemetry Report</title>
<style>
  body {{
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    max-width: 960px; margin: 2rem auto; padding: 0 1rem;
    background: #1a1a2e; color: #e0e0e0;
  }}
  h1 {{ color: #00d4ff; border-bottom: 2px solid #00d4ff; padding-bottom: 0.5rem; }}
  h2 {{ color: #00d4ff; margin-top: 2rem; }}
  pre {{
    background: #16213e; padding: 1rem; border-radius: 8px;
    overflow-x: auto; font-size: 0.85rem; line-height: 1.4;
    white-space: pre-wrap;
  }}
  .meta {{ color: #888; font-size: 0.9rem; }}
  .tabs {{ display: flex; gap: 0.5rem; margin: 1rem 0; }}
  .tab {{
    padding: 0.5rem 1rem; cursor: pointer; border-radius: 4px;
    background: #16213e; border: 1px solid #333;
  }}
  .tab.active {{ background: #00d4ff; color: #1a1a2e; font-weight: bold; }}
  .tab-content {{ display: none; }}
  .tab-content.active {{ display: block; }}
</style>
</head>
<body>
<h1>HelixScreen Telemetry Report</h1>
<p class="meta">Generated: {generated_at}</p>
<p class="meta">Charts will be added in a future version.</p>

<div class="tabs">
  <div class="tab active" onclick="showTab('summary')">Summary</div>
  <div class="tab" onclick="showTab('json')">Raw JSON</div>
</div>

<div id="tab-summary" class="tab-content active">
<pre>{terminal_str}</pre>
</div>

<div id="tab-json" class="tab-content">
<pre>{json_str}</pre>
</div>

<script>
function showTab(name) {{
  document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(el => el.classList.remove('active'));
  document.getElementById('tab-' + name).classList.add('active');
  event.target.classList.add('active');
}}
</script>
</body>
</html>
"""
        with open(output_path, "w") as f:
            f.write(html)
        print(f"HTML report written to {output_path}", file=sys.stderr)

    # -- Helpers -----------------------------------------------------------

    def _join_session_field(
        self, df: pd.DataFrame, field: str
    ) -> pd.DataFrame:
        """Join a session field onto another dataframe by device_id."""
        if field not in self.sessions.columns:
            return df
        device_map = self.sessions.groupby("device_id")[field].first()
        result = df.copy()
        result[field] = result["device_id"].map(device_map)
        return result

    @staticmethod
    def _distribution(
        df: pd.DataFrame, column: str, top: Optional[int] = None
    ) -> dict:
        """Compute value_counts for a column, optionally top-N."""
        if column not in df.columns:
            return {}
        counts = df[column].dropna().value_counts()
        if top is not None:
            counts = counts.head(top)
        return counts.to_dict()

    @staticmethod
    def _success_rate_by(
        df: pd.DataFrame, column: str, min_count: int = 1
    ) -> dict:
        """Compute success rate grouped by a column."""
        if column not in df.columns or "outcome" not in df.columns:
            return {}
        grouped = df.groupby(column)
        result = {}
        for name, group in grouped:
            total = len(group)
            if total < min_count:
                continue
            successes = (group["outcome"] == "success").sum()
            result[str(name)] = {
                "rate": round(successes / total * 100, 1),
                "total": total,
            }
        return dict(sorted(result.items(), key=lambda x: -x[1]["rate"]))

    @staticmethod
    def _fmt_distribution(
        lines: list[str], title: str, data: Optional[dict]
    ) -> None:
        if not data:
            return
        lines.append(f"\n  {title}:")
        for key, count in data.items():
            lines.append(f"    {key}: {count}")

    @staticmethod
    def _fmt_percentages(
        lines: list[str], title: str, data: Optional[dict]
    ) -> None:
        if not data:
            return
        lines.append(f"\n  {title}:")
        for key, pct in data.items():
            lines.append(f"    {key}: {pct}%")

    @staticmethod
    def _fmt_duration(seconds: float) -> str:
        if seconds < 60:
            return f"{seconds:.0f}s"
        if seconds < 3600:
            return f"{seconds / 60:.1f}min"
        return f"{seconds / 3600:.1f}hr"


def find_project_root() -> Path:
    """Walk up from script location to find the project root."""
    path = Path(__file__).resolve().parent
    for _ in range(10):
        if (path / ".git").exists() or (path / "Makefile").exists():
            return path
        path = path.parent
    return Path.cwd()


def main():
    parser = argparse.ArgumentParser(
        description="HelixScreen Telemetry Analytics"
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output metrics as JSON",
    )
    parser.add_argument(
        "--html",
        metavar="FILE",
        help="Generate self-contained HTML report",
    )
    parser.add_argument(
        "--since",
        metavar="YYYY-MM-DD",
        help="Only include events on or after this date",
    )
    parser.add_argument(
        "--until",
        metavar="YYYY-MM-DD",
        help="Only include events on or before this date",
    )
    parser.add_argument(
        "--data-dir",
        metavar="PATH",
        help="Override default data directory",
    )
    args = parser.parse_args()

    if args.data_dir:
        data_dir = args.data_dir
    else:
        root = find_project_root()
        data_dir = str(root / ".telemetry-data" / "events")

    analyzer = TelemetryAnalyzer()
    analyzer.load_events(data_dir, since=args.since, until=args.until)

    if analyzer.all_events.empty:
        print("No data.", file=sys.stderr)
        sys.exit(0)

    metrics = analyzer.compute_all()

    if args.json:
        print(analyzer.format_json(metrics))
    elif args.html:
        analyzer.format_html(metrics, args.html)
        # Also print terminal output
        print(analyzer.format_terminal(metrics))
    else:
        print(analyzer.format_terminal(metrics))


if __name__ == "__main__":
    main()
