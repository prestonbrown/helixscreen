// SPDX-License-Identifier: GPL-3.0-or-later
// SQL query builders for Analytics Engine dashboard endpoints. Every column is
// named through the schema table (col), never by position.

import {
  EVENT_FANOUT,
  FILTER_KEYS,
  MAX_SLOT,
  POINT_KINDS,
  col,
  hasCol,
  hasKind,
  type FilterKey,
  type NotApplicable,
} from "./schema";
import { isDeviceId } from "./devices";
import type { HealthRowKey } from "./health";

export interface QueryConfig {
  accountId: string;
  apiToken: string;
}

export interface FilterParams {
  platform?: string;
  version?: string;
  model?: string;
  board?: string;
}

/**
 * Sanitize a filter value for interpolation into a single-quoted SQL literal.
 * Keeps spaces, letters, digits and . - _ ( ) / + (what printer model names
 * and versions contain); drops everything else, including quotes, backslashes
 * and semicolons.
 */
export function sanitizeFilterValue(val: string): string {
  return val.replace(/[^a-zA-Z0-9 .\-_()/+]/g, "");
}

// ---------- Weighted aggregates ----------

// Analytics Engine samples rows under load and records how many rows each
// stored row stands for in _sample_interval. Counts, sums and averages over
// raw rows weight by it, or they undercount whatever was sampled.
const SAMPLE_WEIGHT = "_sample_interval";

/**
 * The weight column a per-device subquery exposes: the sample interval of the
 * device's latest row. It takes its own name because aliasing an aggregate to
 * the column it aggregates is ambiguous in ClickHouse SQL.
 */
const DEVICE_WEIGHT = "device_weight";
const deviceWeight = `argMax(${SAMPLE_WEIGHT}, timestamp) as ${DEVICE_WEIGHT}`;

/** The weight column deltaSource exposes: the sample interval of one deduplicated snapshot row. */
const DELTA_WEIGHT = "delta_weight";

export function wCount(weight = SAMPLE_WEIGHT): string {
  return `sum(${weight})`;
}

export function wSum(expr: string, weight = SAMPLE_WEIGHT): string {
  return `sum((${expr}) * ${weight})`;
}

export function wAvg(expr: string, weight = SAMPLE_WEIGHT): string {
  return `${wSum(expr, weight)} / ${wCount(weight)}`;
}

export function wCountIf(cond: string, weight = SAMPLE_WEIGHT): string {
  return `sumIf(${weight}, ${cond})`;
}

// ---------- Filters ----------

export type { FilterKey };

/**
 * Update events carry two versions. The one a version filter, or a per-version
 * table, means is the version the device was running when it sent the event.
 */
const RUNNING_VERSION_FIELD: Record<string, string> = {
  update_failed: "from_version",
  update_success: "version",
};

function runningVersionField(kind: string): string {
  return RUNNING_VERSION_FIELD[kind] ?? "app_version";
}

/** The schema field each dashboard filter reads on a given point kind. */
const FILTER_FIELD: Record<FilterKey, (kind: string) => string> = {
  platform: () => "app_platform",
  version: runningVersionField,
  // Only session and hardware_profile report the detected printer model.
  model: () => "printer_model",
  board: () => "app_board",
};

/** The values one filter parameter names: split on commas, trimmed, sanitized, empties dropped. */
export function filterValues(raw?: string): string[] {
  if (!raw) return [];
  return raw
    .split(",")
    .map((v) => sanitizeFilterValue(v.trim()))
    .filter(Boolean);
}

/** Analytics Engine rejects a bare 0/1 as a boolean ("cannot combine the Boolean and Integer types"), so a constant predicate is a comparison. */
export const SQL_FALSE = "1 = 0";
export const SQL_TRUE = "1 = 1";
const EXCLUDED = ` AND ${SQL_FALSE}`;

/**
 * The filter clause for one point kind: " AND <column> IN (...)" per filter,
 * each on that kind's own column. A kind that lacks a filtered field gets
 * " AND 1 = 0": it cannot be matched against a column that means something else.
 */
export function filterFor(kind: string, filters?: FilterParams): string {
  if (!hasKind(kind)) throw new Error(`unknown point kind: ${kind}`);
  let clause = "";
  for (const key of FILTER_KEYS) {
    const values = filterValues(filters?.[key]);
    if (values.length === 0) continue;
    const field = FILTER_FIELD[key](kind);
    if (!hasCol(kind, field)) return EXCLUDED;
    clause += ` AND ${col(kind, field)} IN (${values.map((v) => `'${v}'`).join(", ")})`;
  }
  return clause;
}

/**
 * The active filter that leaves none of `kinds` in the query, because no kind
 * has the field it filters on, or null when at least one kind can match. A
 * value read from those kinds then means "not reported per this dimension",
 * not "nothing reported yet".
 */
export function filterNotApplicable(kinds: string[], filters?: FilterParams): FilterKey | null {
  for (const key of FILTER_KEYS) {
    if (filterValues(filters?.[key]).length === 0) continue;
    if (kinds.every((kind) => !hasCol(kind, FILTER_FIELD[key](kind)))) return key;
  }
  return null;
}

/** The point kinds behind each payload block that a filter can leave unreported. */
export const BLOCK_KINDS = {
  crashes: ["crash"],
  errors: ["error_encountered"],
  prints: ["print_outcome"],
  updates: ["update_failed", "update_success"],
  performance: ["performance_snapshot", "memory_snapshot"],
} as const satisfies Record<string, readonly string[]>;
export type Block = keyof typeof BLOCK_KINDS;

/** filterNotApplicable for each named block, as a payload's `not_applicable` field. */
export function notApplicableFor<K extends Block>(blocks: readonly K[], filters?: FilterParams): NotApplicable<K> {
  return Object.fromEntries(
    blocks.map((b) => [b, filterNotApplicable([...BLOCK_KINDS[b]], filters)]),
  ) as NotApplicable<K>;
}

/** True when the active filters name a field this kind does not have, so none of its rows can match. */
export function excludedBy(kind: string, filters?: FilterParams): boolean {
  return filterFor(kind, filters) === EXCLUDED;
}

/**
 * The filter clause for a query over several point kinds, whose columns differ:
 * " AND ((index1 = 'a' AND ...) OR (index1 = 'b' AND ...))". A kind the
 * filters exclude drops out of the OR; when every kind does, " AND 1 = 0". With
 * no active filter it is the plain " AND index1 IN ('a', 'b')".
 */
export function filterForKinds(kinds: string[], filters?: FilterParams): string {
  const clauses = kinds.map((kind) => [kind, filterFor(kind, filters)] as const);
  if (clauses.every(([, f]) => f === "")) {
    return ` AND index1 IN (${kinds.map((k) => `'${k}'`).join(", ")})`;
  }
  const branches = clauses.filter(([, f]) => f !== EXCLUDED).map(([kind, f]) => `(index1 = '${kind}'${f})`);
  return branches.length === 0 ? EXCLUDED : ` AND (${branches.join(" OR ")})`;
}

/** Every point kind a device event writes, in table order. */
export const EVENT_KINDS: string[] = Object.keys(POINT_KINDS).filter((kind) =>
  Object.values(EVENT_FANOUT).some((kinds) => kinds.includes(kind)),
);

/**
 * The one column a field occupies across several kinds, for a query that reads
 * it without knowing which kind a row is. Throws when the kinds disagree.
 */
