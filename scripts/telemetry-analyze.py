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
                "total": len(self.all_events),
            },
            "adoption": self.compute_adoption_metrics(),
            "print_reliability": self.compute_print_metrics(),
            "crash_analysis": self.compute_crash_metrics(),
            "update_analysis": self.compute_update_metrics(),
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
            f"updates={ec.get('update_successes', 0) + ec.get('update_failures', 0)})"
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
