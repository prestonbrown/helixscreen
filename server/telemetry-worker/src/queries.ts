// SPDX-License-Identifier: GPL-3.0-or-later
// SQL query builders for Analytics Engine dashboard endpoints.

export interface QueryConfig {
  accountId: string;
  apiToken: string;
}

export interface FilterParams {
  platform?: string;
  version?: string;
  model?: string;
}

/**
 * Sanitize a filter value to prevent SQL injection.
 * Only allows alphanumeric, dots, hyphens, and underscores.
 */
function sanitizeFilterValue(val: string): string {
  return val.replace(/[^a-zA-Z0-9.\-_]/g, "");
}

/**
 * Build SQL AND clauses from filter params.
 * blob2=version, blob3=platform, blob4=model (for session-like events).
 */
export function buildFilterClause(
  filters?: FilterParams,
  blobMap?: { platform?: string; version?: string; model?: string },
): string {
  if (!filters) return "";
  const map = {
    platform: blobMap?.platform ?? "blob3",
    version: blobMap?.version ?? "blob2",
    model: blobMap?.model ?? "blob4",
  };
  let clause = "";
  for (const [key, col] of Object.entries(map)) {
    const val = filters[key as keyof FilterParams];
    if (!val) continue;
    const parts = val.split(",").map((v) => sanitizeFilterValue(v.trim())).filter(Boolean);
    if (parts.length === 0) continue;
    if (parts.length === 1) {
      clause += ` AND ${col} = '${parts[0]}'`;
    } else {
      clause += ` AND ${col} IN (${parts.map((p) => `'${p}'`).join(", ")})`;
    }
  }
  return clause;
}

/**
 * Parse a range string like "7d", "30d", "90d" into a SQL timestamp filter.
 * Returns the number of days, clamped to [1, 90].
 */
export function parseRange(range: string | null): number {
  if (!range) return 30;
  const match = range.match(/^(\d+)d$/);
  if (!match) return 30;
  const days = parseInt(match[1], 10);
  return Math.max(1, Math.min(days, 90));
}

/**
 * Execute a SQL query against the Analytics Engine SQL API.
 */
export async function executeQuery(
  config: QueryConfig,
  sql: string,
): Promise<unknown> {
  const url = `https://api.cloudflare.com/client/v4/accounts/${config.accountId}/analytics_engine/sql`;
  const res = await fetch(url, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${config.apiToken}`,
      "Content-Type": "text/plain",
    },
    body: sql,
  });

  if (!res.ok) {
    const text = await res.text();
    throw new Error(`Analytics Engine SQL API error (${res.status}): ${text}`);
  }

  const result = (await res.json()) as { data?: Array<Record<string, unknown>> };

  // Analytics Engine returns ALL values as strings. Coerce numeric-looking values
  // to numbers so downstream code can safely do arithmetic without concatenation.
  if (result.data) {
    for (const row of result.data) {
      for (const [key, val] of Object.entries(row)) {
        if (typeof val === "string" && val !== "") {
          // Skip date-like (2026-03-24) and version-like (v0.99.0) strings
          if (/^\d{4}-\d{2}-\d{2}/.test(val) || /^v?\d+\.\d+/.test(val)) continue;
          const num = Number(val);
          if (!isNaN(num)) row[key] = num;
        }
      }
    }
  }

  return result;
}

// SQL query builders for each dashboard endpoint

export function overviewQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Active devices (unique device_ids from sessions)
    `SELECT count(DISTINCT blob1) as active_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${f}`,
    // Total events
    `SELECT count() as total_events FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${f}`,
    // Crash rate (crashes / sessions)
    `SELECT
      sumIf(1, index1 = 'crash') as crash_count,
      sumIf(1, index1 = 'session') as session_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${f}`,
    // Print success rate
    `SELECT
      sumIf(1, blob2 = 'success') as successes,
      count() as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${buildFilterClause(filters, { version: "blob4" })}`,
    // Events over time
    `SELECT
      toDate(timestamp) as date,
      count() as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${f}
    GROUP BY date
    ORDER BY date`,
    // Daily active devices (unique device_ids with sessions per day)
    `SELECT
      toDate(timestamp) as date,
      count(DISTINCT blob1) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${f}
    GROUP BY date
    ORDER BY date`,
    // First-seen date per device (for cumulative growth curve)
    `SELECT
      toDate(min(timestamp)) as first_seen,
      count() as new_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${f}
    GROUP BY blob1`,
    // CDN fleet: distinct sources polling the update manifest per day.
    //
    // Deliberately unfiltered by platform/version/model - this event is written
    // by the daily cron from zone HTTP analytics and carries none of those
    // dimensions, so applying the filter clause would blank the metric out
    // whenever a dashboard filter is set.
    //
    // blob1 is the UTC day described (not a device_id), blob2 the channel.
    // max() rather than sum() so a re-run or backfill of the same day collapses
    // instead of double-counting. The window is widened by two days because the
    // row's own timestamp is the snapshot time, a day after the day it covers.
    `SELECT
      blob1 as date,
      max(double2) as sources,
      max(double1) as polls,
      max(double4) as errors
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days + 2}' DAY
      AND index1 = 'cdn_fleet_daily'
      AND blob2 = 'stable'
    GROUP BY date
    ORDER BY date`,
  ];
}

export function adoptionQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Platforms
    `SELECT blob3 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob3 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Versions
    `SELECT blob2 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob2 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Printer models
    `SELECT blob4 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob4 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Kinematics
    `SELECT blob5 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob5 != ''${f} GROUP BY name ORDER BY count DESC`,
  ];
}