function sharedCol(kinds: string[], fieldFor: (kind: string) => string): string {
  const cols = new Set(kinds.map((kind) => col(kind, fieldFor(kind))));
  if (cols.size !== 1) {
    throw new Error(`kinds ${kinds.join(", ")} store this field in different columns: ${[...cols].join(", ")}`);
  }
  return [...cols][0];
}

const DEVICE_ID = sharedCol(EVENT_KINDS, () => "device_id");

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

const AE_DATE_RE = /^\d{4}-\d{2}-\d{2}$/;

/** True for a real calendar date written as YYYY-MM-DD. */
export function isIsoDate(date: string): boolean {
  if (!AE_DATE_RE.test(date)) return false;
  const parsed = new Date(`${date}T00:00:00Z`);
  return !isNaN(parsed.getTime()) && parsed.toISOString().slice(0, 10) === date;
}

/**
 * Per-kind row counts for one UTC day of Analytics Engine, for reconciling
 * against R2. `rows` is the raw count() next to `weighted`, the
 * _sample_interval-weighted count: their gap is how much AE sampled away, and
 * that comparison is this query's whole purpose. The date is validated before
 * it reaches the SQL string.
 */
export function aeCountsQuery(date: string): string {
  if (!isIsoDate(date)) {
    throw new Error(`date must be YYYY-MM-DD, got ${JSON.stringify(date)}`);
  }
  return `SELECT
      index1 AS kind,
      count() AS row_count,
      sum(_sample_interval) AS weighted,
      count(DISTINCT ${DEVICE_ID}) AS devices
    FROM helixscreen_telemetry
    WHERE toDate(timestamp) = '${date}'
    GROUP BY kind
    ORDER BY kind`;
}

// SQL query builders for each dashboard endpoint

/** Column lookup bound to one point kind. */
const colsOf =
  (kind: string) =>
  (field: string): string =>
    col(kind, field);

const session = colsOf("session");
const printOutcome = colsOf("print_outcome");
const crash = colsOf("crash");
const memSnap = colsOf("memory_snapshot");
const memWarn = colsOf("memory_warning");
const hardware = colsOf("hardware_profile");
const settings = colsOf("settings_snapshot");
const errorEnc = colsOf("error_encountered");
const printStart = colsOf("print_start_context");
const perf = colsOf("performance_snapshot");
const settingsChange = colsOf("settings_change");
const cdnFleet = colsOf("cdn_fleet_daily");
const updateFailed = colsOf("update_failed");

const DATASET = "helixscreen_telemetry";

/** "timestamp >= NOW() - INTERVAL 'N' DAY": the query window on AE write time. */
const inRange = (days: number) => `timestamp >= NOW() - INTERVAL '${days}' DAY`;

/**
 * Distinct devices per non-empty value of one field of one kind, as
 * (name, count) rows, most devices first.
 */
function distinctDevicesBy(kind: string, field: string, days: number, filters?: FilterParams, limit = ""): string {
  const c = colsOf(kind);
  return `SELECT ${c(field)} as name, count(DISTINCT ${c("device_id")}) as count FROM ${DATASET} WHERE ${inRange(days)} AND index1 = '${kind}' AND ${c(field)} != ''${filterFor(kind, filters)} GROUP BY name ORDER BY count DESC${limit}`;
}

// ---------- Delta-era sources ----------

/** Delta-era client events carry this schema_version or later. */
const DELTA_SCHEMA = 3;

const DELTA_KEY = ["device_id", "boot_id", "snapshot_seq"];

/**
 * Point kinds that report per-window deltas: they carry the idempotency key
 * (device_id, boot_id, snapshot_seq) and a schema_version to gate on.
 */
export const DELTA_KINDS: readonly string[] = Object.keys(POINT_KINDS).filter((kind) =>
  [...DELTA_KEY, "schema_version"].every((field) => hasCol(kind, field)),
);

/** The kinds the panel_usage event writes; a device with any of them reported UI activity. */
const PANEL_EVENT_KINDS = EVENT_FANOUT.panel_usage;

/**
 * The delta kinds each dashboard endpoint reads, and so the kinds its
 * `delta_era_devices` count covers.
 */
export const ENDPOINT_DELTA_KINDS = {
  engagement: PANEL_EVENT_KINDS,
  ux: ["panel_usage"],
  features: PANEL_EVENT_KINDS,
  reliability: ["connection_stability"],
  stability: ["connection_stability"],
} as const satisfies Record<string, readonly string[]>;

/**
 * deltaSource names each metric after its schema field, except a field named
 * after an aggregate function: `count` becomes `visit_count`.
 */
const METRIC_ALIAS: Record<string, string> = { count: "visit_count" };
const metricAlias = (field: string) => METRIC_ALIAS[field] ?? field;

/** The time the snapshot was first written, as deltaSource exposes it. */
const WRITTEN_AT = "written_at";

/**
 * A subquery over the schema-3 rows of one delta kind with resends removed:
 * one row per (device, boot, snapshot_seq, ...dims). A resent batch stores an
 * identical second row, so every other column keeps one copy (argMax on write
 * time). Exposes the key and dims under their field names, each metric double
 * under its field name (see METRIC_ALIAS), `written_at` (first write) and
 * `delta_weight` (the row's _sample_interval). Rows from older clients are
 * cumulative, never deltas, so they are left out rather than summed.
 *
 * A metric is meaningful only at the grain of the dims: with no dims a
 * panel_usage row stands for one snapshot, so session_duration_sec is that
 * window, but time_sec and visits are one arbitrary panel's values.
 */
export function deltaSource(kind: string, dims: string[], days: number, filters?: FilterParams): string {
  if (!DELTA_KINDS.includes(kind)) throw new Error(`${kind} is not a delta kind`);
  const c = colsOf(kind);
  const key = [...DELTA_KEY, ...dims];
  const metrics = POINT_KINDS[kind].columns
    .filter((def) => def.type === "double" && !key.includes(def.field) && def.field !== "schema_version")
    .map((def) => `argMax(${c(def.field)}, timestamp) as ${metricAlias(def.field)}`);
  const select = [
    ...key.map((field) => `${c(field)} as ${field}`),
    ...metrics,
    `min(timestamp) as ${WRITTEN_AT}`,
    `argMax(${SAMPLE_WEIGHT}, timestamp) as ${DELTA_WEIGHT}`,
  ];
  return `SELECT ${select.join(", ")}
      FROM ${DATASET}
      WHERE ${inRange(days)} AND index1 = '${kind}' AND ${c("schema_version")} >= ${DELTA_SCHEMA}${filterFor(kind, filters)}
      GROUP BY ${key.map(c).join(", ")}`;
}

/** Distinct devices with schema-3 rows of any of `kinds` in the window, as `delta_era_devices`. */
function deltaEraDevices(kinds: readonly string[], days: number, filters?: FilterParams): string {
  const schema = sharedCol([...kinds], () => "schema_version");
  return `SELECT count(DISTINCT ${DEVICE_ID}) as delta_era_devices FROM ${DATASET} WHERE ${inRange(days)} AND ${schema} >= ${DELTA_SCHEMA}${filterForKinds([...kinds], filters)}`;
}

/**
 * Average over boots of a deltaSource field summed per boot, for use over
 * deltaSource rows. A boot is a session: each snapshot's session_duration_sec
 * is the window it covers, so a boot's length is the sum over its snapshots.
 * boot_id is random per process start, so it identifies a boot on its own.
 */
function perBootAvg(field: string): string {
  return `${wSum(field, DELTA_WEIGHT)} / count(DISTINCT boot_id)`;
}

