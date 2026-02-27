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

  return res.json();
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
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND blob2 != ''${buildFilterClause(filters)}
    GROUP BY ver`,
    // By signal
    `SELECT blob3 as signal, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND blob3 != ''${f} GROUP BY signal ORDER BY count DESC`,
    // Average uptime
    `SELECT avg(double1) as avg_uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${f}`,
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
    // RSS over time (avg/p95/max by date)
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

export function hardwareQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Printer models (top 20 by blob4)
    `SELECT blob4 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob4 != ''${f} GROUP BY name ORDER BY count DESC LIMIT 20`,
    // Kinematics (by blob5)
    `SELECT blob5 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob5 != ''${f} GROUP BY name ORDER BY count DESC`,
    // MCU chips (by blob6)
    `SELECT blob6 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND blob6 != ''${f} GROUP BY name ORDER BY count DESC LIMIT 20`,
    // Capabilities bitmask (bitwise check on double8 for 8 bits)
    `SELECT
      sumIf(1, bitAnd(toUInt8(double8), 1) > 0) as cap_0,
      sumIf(1, bitAnd(toUInt8(double8), 2) > 0) as cap_1,
      sumIf(1, bitAnd(toUInt8(double8), 4) > 0) as cap_2,
      sumIf(1, bitAnd(toUInt8(double8), 8) > 0) as cap_3,
      sumIf(1, bitAnd(toUInt8(double8), 16) > 0) as cap_4,
      sumIf(1, bitAnd(toUInt8(double8), 32) > 0) as cap_5,
      sumIf(1, bitAnd(toUInt8(double8), 64) > 0) as cap_6,
      sumIf(1, bitAnd(toUInt8(double8), 128) > 0) as cap_7,
      count() as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile'${f}`,
    // Build volume averages
    `SELECT
      avg(double5) as avg_vol_x,
      avg(double6) as avg_vol_y,
      avg(double7) as avg_vol_z
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND double5 > 0${f}`,
    // Fan/sensor/macro count averages
    `SELECT
      avg(double2) as avg_fan_count,
      avg(double3) as avg_sensor_count,
      avg(double4) as avg_macro_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile'${f}`,
  ];
}

export function engagementQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Panel time (sum double2 by blob4 from panel_usage)
    `SELECT blob4 as panel, sum(double2) as total_time_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != ''${f} GROUP BY panel ORDER BY total_time_sec DESC`,
    // Panel visits (sum double3 by blob4)
    `SELECT blob4 as panel, sum(double3) as total_visits FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != ''${f} GROUP BY panel ORDER BY total_visits DESC`,
    // Session duration trend (avg double1 by date)
    `SELECT
      toDate(timestamp) as date,
      avg(double1) as avg_session_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage'${f}
    GROUP BY date
    ORDER BY date`,
    // Theme distribution (count by blob4 from settings_snapshot)
    `SELECT blob4 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot' AND blob4 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Locale distribution (count by blob5 from settings_snapshot)
    `SELECT blob5 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot' AND blob5 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Brightness quantiles
    `SELECT
      quantileExactWeighted(0.25)(double1, _sample_interval) as p25,
      quantileExactWeighted(0.5)(double1, _sample_interval) as p50,
      quantileExactWeighted(0.75)(double1, _sample_interval) as p75
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot'${f}`,
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

export function printStartQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Slicer distribution (by blob4)
    `SELECT blob4 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND blob4 != ''${f} GROUP BY name ORDER BY count DESC`,
    // File size buckets (by blob5)
    `SELECT blob5 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND blob5 != ''${f} GROUP BY name ORDER BY count DESC`,
    // Thumbnail adoption (avg double2)
    `SELECT avg(double2) as thumbnail_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // AMS usage (avg double3)
    `SELECT avg(double3) as ams_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // Source distribution (by blob7)
    `SELECT blob7 as name, count() as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND blob7 != ''${f} GROUP BY name ORDER BY count DESC`,
  ];
}

export function releasesQueries(versions: string[]): string[] {
  const dataset = "helixscreen_telemetry";
  // Clean version strings — strip 'v' prefix if present for matching
  const cleanVersions = versions.map((v) => v.replace(/^v/, ""));
  const versionList = cleanVersions.map((v) => `'${v}'`).join(", ");
  return [
    // Per-version stats: sessions, crashes, active devices
    `SELECT
      blob2 as ver,
      sumIf(1, index1 = 'session') as total_sessions,
      sumIf(1, index1 = 'crash') as total_crashes,
      countIf(DISTINCT blob1, index1 = 'session') as active_devices
    FROM ${dataset}
    WHERE blob2 IN (${versionList})
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