export function printsQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  // print_outcome has version in blob4, no platform blob — use custom blob map
  const f = buildFilterClause(filters, { version: "blob4" });
  return [
    // Success rate over time
    `SELECT
      toDate(timestamp) as date,
      sumIf(1, blob2 = 'success') / count() as rate,
      count() as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${f}
    GROUP BY date
    ORDER BY date`,
    // By filament type
    `SELECT
      blob3 as type,
      sumIf(1, blob2 = 'success') / count() as success_rate,
      count() as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome' AND blob3 != ''${f}
    GROUP BY type
    ORDER BY count DESC`,
    // Average duration
    `SELECT avg(double1) as avg_duration_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${f}`,
  ];
}

export function crashesQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  // crash event: blob2=version, blob4=platform — custom blob map
  const f = buildFilterClause(filters, { platform: "blob4" });
  // Session events use the default platform column layout (not blob4); keep
  // their filter un-remapped so platform filtering still works.
  const sessF = buildFilterClause(filters);
  return [
    // By version (crash count + session count for rate)
    `SELECT
      blob2 as ver,
      count() as crash_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND blob2 != ''${f}
    GROUP BY ver
    ORDER BY crash_count DESC`,
    // Session counts by version (for crash rate denominator)
    `SELECT
      blob2 as ver,
      count() as session_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob2 != ''${sessF}
    GROUP BY ver`,
    // By signal
    `SELECT blob3 as signal, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND blob3 != ''${f} GROUP BY signal ORDER BY count DESC`,
    // Average uptime
    `SELECT avg(double1) as avg_uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${f}`,
    // Crashes by platform (count + unique crashing devices).
    // Crash events use the custom blob map: blob1=device_id, blob2=version,
    // blob3=signal, blob4=platform.
    `SELECT
      blob4 as platform,
      count() as crash_count,
      uniqExact(blob1) as device_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND blob4 != ''${f}
    GROUP BY platform
    ORDER BY crash_count DESC`,
    // Sessions by platform (for per-platform rate denominator + device count).
    // Session events use the default blob map: blob1=device_id, blob2=version,
    // blob3=platform, blob4=model.
    `SELECT
      blob3 as platform,
      count() as session_count,
      uniqExact(blob1) as device_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob3 != ''${sessF}
    GROUP BY platform
    ORDER BY session_count DESC`,
    // Trailing 14-day rate: overall crashes (all platforms, including empty).
    // NOTE: the per-platform queries below filter `blob3/blob4 != ''`, so the
    // sum of per-platform counts may be slightly less than these overall
    // totals if any events arrive with unset platform. In practice our client
    // always sets app_platform, so the gap is zero, but the overall totals are
    // deliberately un-filtered for truthfulness on the gauge.
    `SELECT count() as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'crash'${f}`,
    // Trailing 14-day rate: overall sessions
    `SELECT count() as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'session'${sessF}`,
    // Trailing 14-day rate: per-platform crashes
    `SELECT blob4 as platform, count() as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'crash' AND blob4 != ''${f} GROUP BY platform`,
    // Trailing 14-day rate: per-platform sessions
    `SELECT blob3 as platform, count() as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'session' AND blob3 != ''${sessF} GROUP BY platform`,
  ];
}