export function overviewQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const sessF = filterFor("session", filters);
  const eventsF = filterForKinds(EVENT_KINDS, filters);
  return [
    // Active devices (unique device_ids from sessions)
    `SELECT count(DISTINCT ${session("device_id")}) as active_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF}`,
    // Total events
    `SELECT ${wCount()} as total_events FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${eventsF}`,
    // Crash rate (crashes / sessions)
    `SELECT
      ${wCountIf("index1 = 'crash'")} as crash_count,
      ${wCountIf("index1 = 'session'")} as session_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${filterForKinds(["crash", "session"], filters)}`,
    // Print success rate
    `SELECT
      ${wCountIf(`${printOutcome("outcome")} = 'success'`)} as successes,
      ${wCount()} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${filterFor("print_outcome", filters)}`,
    // Events over time
    `SELECT
      toDate(timestamp) as date,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${eventsF}
    GROUP BY date
    ORDER BY date`,
    // Daily active devices (unique device_ids with sessions per day)
    `SELECT
      toDate(timestamp) as date,
      count(DISTINCT ${session("device_id")}) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF}
    GROUP BY date
    ORDER BY date`,
    // First-seen date per device (for cumulative growth curve)
    `SELECT
      toDate(min(timestamp)) as first_seen,
      ${wCount()} as new_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF}
    GROUP BY ${session("device_id")}`,
    // CDN fleet: distinct sources polling the update manifest per day.
    //
    // Deliberately unfiltered by platform/version/model/board - this kind is
    // written by the daily cron from zone HTTP analytics and carries none of
    // those dimensions, so applying a filter would blank the metric out
    // whenever a dashboard filter is set.
    //
    // `date` is the UTC day described (the kind has no device_id). max()
    // rather than sum() so a re-run of the same day collapses instead of
    // double-counting. The window is widened by two days because the row's own
    // timestamp is the snapshot time, a day after the day it covers.
    `SELECT
      ${cdnFleet("date")} as date,
      max(${cdnFleet("sources")}) as sources,
      max(${cdnFleet("polls")}) as polls,
      max(${cdnFleet("errors")}) as errors
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days + 2}' DAY
      AND index1 = 'cdn_fleet_daily'
      AND ${cdnFleet("channel")} = 'stable'
    GROUP BY date
    ORDER BY date`,
  ];
}

export function adoptionQueries(days: number, filters?: FilterParams): string[] {
  const distribution = (field: string) => distinctDevicesBy("session", field, days, filters);
  return [
    // Platforms
    distribution("app_platform"),
    // Versions
    distribution("app_version"),
    // Printer models
    distribution("printer_model"),
    // Kinematics
    distribution("kinematics"),
    // Boards
    distribution("app_board"),
    // Install sources
    distribution("install_source"),
    // Host firmware mods
    distribution("host_mod"),
  ];
}

export function printsQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("print_outcome", filters);
  const success = wCountIf(`${printOutcome("outcome")} = 'success'`);
  return [
    // Success rate over time
    `SELECT
      toDate(timestamp) as date,
      ${success} / ${wCount()} as rate,
      ${wCount()} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${f}
    GROUP BY date
    ORDER BY date`,
    // By filament type
    `SELECT
      ${printOutcome("filament_type")} as type,
      ${success} / ${wCount()} as success_rate,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome' AND ${printOutcome("filament_type")} != ''${f}
    GROUP BY type
    ORDER BY count DESC`,
    // Average duration
    `SELECT ${wAvg(printOutcome("duration_sec"))} as avg_duration_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${f}`,
    // Outcomes per device and version, attributed to versions and models
    // through the session rows below (prints.ts).
    releasePrintsSql(inRange(days), undefined, filters),
    deviceSessionsSql(0, filters),
  ];
}

export function crashesQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("crash", filters);
  const sessF = filterFor("session", filters);
  return [
    // By version (crash count + session count for rate)
    `SELECT
      ${crash("app_version")} as ver,
      ${wCount()} as crash_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("app_version")} != ''${f}
    GROUP BY ver
    ORDER BY crash_count DESC`,
    // Session counts by version (for crash rate denominator)
    `SELECT
      ${session("app_version")} as ver,
      ${wCount()} as session_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("app_version")} != ''${sessF}
    GROUP BY ver`,
    // By signal
    `SELECT ${crash("signal_name")} as signal, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("signal_name")} != ''${f} GROUP BY signal ORDER BY count DESC`,
    // Average uptime
    `SELECT ${wAvg(crash("uptime_sec"))} as avg_uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${f}`,
    // Crashes by platform (count + unique crashing devices)
    `SELECT
      ${crash("app_platform")} as platform,
      ${wCount()} as crash_count,
      count(DISTINCT ${crash("device_id")}) as device_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY crash_count DESC`,
    // Sessions by platform (for per-platform rate denominator + device count)
    `SELECT
      ${session("app_platform")} as platform,
      ${wCount()} as session_count,
      count(DISTINCT ${session("device_id")}) as device_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("app_platform")} != ''${sessF}
    GROUP BY platform
    ORDER BY session_count DESC`,
    // Trailing 14-day rate: overall crashes (all platforms, including empty).
    // NOTE: the per-platform queries below skip an empty platform, so the sum
    // of per-platform counts may be slightly less than these overall totals if
    // any events arrive with unset platform. In practice our client always sets
    // app_platform, so the gap is zero, but the overall totals are deliberately
    // un-filtered for truthfulness on the gauge.
    `SELECT ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'crash'${f}`,
    // Trailing 14-day rate: overall sessions
    `SELECT ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'session'${sessF}`,
    // Trailing 14-day rate: per-platform crashes
    `SELECT ${crash("app_platform")} as platform, ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'crash' AND ${crash("app_platform")} != ''${f} GROUP BY platform`,
    // Trailing 14-day rate: per-platform sessions
    `SELECT ${session("app_platform")} as platform, ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'session' AND ${session("app_platform")} != ''${sessF} GROUP BY platform`,
  ];
}

export function crashListQuery(days: number, limit: number, filters?: FilterParams): string {
  const dataset = "helixscreen_telemetry";
  return `SELECT
    timestamp,
    ${crash("device_id")} as device_id,
    ${crash("app_version")} as ver,
    ${crash("signal_name")} as sig,
    ${crash("app_platform")} as platform,
    ${crash("uptime_sec")} as uptime_sec
  FROM ${dataset}
  WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${filterFor("crash", filters)}
  ORDER BY timestamp DESC
  LIMIT ${limit}`;
}

export function memoryQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("memory_snapshot", filters);
  const rss = memSnap("rss_kb");
  return [
    // RSS over time (avg/p95/max by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(rss)} as avg_rss_kb,
      quantileExactWeighted(0.95)(${rss}, _sample_interval) as p95_rss_kb,
      max(${rss}) as max_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
    // RSS by platform
    `SELECT
      ${memSnap("app_platform")} as platform,
      ${wAvg(rss)} as avg_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot' AND ${memSnap("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY avg_rss_kb DESC`,
    // VM peak trend (avg by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(memSnap("vm_peak_kb"))} as avg_vm_peak_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
  ];
}

export function memoryWarningQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("memory_warning", filters);
  const level = memWarn("level");
  const rss = memWarn("rss_kb");
  return [
    // Warning count by level
    `SELECT
      ${level} as level,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning' AND ${level} != ''${f}
    GROUP BY level
    ORDER BY count DESC`,
    // Warnings over time (count by date, split by level)
    `SELECT
      toDate(timestamp) as date,
      ${level} as level,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    GROUP BY date, level
    ORDER BY date`,
    // RSS at warning time (avg/max by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(rss)} as avg_rss_kb,
      max(${rss}) as max_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    GROUP BY date
    ORDER BY date`,
    // Warnings by platform
    `SELECT
      ${memWarn("app_platform")} as platform,
      ${wCount()} as count,
      ${wAvg(rss)} as avg_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning' AND ${memWarn("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY count DESC`,
    // Affected devices count
    `SELECT count(DISTINCT ${memWarn("device_id")}) as affected_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}`,
    // Recent warnings list (newest first)
    `SELECT
      timestamp,
      ${memWarn("device_id")} as device_id,
      ${memWarn("app_version")} as version,
      ${memWarn("app_platform")} as platform,
      ${level} as level,
      ${memWarn("reason")} as reason,
      ${memWarn("uptime_sec")} as uptime_sec,
      ${rss} as rss_kb,
      ${memWarn("system_available_mb")} as system_available_mb,
      ${memWarn("growth_5min_kb")} as growth_5min_kb,
      ${memWarn("private_dirty_kb")} as private_dirty_kb,
      ${memWarn("pss_kb")} as pss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    ORDER BY timestamp DESC
    LIMIT 100`,
  ];
}

export function hardwareQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("hardware_profile", filters);
  const dev = hardware("device_id");
  const distribution = (field: string) => distinctDevicesBy("hardware_profile", field, days, filters);
  const capBits = Array.from(
    { length: 13 },
    (_, i) => `      ${wSum(`toUInt32(bitAnd(toUInt32(caps), ${1 << i}) > 0)`, DEVICE_WEIGHT)} as cap_${i},`,
  ).join("\n");
  const fans = hardware("fan_count");
  const sensors = hardware("sensor_count");
  const macros = hardware("macro_count");
  return [
    // Printer models (deduplicated by device_id)
    distribution("printer_model"),
    // Kinematics (deduplicated by device_id)
    distribution("kinematics"),
    // MCU chips (deduplicated by device_id)
    distribution("mcu_chip"),
    // Capabilities bitmask — deduplicate: subquery gets latest row per device, outer query aggregates
    `SELECT
${capBits}
      ${wCount(DEVICE_WEIGHT)} as total
    FROM (
      SELECT ${dev}, argMax(${hardware("capabilities_bitmask")}, timestamp) as caps, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile'${f}
      GROUP BY ${dev}
    )`,
    // Build volume averages (deduplicated: latest per device)
    `SELECT
      ${wAvg("vol_x", DEVICE_WEIGHT)} as avg_vol_x,
      ${wAvg("vol_y", DEVICE_WEIGHT)} as avg_vol_y,
      ${wAvg("vol_z", DEVICE_WEIGHT)} as avg_vol_z
    FROM (
      SELECT ${dev}, argMax(${hardware("build_vol_x")}, timestamp) as vol_x, argMax(${hardware("build_vol_y")}, timestamp) as vol_y, argMax(${hardware("build_vol_z")}, timestamp) as vol_z, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND ${hardware("build_vol_x")} > 0${f}
      GROUP BY ${dev}
    )`,
    // Fan/sensor/macro count averages (deduplicated: latest per device, filter out rows where all three are zero — old mis-mapped data)
    `SELECT
      ${wAvg("fans", DEVICE_WEIGHT)} as avg_fan_count,
      ${wAvg("sensors", DEVICE_WEIGHT)} as avg_sensor_count,
      ${wAvg("macros", DEVICE_WEIGHT)} as avg_macro_count
    FROM (
      SELECT ${dev}, argMax(${fans}, timestamp) as fans, argMax(${sensors}, timestamp) as sensors, argMax(${macros}, timestamp) as macros, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND (${fans} > 0 OR ${sensors} > 0 OR ${macros} > 0)${f}
      GROUP BY ${dev}
    )`,
    // Host RAM distribution (from session events, deduplicated by device_id)
    `SELECT
      ram_mb,
      ${wCount(DEVICE_WEIGHT)} as count
    FROM (
      SELECT ${session("device_id")}, argMax(${session("ram_total_mb")}, timestamp) as ram_mb, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("ram_total_mb")} > 0${filterFor("session", filters)}
      GROUP BY ${session("device_id")}
    )
    GROUP BY ram_mb
    ORDER BY ram_mb`,
    // AMS backend distribution (deduplicated by device_id)
    distribution("ams_type"),
    // helix_macros.cfg adoption: "1" installed, "0" not. Empty is excluded, so
    // the denominator is devices that actually reported, not all devices.
    distribution("has_helix_macros"),
    // Moonraker topology: "1" on this machine, "0" over the network. Only
    // clients new enough to report it are counted (empty is excluded).
    distribution("moonraker_is_local"),
  ];
}

export function engagementQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const widgetPlacement = colsOf("widget_placement");
  const settingsF = filterFor("settings_snapshot", filters);
  const settingsDistribution = (field: string) => distinctDevicesBy("settings_snapshot", field, days, filters);
  const brightness = settings("brightness_pct");
  const panels = deltaSource("panel_usage", ["panel_name"], days, filters);
  const snapshots = deltaSource("panel_usage", [], days, filters);
  const shownPanel = "panel_name != '' AND panel_name != 'home'";
  return [
    // 0: Panel time (exclude "home")
    `SELECT panel_name as panel, ${wSum("time_sec", DELTA_WEIGHT)} as total_time_sec FROM (${panels}) WHERE ${shownPanel} GROUP BY panel ORDER BY total_time_sec DESC`,
    // 1: Panel visits (exclude "home")
    `SELECT panel_name as panel, ${wSum("visits", DELTA_WEIGHT)} as total_visits FROM (${panels}) WHERE ${shownPanel} GROUP BY panel ORDER BY total_visits DESC`,
    // 2: Session duration trend: per-boot average of the time each boot reported that day
    `SELECT
      toDate(${WRITTEN_AT}) as date,
      ${perBootAvg("session_duration_sec")} as avg_session_sec
    FROM (${snapshots})
    GROUP BY date
    ORDER BY date`,
    // 3: Theme distribution (deduplicated by device_id)
    settingsDistribution("theme_name"),
    // 4: Locale distribution (deduplicated by device_id)
    settingsDistribution("locale"),
    // 5: Brightness quantiles
    `SELECT
      quantileExactWeighted(0.25)(${brightness}, _sample_interval) as p25,
      quantileExactWeighted(0.5)(${brightness}, _sample_interval) as p50,
      quantileExactWeighted(0.75)(${brightness}, _sample_interval) as p75
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot'${settingsF}`,
    // 6: Dark vs Light mode distribution (deduplicated by device_id)
    settingsDistribution("theme"),
    // 7: Widget placement (unique per device)
    `SELECT ${widgetPlacement("widget_id")} as widget, count(DISTINCT ${widgetPlacement("device_id")}) as devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'widget_placement' AND ${widgetPlacement("widget_id")} != ''${filterFor("widget_placement", filters)} GROUP BY widget ORDER BY devices DESC`,
    // 8: Widget interactions
    `SELECT widget_id as widget, ${wSum(metricAlias("count"), DELTA_WEIGHT)} as interactions FROM (${deltaSource("widget_interaction", ["widget_id"], days, filters)}) WHERE widget_id != '' GROUP BY widget ORDER BY interactions DESC`,
    // 9: Overlay visits
    `SELECT overlay_name as overlay, ${wSum(metricAlias("count"), DELTA_WEIGHT)} as total_visits FROM (${deltaSource("overlay_visit", ["overlay_name"], days, filters)}) WHERE overlay_name != '' GROUP BY overlay ORDER BY total_visits DESC`,
    // 10: Avg session duration over the whole window
    `SELECT ${perBootAvg("session_duration_sec")} as avg_session_sec FROM (${snapshots})`,
    // 11: Devices on a delta-era client
    deltaEraDevices(ENDPOINT_DELTA_KINDS.engagement, days, filters),
  ];
}