export function crashListQuery(days: number, limit: number, filters?: FilterParams): string {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters, { platform: "blob4" });
  return `SELECT
    timestamp,
    blob1 as device_id,
    blob2 as ver,
    blob3 as sig,
    blob4 as platform,
    double1 as uptime_sec
  FROM ${dataset}
  WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${f}
  ORDER BY timestamp DESC
  LIMIT ${limit}`;
}

export function memoryQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // RSS over time (avg/p95/max by date — one sample per device per day via _sample_interval weighting)
    `SELECT
      toDate(timestamp) as date,
      avg(double2) as avg_rss_kb,
      quantileExactWeighted(0.95)(double2, _sample_interval) as p95_rss_kb,
      max(double2) as max_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
    // RSS by platform (avg by blob3)
    `SELECT
      blob3 as platform,
      avg(double2) as avg_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot' AND blob3 != ''${f}
    GROUP BY platform
    ORDER BY avg_rss_kb DESC`,
    // VM peak trend (avg by date)
    `SELECT
      toDate(timestamp) as date,
      avg(double4) as avg_vm_peak_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
  ];
}

export function memoryWarningQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Warning count by level
    `SELECT
      blob4 as level,
      count() as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning' AND blob4 != ''${f}
    GROUP BY level
    ORDER BY count DESC`,
    // Warnings over time (count by date, split by level)
    `SELECT
      toDate(timestamp) as date,
      blob4 as level,
      count() as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    GROUP BY date, level
    ORDER BY date`,
    // RSS at warning time (avg/max by date)
    `SELECT
      toDate(timestamp) as date,
      avg(double2) as avg_rss_kb,
      max(double2) as max_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    GROUP BY date
    ORDER BY date`,
    // Warnings by platform
    `SELECT
      blob3 as platform,
      count() as count,
      avg(double2) as avg_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning' AND blob3 != ''${f}
    GROUP BY platform
    ORDER BY count DESC`,
    // Affected devices count
    `SELECT count(DISTINCT blob1) as affected_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}`,
    // Recent warnings list (newest first)
    `SELECT
      timestamp,
      blob1 as device_id,
      blob2 as version,
      blob3 as platform,
      blob4 as level,
      blob5 as reason,
      double1 as uptime_sec,
      double2 as rss_kb,
      double4 as system_available_mb,
      double5 as growth_5min_kb,
      double6 as private_dirty_kb,
      double7 as pss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    ORDER BY timestamp DESC
    LIMIT 100`,
  ];
}