export function reliabilityQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const errF = filterFor("error_encountered", filters);
  const category = errorEnc("category");
  const windows = deltaSource("connection_stability", [], days, filters);
  return [
    // 0: Uptime %: connected time over observed time, per day
    `SELECT
      toDate(${WRITTEN_AT}) as date,
      100 * ${wSum("total_connected_sec", DELTA_WEIGHT)} / (${wSum("total_connected_sec + total_disconnected_sec", DELTA_WEIGHT)} + 0.001) as avg_uptime_pct
    FROM (${windows})
    GROUP BY date
    ORDER BY date`,
    // 1: Disconnects per boot, per day
    `SELECT
      toDate(${WRITTEN_AT}) as date,
      ${perBootAvg("disconnect_count")} as avg_disconnects
    FROM (${windows})
    GROUP BY date
    ORDER BY date`,
    // 2: Longest outage (an outage spanning snapshots keeps its start, so max is the real one)
    `SELECT max(longest_disconnect_sec) as max_disconnect_sec FROM (${windows})`,
    // 3: Top error categories
    `SELECT ${category} as category, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category ORDER BY count DESC LIMIT 20`,
    // 4: Error codes
    `SELECT ${category} as category, ${errorEnc("code")} as code, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category, code ORDER BY count DESC LIMIT 50`,
    // 5: Devices on a delta-era client
    deltaEraDevices(ENDPOINT_DELTA_KINDS.reliability, days, filters),
  ];
}

export function stabilityQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const crashF = filterFor("crash", filters);
  const sessF = filterFor("session", filters);
  const errF = filterFor("error_encountered", filters);
  const category = errorEnc("category");
  const code = errorEnc("code");
  const display = `index1 = 'error_encountered' AND ${category} = 'display'`;
  return [
    // 0: Crash count over time
    `SELECT toDate(timestamp) as date, ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${crashF} GROUP BY date ORDER BY date`,
    // 1: Session count over time (for rate denominator)
    `SELECT toDate(timestamp) as date, ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF} GROUP BY date ORDER BY date`,
    // 2: Crash count by version
    `SELECT ${crash("app_version")} as ver, ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("app_version")} != ''${crashF} GROUP BY ver ORDER BY crash_count DESC`,
    // 3: Session count by version
    `SELECT ${session("app_version")} as ver, ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("app_version")} != ''${sessF} GROUP BY ver`,
    // 4: Crashes by signal
    `SELECT ${crash("signal_name")} as signal, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("signal_name")} != ''${crashF} GROUP BY signal ORDER BY count DESC`,
    // 5: Average uptime before crash
    `SELECT ${wAvg(crash("uptime_sec"))} as avg_uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${crashF}`,
    // 6: Klippy errors and shutdowns over time
    `SELECT toDate(${WRITTEN_AT}) as date, ${wSum("klippy_error_count", DELTA_WEIGHT)} as errors, ${wSum("klippy_shutdown_count", DELTA_WEIGHT)} as shutdowns FROM (${deltaSource("connection_stability", [], days, filters)}) GROUP BY date ORDER BY date`,
    // 7: Memory warning count over time
    `SELECT toDate(timestamp) as date, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${filterFor("memory_warning", filters)} GROUP BY date ORDER BY date`,
    // 8: Error hotspots (categories)
    `SELECT ${category} as category, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category ORDER BY count DESC LIMIT 20`,
    // 9: Error codes detail
    `SELECT ${category} as category, ${code} as code, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category, code ORDER BY count DESC LIMIT 50`,
    // 10: Display anomaly count over time (category='display')
    `SELECT toDate(timestamp) as date, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF} GROUP BY date ORDER BY date`,
    // 11: Display anomaly codes breakdown
    `SELECT ${code} as code, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF} GROUP BY code ORDER BY count DESC LIMIT 20`,
    // 12: Display anomaly by version
    `SELECT ${errorEnc("app_version")} as version, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display} AND ${errorEnc("app_version")} != ''${errF} GROUP BY version ORDER BY count DESC LIMIT 20`,
    // 13: Recent display anomalies with context (for backtrace inspection)
    `SELECT timestamp, ${errorEnc("device_id")} as device_id, ${errorEnc("app_version")} as version, ${errorEnc("app_platform")} as platform, ${code} as code, ${errorEnc("context")} as context, ${errorEnc("uptime_sec")} as uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF} ORDER BY timestamp DESC LIMIT 50`,
    // 14: Display anomaly affected devices count
    `SELECT count(DISTINCT ${errorEnc("device_id")}) as affected_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF}`,
    // 15: Devices on a delta-era client
    deltaEraDevices(ENDPOINT_DELTA_KINDS.stability, days, filters),
    // 16: Crash clusters over the range
    crashClustersSql([0, days], filters),
    // 17: Error codes per version over the range
    errorCodesSql([0, days], filters, true),
  ];
}

export function printStartQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("print_start_context", filters);
  // Weighted row count per value of one print_start_context field, empty excluded.
  const distribution = (field: string) =>
    `SELECT ${printStart(field)} as name, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND ${printStart(field)} != ''${f} GROUP BY name ORDER BY count DESC`;
  return [
    // Slicer distribution
    distribution("slicer"),
    // File size buckets
    distribution("file_size_bucket"),
    // Thumbnail adoption (0-1 flag as a percentage)
    `SELECT ${wAvg(printStart("has_thumbnail"))} * 100 as thumbnail_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // AMS usage (0-1 flag as a percentage)
    `SELECT ${wAvg(printStart("ams_active"))} * 100 as ams_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // Source distribution
    distribution("source"),
  ];
}