export function hardwareQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Printer models (top 20, deduplicated by device_id)
    `SELECT blob4 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob4 != ''${f} GROUP BY name ORDER BY count DESC LIMIT 20`,
    // Kinematics (deduplicated by device_id)
    `SELECT blob5 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob5 != ''${f} GROUP BY name ORDER BY count DESC`,
    // MCU chips (deduplicated by device_id)
    `SELECT blob6 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob6 != ''${f} GROUP BY name ORDER BY count DESC LIMIT 20`,
    // Capabilities bitmask — deduplicate: subquery gets latest row per device, outer query aggregates
    `SELECT
      sum(toUInt32(bitAnd(toUInt32(caps), 1) > 0)) as cap_0,
      sum(toUInt32(bitAnd(toUInt32(caps), 2) > 0)) as cap_1,
      sum(toUInt32(bitAnd(toUInt32(caps), 4) > 0)) as cap_2,
      sum(toUInt32(bitAnd(toUInt32(caps), 8) > 0)) as cap_3,
      sum(toUInt32(bitAnd(toUInt32(caps), 16) > 0)) as cap_4,
      sum(toUInt32(bitAnd(toUInt32(caps), 32) > 0)) as cap_5,
      sum(toUInt32(bitAnd(toUInt32(caps), 64) > 0)) as cap_6,
      sum(toUInt32(bitAnd(toUInt32(caps), 128) > 0)) as cap_7,
      sum(toUInt32(bitAnd(toUInt32(caps), 256) > 0)) as cap_8,
      sum(toUInt32(bitAnd(toUInt32(caps), 512) > 0)) as cap_9,
      sum(toUInt32(bitAnd(toUInt32(caps), 1024) > 0)) as cap_10,
      sum(toUInt32(bitAnd(toUInt32(caps), 2048) > 0)) as cap_11,
      sum(toUInt32(bitAnd(toUInt32(caps), 4096) > 0)) as cap_12,
      count() as total
    FROM (
      SELECT blob1, argMax(double8, timestamp) as caps
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile'${f}
      GROUP BY blob1
    )`,
    // Build volume averages (deduplicated: latest per device)
    `SELECT
      avg(vol_x) as avg_vol_x,
      avg(vol_y) as avg_vol_y,
      avg(vol_z) as avg_vol_z
    FROM (
      SELECT blob1, argMax(double5, timestamp) as vol_x, argMax(double6, timestamp) as vol_y, argMax(double7, timestamp) as vol_z
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND double5 > 0${f}
      GROUP BY blob1
    )`,
    // Fan/sensor/macro count averages (deduplicated: latest per device, filter out rows where all three are zero — old mis-mapped data)
    `SELECT
      avg(fans) as avg_fan_count,
      avg(sensors) as avg_sensor_count,
      avg(macros) as avg_macro_count
    FROM (
      SELECT blob1, argMax(double2, timestamp) as fans, argMax(double3, timestamp) as sensors, argMax(double4, timestamp) as macros
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND (double2 > 0 OR double3 > 0 OR double4 > 0)${f}
      GROUP BY blob1
    )`,
    // Host RAM distribution (from session events, deduplicated by device_id)
    `SELECT
      ram_mb,
      count() as count
    FROM (
      SELECT blob1, argMax(double1, timestamp) as ram_mb
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND double1 > 0${f}
      GROUP BY blob1
    )
    GROUP BY ram_mb
    ORDER BY ram_mb`,
    // AMS backend distribution (deduplicated by device_id)
    `SELECT blob8 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob8 != ''${f} GROUP BY name ORDER BY count DESC`,
    // helix_macros.cfg adoption: "1" installed, "0" not. Empty is excluded, so
    // the denominator is devices that actually reported, not all devices.
    `SELECT blob9 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob9 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Moonraker topology: "1" on this machine, "0" over the network. Only
    // clients new enough to report it are counted (see the blob9 note).
    `SELECT blob10 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob10 != ''${f} GROUP BY name ORDER BY count DESC`,
  ];
}

export function engagementQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Panel time (sum double2 by blob4 from panel_usage, exclude "home")
    `SELECT blob4 as panel, sum(double2) as total_time_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != '' AND blob4 != 'home'${f} GROUP BY panel ORDER BY total_time_sec DESC`,
    // Panel visits (sum double3 by blob4, exclude "home")
    `SELECT blob4 as panel, sum(double3) as total_visits FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != '' AND blob4 != 'home'${f} GROUP BY panel ORDER BY total_visits DESC`,
    // Session duration trend (avg double1 by date)
    `SELECT
      toDate(timestamp) as date,
      avg(double1) as avg_session_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage'${f}
    GROUP BY date
    ORDER BY date`,
    // Theme distribution (deduplicated by device_id)
    `SELECT blob4 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot' AND blob4 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Locale distribution (deduplicated by device_id)
    `SELECT blob5 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot' AND blob5 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Brightness quantiles
    `SELECT
      quantileExactWeighted(0.25)(double1, _sample_interval) as p25,
      quantileExactWeighted(0.5)(double1, _sample_interval) as p50,
      quantileExactWeighted(0.75)(double1, _sample_interval) as p75
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot'${f}`,
    // Dark vs Light mode distribution (deduplicated by device_id)
    `SELECT blob8 as name, count(DISTINCT blob1) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot' AND blob8 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Widget placement (count by blob4 from widget_placement, unique per device)
    `SELECT blob4 as widget, count(DISTINCT blob1) as devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'widget_placement' AND blob4 != ''${f} GROUP BY widget ORDER BY devices DESC`,
    // Widget interactions (sum double2 by blob4 from widget_interaction)
    `SELECT blob4 as widget, sum(double2) as interactions FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'widget_interaction' AND blob4 != ''${f} GROUP BY widget ORDER BY interactions DESC`,
    // Overlay visits (sum double2 by blob4 from overlay_visit)
    `SELECT blob4 as overlay, sum(double2) as total_visits FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'overlay_visit' AND blob4 != ''${f} GROUP BY overlay ORDER BY total_visits DESC`,
  ];
}

export function reliabilityQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Uptime % (avg connected/(connected+disconnected) by date)
    `SELECT
      toDate(timestamp) as date,
      avg(double4 / (double4 + double5 + 0.001)) as avg_uptime_pct
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${f}
    GROUP BY date
    ORDER BY date`,
    // Disconnect frequency (avg double3 by date)
    `SELECT
      toDate(timestamp) as date,
      avg(double3) as avg_disconnects
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${f}
    GROUP BY date
    ORDER BY date`,
    // Longest outage (max double6)
    `SELECT max(double6) as max_disconnect_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${f}`,
    // Top error categories (count by blob4 from error_encountered)
    `SELECT blob4 as category, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 != ''${f} GROUP BY category ORDER BY count DESC LIMIT 20`,
    // Error codes (count by blob4, blob5 from error_encountered)
    `SELECT blob4 as category, blob5 as code, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 != ''${f} GROUP BY category, code ORDER BY count DESC LIMIT 50`,
  ];
}

export function stabilityQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  // crash event: blob2=version, blob4=platform — custom blob map
  const crashFilter = buildFilterClause(filters, { platform: "blob4" });
  const f = buildFilterClause(filters);
  return [
    // 0: Crash count over time
    `SELECT toDate(timestamp) as date, count() as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${crashFilter} GROUP BY date ORDER BY date`,
    // 1: Session count over time (for rate denominator)
    `SELECT toDate(timestamp) as date, count() as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${f} GROUP BY date ORDER BY date`,
    // 2: Crash count by version
    `SELECT blob2 as ver, count() as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND blob2 != ''${crashFilter} GROUP BY ver ORDER BY crash_count DESC`,
    // 3: Session count by version
    `SELECT blob2 as ver, count() as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob2 != ''${f} GROUP BY ver`,
    // 4: Crashes by signal
    `SELECT blob3 as signal, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND blob3 != ''${crashFilter} GROUP BY signal ORDER BY count DESC`,
    // 5: Average uptime before crash
    `SELECT avg(double1) as avg_uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${crashFilter}`,
    // 6: Klippy errors and shutdowns over time
    `SELECT toDate(timestamp) as date, sum(double7) as errors, sum(double8) as shutdowns FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${f} GROUP BY date ORDER BY date`,
    // 7: Memory warning count over time
    `SELECT toDate(timestamp) as date, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f} GROUP BY date ORDER BY date`,
    // 8: Error hotspots (categories)
    `SELECT blob4 as category, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 != ''${f} GROUP BY category ORDER BY count DESC LIMIT 20`,
    // 9: Error codes detail
    `SELECT blob4 as category, blob5 as code, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 != ''${f} GROUP BY category, code ORDER BY count DESC LIMIT 50`,
    // 10: Display anomaly count over time (category='display')
    `SELECT toDate(timestamp) as date, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 = 'display'${f} GROUP BY date ORDER BY date`,
    // 11: Display anomaly codes breakdown
    `SELECT blob5 as code, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 = 'display'${f} GROUP BY code ORDER BY count DESC LIMIT 20`,
    // 12: Display anomaly by version
    `SELECT blob2 as version, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 = 'display' AND blob2 != ''${f} GROUP BY version ORDER BY count DESC LIMIT 20`,
    // 13: Recent display anomalies with context (for backtrace inspection)
    `SELECT timestamp, blob1 as device_id, blob2 as version, blob3 as platform, blob5 as code, blob6 as context, double1 as uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 = 'display'${f} ORDER BY timestamp DESC LIMIT 50`,
    // 14: Display anomaly affected devices count
    `SELECT count(DISTINCT blob1) as affected_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND blob4 = 'display'${f}`,
  ];
}

export function printStartQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Slicer distribution (by blob4)
    `SELECT blob4 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND blob4 != ''${f} GROUP BY name ORDER BY count DESC`,
    // File size buckets (by blob5)
    `SELECT blob5 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND blob5 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Thumbnail adoption (avg double2, convert 0-1 to percentage)
    `SELECT avg(double2) * 100 as thumbnail_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // AMS usage (avg double3, convert 0-1 to percentage)
    `SELECT avg(double3) * 100 as ams_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // Source distribution (by blob7)
    `SELECT blob7 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND blob7 != ''${f} GROUP BY name ORDER BY count DESC`,
  ];
}