export function performanceQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("performance_snapshot", filters);
  const dropped = perf("dropped_frame_count");
  const total = perf("total_frame_count");
  const dev = perf("device_id");
  return [
    // 0: Frame time percentiles over time
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(perf("frame_time_p50_ms"))} as avg_p50,
      ${wAvg(perf("frame_time_p95_ms"))} as avg_p95,
      ${wAvg(perf("frame_time_p99_ms"))} as avg_p99
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
    // 1: Drop rate by platform
    `SELECT
      ${perf("app_platform")} as platform,
      ${wSum(dropped)} as dropped,
      ${wSum(total)} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY dropped DESC`,
    // 2: Drop rate by version over time
    `SELECT
      toDate(timestamp) as date,
      ${perf("app_version")} as version,
      ${wSum(dropped)} as dropped,
      ${wSum(total)} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("app_version")} != ''${f}
    GROUP BY date, version
    ORDER BY date`,
    // 3: Worst panels
    `SELECT
      ${perf("worst_panel")} as panel,
      ${wCount()} as times_worst,
      ${wAvg(perf("worst_panel_p95_ms"))} as avg_p95_ms
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("worst_panel")} != ''${f}
    GROUP BY panel
    ORDER BY times_worst DESC
    LIMIT 20`,
    // 4: Fleet-wide metrics
    `SELECT
      ${wAvg(perf("frame_time_p50_ms"))} as fleet_p50,
      ${wSum(dropped)} as total_dropped,
      ${wSum(total)} as total_frames,
      count(DISTINCT ${dev}) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}`,
    // 5: Devices with high drop rate (>5%)
    `SELECT ${wCount(DEVICE_WEIGHT)} as high_drop_devices
    FROM (
      SELECT ${dev}, ${wSum(dropped)} as dropped, ${wSum(total)} as total, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
      GROUP BY ${dev}
      HAVING total > 0 AND dropped / total > 0.05
    )`,
    // 6: Frame time p95 per platform and version
    `SELECT
      ${perf("app_platform")} as platform,
      ${perf("app_version")} as version,
      ${wAvg(perf("frame_time_p95_ms"))} as p95_frame_ms,
      count(DISTINCT ${dev}) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("app_platform")} != '' AND ${perf("app_version")} != ''${f}
    GROUP BY platform, version`,
    // 7: Worst panel per platform
    `SELECT
      ${perf("app_platform")} as platform,
      ${perf("worst_panel")} as panel,
      ${wCount()} as times_worst,
      ${wAvg(perf("worst_panel_p95_ms"))} as avg_p95_ms
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("app_platform")} != '' AND ${perf("worst_panel")} != ''${f}
    GROUP BY platform, panel
    ORDER BY times_worst DESC`,
    // 8: Average RSS per platform and version
    `SELECT
      ${memSnap("app_platform")} as platform,
      ${memSnap("app_version")} as version,
      ${wAvg(memSnap("rss_kb"))} as avg_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot' AND ${memSnap("app_platform")} != '' AND ${memSnap("app_version")} != ''${filterFor("memory_snapshot", filters)}
    GROUP BY platform, version`,
  ];
}

/** settings_snapshot flags for features that are configured rather than visited. */
export const CONFIGURED_FEATURE_FLAGS = ["spoolman_configured", "theme_changed", "multi_printer"] as const;

/**
 * Feature adoption from overlay and widget deltas: per id, the devices that
 * used it at least once in the window, as (id, devices) rows. The denominator
 * is the delta-era device count (query 2). AE has no UNION, so overlays and
 * widgets are two queries. Queries 3 onward are the configured-feature flags,
 * one per CONFIGURED_FEATURE_FLAGS entry, as ("1"/"0", devices) rows.
 */
export function featuresQueries(days: number, filters?: FilterParams): string[] {
  const usedBy = (kind: string, idField: string) =>
    `SELECT ${idField} as id, count(DISTINCT device_id) as devices FROM (${deltaSource(kind, [idField], days, filters)}) WHERE ${idField} != '' AND ${metricAlias("count")} > 0 GROUP BY id ORDER BY devices DESC`;
  return [
    // 0: Overlays, by stable overlay id
    usedBy("overlay_visit", "overlay_name"),
    // 1: Home widgets, by widget id
    usedBy("widget_interaction", "widget_id"),
    // 2: Devices on a delta-era client
    deltaEraDevices(ENDPOINT_DELTA_KINDS.features, days, filters),
    // 3+: Configured-feature flags; empty (a client too old to send one) is excluded
    ...CONFIGURED_FEATURE_FLAGS.map((flag) => distinctDevicesBy("settings_snapshot", flag, days, filters)),
  ];
}

export function uxInsightsQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const changeF = filterFor("settings_change", filters);
  const setting = settingsChange("setting");
  const panels = deltaSource("panel_usage", ["panel_name"], days, filters);
  const shownPanel = "panel_name != '' AND panel_name != 'home'";
  return [
    // 0: Panel time distribution
    `SELECT panel_name as panel, ${wSum("time_sec", DELTA_WEIGHT)} as total_time_sec
    FROM (${panels})
    WHERE ${shownPanel}
    GROUP BY panel
    ORDER BY total_time_sec DESC`,
    // 1: Panel visit frequency
    `SELECT panel_name as panel, ${wSum("visits", DELTA_WEIGHT)} as total_visits, count(DISTINCT device_id) as devices
    FROM (${panels})
    WHERE ${shownPanel}
    GROUP BY panel
    ORDER BY total_visits DESC`,
    // 2: Settings change frequency
    `SELECT ${setting} as setting, ${wCount()} as change_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND ${setting} != ''${changeF}
    GROUP BY setting
    ORDER BY change_count DESC`,
    // 3: Settings defaults (devices that changed each setting)
    `SELECT ${setting} as setting, count(DISTINCT ${settingsChange("device_id")}) as devices_changed
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND ${setting} != ''${changeF}
    GROUP BY setting
    ORDER BY devices_changed DESC`,
    // 4: Total devices for normalization
    `SELECT count(DISTINCT ${session("device_id")}) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${filterFor("session", filters)}`,
    // 5: Avg session duration: per-boot average
    `SELECT ${perBootAvg("session_duration_sec")} as avg_session_sec
    FROM (${deltaSource("panel_usage", [], days, filters)})`,
    // 6: Settings change rate
    `SELECT ${wCount()} as total_changes, count(DISTINCT ${settingsChange("device_id")}) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change'${changeF}`,
    // 7: Devices on a delta-era client
    deltaEraDevices(ENDPOINT_DELTA_KINDS.ux, days, filters),
  ];
}

/**
 * "<column> IN ('a', 'b')" over the sanitized versions (a leading 'v' is
 * stripped; a version that sanitizes to nothing is dropped), or SQL_FALSE
 * when none remain.
 */
export function versionInClause(column: string, versions: string[]): string {
  const clean = versions.map((v) => sanitizeFilterValue(v.trim().replace(/^v/, ""))).filter(Boolean);
  return clean.length > 0 ? `${column} IN (${clean.map((v) => `'${v}'`).join(", ")})` : SQL_FALSE;
}

const RELEASE_KINDS = ["session", "crash", "update_failed", "update_success"];

/** Versions shown when a release query names none. */
const SCORECARD_TOP_VERSIONS = 8;

/** The most-installed versions among session devices in the last `days` days. */
export function topVersionsQuery(days: number, filters?: FilterParams, limit = SCORECARD_TOP_VERSIONS): string {
  return `SELECT ${session("app_version")} as ver, count(DISTINCT ${session("device_id")}) as devices
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'session' AND ${session("app_version")} != ''${filterFor("session", filters)}
    GROUP BY ver
    ORDER BY devices DESC
    LIMIT ${limit}`;
}

/**
 * The update_failed reason the updater sends when it defers an update because a
 * print is running (src/system/update_checker.cpp). It is not a failure, so the
 * release update failure count leaves it out.
 */
export const UPDATE_DEFERRED_REASON = "print_in_progress";

/**
 * Sessions, crashes and self-update outcomes per version in the window `when`
 * (a timestamp predicate), as rows { ver, sessions, crashes, update_failures,
 * update_successes }. Every kind groups by the version the device was RUNNING:
 * for update_failed that is from_version, because keying on the target would
 * bucket every failure under the newest release and hide which deployed
 * versions are broken (prestonbrown/helixscreen#993).
 */