export function performanceQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // 0: Frame time percentiles over time
    `SELECT
      toDate(timestamp) as date,
      avg(double2) as avg_p50,
      avg(double3) as avg_p95,
      avg(double4) as avg_p99
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
    // 1: Drop rate by platform
    `SELECT
      blob3 as platform,
      sum(double5) as dropped,
      sum(double6) as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND blob3 != ''${f}
    GROUP BY platform
    ORDER BY dropped DESC`,
    // 2: Drop rate by version over time
    `SELECT
      toDate(timestamp) as date,
      blob2 as version,
      sum(double5) as dropped,
      sum(double6) as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND blob2 != ''${f}
    GROUP BY date, version
    ORDER BY date`,
    // 3: Worst panels
    `SELECT
      blob4 as panel,
      count() as times_worst,
      avg(double7) as avg_p95_ms
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND blob4 != ''${f}
    GROUP BY panel
    ORDER BY times_worst DESC
    LIMIT 20`,
    // 4: Fleet-wide metrics
    `SELECT
      avg(double2) as fleet_p50,
      sum(double5) as total_dropped,
      sum(double6) as total_frames,
      count(DISTINCT blob1) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}`,
    // 5: Devices with high drop rate (>5%)
    `SELECT count() as high_drop_devices
    FROM (
      SELECT blob1, sum(double5) as dropped, sum(double6) as total
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
      GROUP BY blob1
      HAVING total > 0 AND dropped / total > 0.05
    )`,
  ];
}

export function featuresQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // 0: Per-feature adoption rate (deduplicated by device)
    `SELECT
      avg(d1) as macros, avg(d2) as camera, avg(d3) as bed_mesh,
      avg(d4) as console_gcode, avg(d5) as input_shaper,
      avg(d6) as filament_management, avg(d7) as manual_probe,
      avg(extra) as extra_bitmask_avg,
      count() as total_devices
    FROM (
      SELECT blob1,
        argMax(double1, timestamp) as d1, argMax(double2, timestamp) as d2,
        argMax(double3, timestamp) as d3, argMax(double4, timestamp) as d4,
        argMax(double5, timestamp) as d5, argMax(double6, timestamp) as d6,
        argMax(double7, timestamp) as d7, argMax(double8, timestamp) as extra
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption'${f}
      GROUP BY blob1
    )`,
    // 1: Feature adoption by version
    `SELECT
      blob2 as version,
      avg(double1) as macros, avg(double2) as camera, avg(double3) as bed_mesh,
      avg(double4) as console_gcode, avg(double5) as input_shaper,
      avg(double6) as filament_management, avg(double7) as manual_probe,
      avg(double8) as extra_bitmask_avg,
      count(DISTINCT blob1) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption' AND blob2 != ''${f}
    GROUP BY version
    ORDER BY version`,
    // 2: Total tracked devices
    `SELECT count(DISTINCT blob1) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption'${f}`,
  ];
}

export function uxInsightsQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // 0: Panel time distribution
    `SELECT blob4 as panel, sum(double2) as total_time_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != '' AND blob4 != 'home'${f}
    GROUP BY panel
    ORDER BY total_time_sec DESC`,
    // 1: Panel visit frequency
    `SELECT blob4 as panel, sum(double3) as total_visits, count(DISTINCT blob1) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != '' AND blob4 != 'home'${f}
    GROUP BY panel
    ORDER BY total_visits DESC`,
    // 2: Settings change frequency
    `SELECT blob4 as setting, count() as change_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND blob4 != ''${f}
    GROUP BY setting
    ORDER BY change_count DESC`,
    // 3: Settings defaults (devices that changed each setting)
    `SELECT blob4 as setting, count(DISTINCT blob1) as devices_changed
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND blob4 != ''${f}
    GROUP BY setting
    ORDER BY devices_changed DESC`,
    // 4: Total devices for normalization
    `SELECT count(DISTINCT blob1) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${f}`,
    // 5: Avg session duration
    `SELECT avg(double1) as avg_session_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage'${f}`,
    // 6: Settings change rate
    `SELECT count() as total_changes, count(DISTINCT blob1) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change'${f}`,
  ];
}

export function releasesQueries(versions: string[]): string[] {
  const dataset = "helixscreen_telemetry";
  // Clean version strings — strip 'v' prefix if present for matching
  const cleanVersions = versions.map((v) => v.replace(/^v/, ""));
  const versionList = cleanVersions.map((v) => `'${v}'`).join(", ");
  return [
    // Per-version stats: sessions, crashes, self-update outcomes.
    //
    // This groups by blob2 across event types, which only works because blob2
    // means "the version the device is RUNNING" everywhere — including the
    // update events (see analytics.ts). For update_failed that is from_version:
    // keying on the target would bucket every failure under the newest release
    // and hide which deployed versions are actually broken
    // (prestonbrown/helixscreen#993).
    `SELECT
      blob2 as ver,
      sumIf(1, index1 = 'session') as total_sessions,
      sumIf(1, index1 = 'crash') as total_crashes,
      sumIf(1, index1 = 'update_failed') as update_failures,
      sumIf(1, index1 = 'update_success') as update_successes
    FROM ${dataset}
    WHERE blob2 IN (${versionList})
    GROUP BY ver`,
    // Per-version active devices (count distinct requires separate query)
    `SELECT
      blob2 as ver,
      count(DISTINCT blob1) as active_devices
    FROM ${dataset}
    WHERE index1 = 'session' AND blob2 IN (${versionList})
    GROUP BY ver`,
    // Per-version print stats
    `SELECT
      blob4 as ver,
      sumIf(1, blob2 = 'success') as print_successes,
      count() as print_total
    FROM ${dataset}
    WHERE index1 = 'print_outcome' AND blob4 IN (${versionList})
    GROUP BY ver`,
  ];
}