export function releaseStatsSql(when: string, versions: string[], filters?: FilterParams): string {
  const running = sharedCol(RELEASE_KINDS, runningVersionField);
  const kindList = RELEASE_KINDS.map((k) => `'${k}'`).join(", ");
  const perKind = RELEASE_KINDS.map((k) => `(index1 = '${k}'${filterFor(k, filters)})`).join(" OR ");
  return `SELECT
      ${running} as ver,
      ${wCountIf("index1 = 'session'")} as sessions,
      ${wCountIf("index1 = 'crash'")} as crashes,
      ${wCountIf(`index1 = 'update_failed' AND ${updateFailed("reason")} != '${UPDATE_DEFERRED_REASON}'`)} as update_failures,
      ${wCountIf("index1 = 'update_success'")} as update_successes
    FROM ${DATASET}
    WHERE ${when} AND index1 IN (${kindList}) AND ${versionInClause(running, versions)} AND (${perKind})
    GROUP BY ver`;
}

/**
 * Print outcomes per device and per the version the rows carry, in the window
 * `when`, as rows { device_id, ver, print_successes, print_total }. With
 * `versions`, rows outside them are dropped, but rows with an empty version
 * stay: prints.ts attributes those through deviceSessionsSql.
 */
export function releasePrintsSql(when: string, versions?: string[], filters?: FilterParams): string {
  const ver = printOutcome("app_version");
  const inVersions = versions ? ` AND (${versionInClause(ver, versions)} OR ${ver} = '')` : "";
  return `SELECT
      ${printOutcome("device_id")} as device_id,
      ${ver} as ver,
      ${wCountIf(`${printOutcome("outcome")} = 'success'`)} as print_successes,
      ${wCount()} as print_total
    FROM ${DATASET}
    WHERE ${when} AND index1 = 'print_outcome'${inVersions}${filterFor("print_outcome", filters)}
    GROUP BY device_id, ver`;
}

/** Analytics Engine keeps 90 days of rows. */
const AE_RETENTION_DAYS = 90;

/**
 * Each device's latest session printer model and version before a window
 * ends, `endDaysAgo` days ago (0: now), as rows { device_id, model, ver }.
 * A session is sent only on connect, so a device connected all through a
 * window has none inside it; it cannot change version without restarting,
 * which sends one, so its latest session before the window's end, over all of
 * AE's retention, names the version it ran.
 */
export function deviceSessionsSql(endDaysAgo: number, filters?: FilterParams): string {
  return `SELECT
      ${session("device_id")} as device_id,
      argMax(${session("printer_model")}, timestamp) as model,
      argMax(${session("app_version")}, timestamp) as ver
    FROM ${DATASET}
    WHERE ${windowClause([endDaysAgo, AE_RETENTION_DAYS])} AND index1 = 'session'${filterFor("session", filters)}
    GROUP BY device_id`;
}

/** Per-version release scorecard over the last `days` days. */
export function releaseScorecardQueries(versions: string[], days: number, filters?: FilterParams): string[] {
  const inVersions = (column: string) => versionInClause(column, versions);
  const sessF = filterFor("session", filters);
  return [
    // 0: sessions, crashes, self-update outcomes
    releaseStatsSql(inRange(days), versions, filters),
    // 1: active devices
    `SELECT
      ${session("app_version")} as ver,
      count(DISTINCT ${session("device_id")}) as active_devices
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'session' AND ${inVersions(session("app_version"))}${sessF}
    GROUP BY ver`,
    // 2: print outcomes
    releasePrintsSql(inRange(days), versions, filters),
    // 3: devices that crashed
    `SELECT
      ${crash("app_version")} as ver,
      count(DISTINCT ${crash("device_id")}) as crashing_devices
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'crash' AND ${inVersions(crash("app_version"))}${filterFor("crash", filters)}
    GROUP BY ver`,
    // 4: frame time p95
    `SELECT
      ${perf("app_version")} as ver,
      ${wAvg(perf("frame_time_p95_ms"))} as p95_frame_ms
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'performance_snapshot' AND ${inVersions(perf("app_version"))}${filterFor("performance_snapshot", filters)}
    GROUP BY ver`,
    // 5: RSS p95
    `SELECT
      ${memSnap("app_version")} as ver,
      quantileExactWeighted(0.95)(${memSnap("rss_kb")}, ${SAMPLE_WEIGHT}) as p95_rss_kb
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'memory_snapshot' AND ${inVersions(memSnap("app_version"))}${filterFor("memory_snapshot", filters)}
    GROUP BY ver`,
    // 6: adoption curve, devices per day and version
    `SELECT
      toDate(timestamp) as date,
      ${session("app_version")} as ver,
      count(DISTINCT ${session("device_id")}) as devices
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'session' AND ${inVersions(session("app_version"))}${sessF}
    GROUP BY date, ver
    ORDER BY date`,
    // 7: each device's session version, to attribute print outcomes (2)
    deviceSessionsSql(0, filters),
  ];
}

/** First and last session day per device, for retention cohorts. */
export function retentionQuery(filters?: FilterParams): string {
  return `SELECT
      min(toDate(timestamp)) as first_day,
      max(toDate(timestamp)) as last_day
    FROM ${DATASET}
    WHERE ${inRange(DEVICE_WINDOW_DAYS)} AND index1 = 'session'${filterFor("session", filters)}
    GROUP BY ${session("device_id")}`;
}

// ---------- Devices ----------

/** Devices listed per page of the fleet table. */
const DEVICE_LIST_LIMIT = 200;

/** How far back a device page reads; Analytics Engine keeps 90 days. */
const DEVICE_WINDOW_DAYS = 90;

/** Newest rows kept for one device's timeline. */
const DEVICE_TIMELINE_LIMIT = 500;

export function devicesQueries(days: number, filters?: FilterParams): string[] {
  const sessF = filterFor("session", filters);
  const sessionWhere = `${inRange(days)} AND index1 = 'session'${sessF}`;
  return [
    `SELECT
      ${session("device_id")} as device_id,
      max(timestamp) as last_seen,
      argMax(${session("app_version")}, timestamp) as version,
      argMax(${session("app_platform")}, timestamp) as platform,
      argMax(${session("app_board")}, timestamp) as board,
      argMax(${session("printer_model")}, timestamp) as model,
      ${wCount()} as sessions
    FROM ${DATASET}
    WHERE ${sessionWhere}
    GROUP BY device_id
    ORDER BY last_seen DESC
    LIMIT ${DEVICE_LIST_LIMIT}`,
    `SELECT count(DISTINCT ${session("device_id")}) as total FROM ${DATASET} WHERE ${sessionWhere}`,
    `SELECT ${crash("device_id")} as device_id, ${wCount()} as crashes
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'crash'${filterFor("crash", filters)}
    GROUP BY device_id`,
    `SELECT ${printOutcome("device_id")} as device_id, ${wCount()} as prints
    FROM ${DATASET}
    WHERE ${inRange(days)} AND index1 = 'print_outcome'${filterFor("print_outcome", filters)}
    GROUP BY device_id`,
  ];
}

export function deviceQueries(id: string): string[] {
  if (!isDeviceId(id)) throw new Error("invalid device id");
  // Every slot is selected so decodeRow can read any kind; every event kind keeps
  // device_id in the first blob slot (asserted in the tests), which the filter names directly.
  const slots = Array.from({ length: MAX_SLOT }, (_, i) => `blob${i + 1}`)
    .concat(Array.from({ length: MAX_SLOT }, (_, i) => `double${i + 1}`))
    .join(", ");
  const kinds = EVENT_KINDS.map((k) => `'${k}'`).join(", ");
  return [
    `SELECT timestamp, index1, ${slots}
    FROM ${DATASET}
    WHERE ${inRange(DEVICE_WINDOW_DAYS)} AND index1 IN (${kinds}) AND ${DEVICE_ID} = '${id}'
    ORDER BY timestamp DESC
    LIMIT ${DEVICE_TIMELINE_LIMIT}`,
    `SELECT ${session("app_version")} as version, min(timestamp) as first_seen, max(timestamp) as last_seen
    FROM ${DATASET}
    WHERE ${inRange(DEVICE_WINDOW_DAYS)} AND index1 = 'session' AND ${session("device_id")} = '${id}'
    GROUP BY version`,
  ];
}

// ---------- Health ----------

/** Health windows as [newerDaysAgo, olderDaysAgo]; baseline holds the 28 days before cur and contains prev. */
export const HEALTH_WINDOWS = { cur: [0, 7], prev: [7, 14], baseline: [7, 35] } as const;

export type HealthQueryKey = HealthRowKey;

/** The timestamp predicate for one [newerDaysAgo, olderDaysAgo] window. */
export function windowClause(w: readonly [number, number]): string {
  const [newer, older] = w;
  const from = `timestamp >= NOW() - INTERVAL '${older}' DAY`;
  return newer === 0 ? from : `${from} AND timestamp < NOW() - INTERVAL '${newer}' DAY`;
}

/** Versions shown on the Health tab. */
const HEALTH_TOP_VERSIONS = 8;

/** Days the Health tab's top_versions ranks over: the current window. */
const HEALTH_WINDOW_DAYS = HEALTH_WINDOWS.cur[1];

/** Complete days read for the data-health block: today's partial day, the latest day, and the 7 before it. */
const HEALTH_KIND_DAYS = 9;

/**
 * Crashes grouped by (signal, fault code, version, board) in window `w`:
 * sample-weighted count and distinct devices per cluster, as rows
 * { sig, fault, ver, board, count, devices }.
 */
export function crashClustersSql(w: readonly [number, number], filters?: FilterParams): string {
  return `SELECT
      ${crash("signal_name")} as sig,
      ${crash("fault_code_name")} as fault,
      ${crash("app_version")} as ver,
      ${crash("app_board")} as board,
      ${wCount()} as count,
      count(DISTINCT ${crash("device_id")}) as devices
    FROM ${DATASET}
    WHERE ${windowClause(w)} AND index1 = 'crash'${filterFor("crash", filters)}
    GROUP BY sig, fault, ver, board`;
}

/**
 * Error reports grouped by (category, code), and by version too when
 * `byVersion` is set, in window `w`: sample-weighted count and distinct
 * devices, as rows { category, code, [version,] count, devices }.
 */
export function errorCodesSql(w: readonly [number, number], filters?: FilterParams, byVersion = false): string {
  const version = byVersion ? `\n      ${errorEnc("app_version")} as version,` : "";
  return `SELECT
      ${errorEnc("category")} as category,
      ${errorEnc("code")} as code,${version}
      ${wCount()} as count,
      count(DISTINCT ${errorEnc("device_id")}) as devices
    FROM ${DATASET}
    WHERE ${windowClause(w)} AND index1 = 'error_encountered'${filterFor("error_encountered", filters)}
    GROUP BY category, code${byVersion ? ", version" : ""}`;
}

/**
 * Failed self-updates grouped by (platform, reason) in window `w`, leaving out
 * the deferred-for-print reason: sample-weighted failures and distinct devices,
 * as rows { platform, reason, failures, devices }.
 */
export function updateFailureGroupsQuery(w: readonly [number, number]): string {
  return `SELECT
      ${updateFailed("app_platform")} as platform,
      ${updateFailed("reason")} as reason,
      ${wCount()} as failures,
      count(DISTINCT ${updateFailed("device_id")}) as devices
    FROM ${DATASET}
    WHERE ${windowClause(w)} AND index1 = 'update_failed' AND ${updateFailed("reason")} != '${UPDATE_DEFERRED_REASON}'
    GROUP BY platform, reason`;
}

/**
 * Every Health tab query. `versions` restricts the per-release queries and
 * comes from running `top_versions` first; filters apply per kind, while
 * `cdn_latest` and `kind_days` ignore them.
 */
export function healthQueries(filters?: FilterParams, versions: string[] = []): Record<HealthQueryKey, string> {
  const inVersions = (column: string) => versionInClause(column, versions);
  const sessF = filterFor("session", filters);
  const crashF = filterFor("crash", filters);
  const crashSession = sharedCol(["session", "crash"], () => "app_version");
  const crashSessionDevice = sharedCol(["session", "crash"], () => "device_id");
  const crashSessionKinds = `index1 IN ('session', 'crash')`;
  const crashSessionFilter = `((index1 = 'session'${sessF}) OR (index1 = 'crash'${crashF}))`;

  const actives = (w: readonly [number, number]) =>
    `SELECT count(DISTINCT ${session("device_id")}) as actives FROM ${DATASET} WHERE ${windowClause(w)} AND index1 = 'session'${sessF}`;
  const release = (w: readonly [number, number]) => releaseStatsSql(windowClause(w), versions, filters);
  const releaseDevices = (w: readonly [number, number]) =>
    `SELECT
      ${crashSession} as ver,
      index1 as event,
      count(DISTINCT ${crashSessionDevice}) as devices
    FROM ${DATASET}
    WHERE ${windowClause(w)} AND ${crashSessionKinds} AND ${inVersions(crashSession)} AND ${crashSessionFilter}
    GROUP BY ver, event`;
  const prints = (w: readonly [number, number]) => releasePrintsSql(windowClause(w), versions, filters);
  const printSessions = (w: readonly [number, number]) => deviceSessionsSql(w[0], filters);
  const errors = (w: readonly [number, number]) => errorCodesSql(w, filters);
  const crashes = (w: readonly [number, number]) => crashClustersSql(w, filters);

  const { cur, prev, baseline } = HEALTH_WINDOWS;
  return {
    actives_cur: actives(cur),
    actives_prev: actives(prev),
    top_versions: topVersionsQuery(HEALTH_WINDOW_DAYS, filters, HEALTH_TOP_VERSIONS),
    release_cur: release(cur),
    release_prev: release(prev),
    release_devices_cur: releaseDevices(cur),
    release_devices_prev: releaseDevices(prev),
    prints_cur: prints(cur),
    prints_prev: prints(prev),
    print_sessions_cur: printSessions(cur),
    print_sessions_prev: printSessions(prev),
    errors_cur: errors(cur),
    errors_prev: errors(prev),
    errors_baseline: errors(baseline),
    crashes_cur: crashes(cur),
    crashes_prev: crashes(prev),
    // The newest daily row; the window is wider than a day because the row's
    // timestamp is the snapshot time, after the day it describes.
    cdn_latest: `SELECT ${cdnFleet("date")} as date, max(${cdnFleet("sources")}) as sources
    FROM ${DATASET}
    WHERE ${inRange(HEALTH_KIND_DAYS)} AND index1 = 'cdn_fleet_daily' AND ${cdnFleet("channel")} = 'stable'
    GROUP BY date
    ORDER BY date DESC
    LIMIT 1`,
    kind_days: `SELECT toDate(timestamp) as day, index1 as event, ${wCount()} as count
    FROM ${DATASET}
    WHERE ${inRange(HEALTH_KIND_DAYS)} AND index1 != 'cdn_fleet_daily'
    GROUP BY day, event`,
  };
}
