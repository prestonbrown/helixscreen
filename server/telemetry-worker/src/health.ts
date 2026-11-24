// SPDX-License-Identifier: GPL-3.0-or-later
// Health tab payload types and the row folding that builds them. SQL lives in
// queries.ts. Imports only versions, prints and schema so the dashboard can import
// these types without Worker runtime types.

import { num, str, type NotApplicable, type RawRow } from "./schema";
import { printsByVersion } from "./prints";
import { compareVersions } from "./versions";
import { classifyHangs, splitOf, type ClassifiedHang, type HangRow, type HangSplit, type MemSnapshot } from "./hang_cause";

/** The counts one release's rates are computed from, over one window. */
export interface ReleaseCounts {
  sessions: number;
  crashes: number;
  /** Distinct devices with a session. */
  session_devices: number;
  /** Distinct devices with a crash. */
  crash_devices: number;
  update_failures: number;
  update_successes: number;
  print_successes: number;
  print_total: number;
}

/** A release's rates; each is null when its denominator is 0. */
export interface ReleaseRates {
  /** Share of session devices with no crash, in [0, 1]. */
  crash_free: number | null;
  /** Crashes per session. */
  crash_rate: number | null;
  /** Failed share of self-update attempts by devices running the release. */
  update_failure_rate: number | null;
  /** Successful share of finished prints. */
  print_success: number | null;
}

export interface ReleaseWindow extends ReleaseRates {
  devices: number;
  /** The counts update_failure_rate is computed from. */
  update_failures: number;
  update_successes: number;
}

/** The Health payload blocks a filter can leave unreported. */
export type HealthBlock = "crashes" | "errors" | "prints" | "updates";

export interface HealthPayload {
  fleet: { actives_cur: number; actives_prev: number; cdn_sources: number | null; cdn_date: string | null };
  releases: { version: string; cur: ReleaseWindow; prev: ReleaseWindow }[];
  /**
   * `split` is set on main_loop_hang only. That row's cur, prev and devices count the
   * app-blocked share, so a host out of memory never reads as a rising app bug; `split`
   * has the full breakdown for the current week.
   */
  errors: { category: string; code: string; cur: number; prev: number; devices: number; status: "new" | "rising" | "steady"; split?: HangSplit }[];
  crash_clusters: { signal: string; fault_code: string; version: string; board: string; cur: number; prev: number; devices: number }[];
  data_health: { kind: string; latest: number; median: number; low: boolean }[];
  not_applicable: NotApplicable<HealthBlock>;
}

/** The query keys buildHealth reads; the Worker's healthQueries returns one SQL string per key. */
export type HealthRowKey =
  | "actives_cur" | "actives_prev" | "top_versions"
  | "release_cur" | "release_prev"
  | "release_devices_cur" | "release_devices_prev"
  | "prints_cur" | "prints_prev"
  | "print_sessions_cur" | "print_sessions_prev"
  | "errors_cur" | "errors_prev" | "errors_baseline" | "errors_by_device"
  | "hangs_cur" | "hangs_prev" | "hang_snapshots"
  | "crashes_cur" | "crashes_prev"
  | "cdn_latest" | "kind_days";

/** An error rises when this week is at least this multiple of last week and at least RISING_MIN_COUNT. */
const RISING_FACTOR = 2;
export const RISING_MIN_COUNT = 5;
/** An error reported by fewer distinct devices than this never rises: one device is not a trend. */
export const RISING_MIN_DEVICES = 2;
/** An error never rises when one device accounts for more than this share of its count. */
export const RISING_MAX_DEVICE_SHARE = 0.5;

/** The error row the host-memory split applies to. */
const HANG_KEY = "ui\u0000main_loop_hang";

/** A kind is low when its latest complete day is under this share of its median. */
const LOW_SHARE = 0.5;

/** A kind writing fewer rows a day than this is too sparse for a missing day to mean lost data. */
export const LOW_MIN_MEDIAN = 20;

/** Complete days compared against: the latest plus up to this many before it. */
const MEDIAN_DAYS = 7;

/** Kinds written by cron rather than by devices; their daily count says nothing about ingest. */
const CRON_KINDS = new Set(["cdn_fleet_daily"]);

const ratio = (n: number, d: number): number | null => (d > 0 ? n / d : null);

/** The UTC day `delta` days from `day` (YYYY-MM-DD). */
function shiftDay(day: string, delta: number): string {
  const d = new Date(`${day}T00:00:00Z`);
  d.setUTCDate(d.getUTCDate() + delta);
  return d.toISOString().slice(0, 10);
}

function median(values: number[]): number {
  const s = [...values].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length === 0 ? 0 : s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
}

/**
 * The release rates both the Health and Releases tabs show. With
 * `crashesReported` false the active filters exclude every crash row, so the
 * crash rates are null rather than a clean 0% and 100%.
 */
export function releaseRates(c: ReleaseCounts, crashesReported = true): ReleaseRates {
  const crashFree = ratio(c.session_devices - c.crash_devices, c.session_devices);
  return {
    crash_free: crashesReported && crashFree !== null ? Math.min(1, Math.max(0, crashFree)) : null,
    crash_rate: crashesReported ? ratio(c.crashes, c.sessions) : null,
    update_failure_rate: ratio(c.update_failures, c.update_failures + c.update_successes),
    print_success: ratio(c.print_successes, c.print_total),
  };
}

function releaseWindow(
  ver: string,
  stats: RawRow[],
  devices: RawRow[],
  prints: RawRow[],
  crashesReported: boolean,
): ReleaseWindow {
  const st = stats.find((r) => str(r.ver) === ver);
  const pr = prints.find((r) => str(r.ver) === ver);
  const deviceCount = (event: string) =>
    num(devices.find((r) => str(r.ver) === ver && str(r.event) === event)?.devices);
  const counts: ReleaseCounts = {
    sessions: num(st?.sessions),
    crashes: num(st?.crashes),
    session_devices: deviceCount("session"),
    crash_devices: deviceCount("crash"),
    update_failures: num(st?.update_failures),
    update_successes: num(st?.update_successes),
    print_successes: num(pr?.print_successes),
    print_total: num(pr?.print_total),
  };
  return {
    devices: counts.session_devices,
    update_failures: counts.update_failures,
    update_successes: counts.update_successes,
    ...releaseRates(counts, crashesReported),
  };
}

const STATUS_ORDER = { new: 0, rising: 1, steady: 2 } as const;

const ALL_REPORTED: NotApplicable<HealthBlock> = { crashes: null, errors: null, prints: null, updates: null };

/**
 * Folds the health query rows into the payload. `today` is the UTC day
 * (YYYY-MM-DD) whose partial row the data-health block ignores;
 * `notApplicable` names, per block, a filter its kinds cannot carry.
 */
export function buildHealth(
  rows: Record<Exclude<HealthRowKey, "hang_snapshots">, unknown[]> & { hang_snapshots?: unknown[] },
  today: string,
  notApplicable: NotApplicable<HealthBlock> = ALL_REPORTED,
): HealthPayload {
  const r = (k: HealthRowKey): RawRow[] => ((rows as Record<string, unknown[] | undefined>)[k] ?? []) as RawRow[];
  const crashesReported = notApplicable.crashes === null;
  const printsCur = printsByVersion(r("prints_cur"), r("print_sessions_cur"));
  const printsPrev = printsByVersion(r("prints_prev"), r("print_sessions_prev"));

  const releases = r("top_versions")
    .map((t) => str(t.ver))
    .filter(Boolean)
    .sort((a, b) => compareVersions(b, a))
    .map((version) => ({
      version,
      cur: releaseWindow(version, r("release_cur"), r("release_devices_cur"), printsCur, crashesReported),
      prev: releaseWindow(version, r("release_prev"), r("release_devices_prev"), printsPrev, crashesReported),
    }));

  const errKey = (x: RawRow) => `${str(x.category)}\u0000${str(x.code)}`;
  const errPrev = new Map(r("errors_prev").map((x) => [errKey(x), num(x.count)]));
  const errBaseline = new Set(r("errors_baseline").map(errKey));
  // The busiest device's count per code; a code with no row here has no device over the query's floor.
  const errTopDevice = new Map<string, number>();
  for (const x of r("errors_by_device")) {
    errTopDevice.set(errKey(x), Math.max(errTopDevice.get(errKey(x)) ?? 0, num(x.count)));
  }
  const risesBeyondOneDevice = (key: string, cur: number, devices: number, topDevice: number): boolean =>
    devices >= RISING_MIN_DEVICES && topDevice <= RISING_MAX_DEVICE_SHARE * cur;
  const toHangRow = (x: RawRow): HangRow => {
    const mem = num(x.mem);
    return {
      device: str(x.device),
      at: num(x.at) * 1000,
      memAvailableMb: mem > 0 ? mem - 1 : null,
      pressure: str(x.pressure),
      weight: num(x.weight) || 1,
    };
  };
  const snapshots: MemSnapshot[] = r("hang_snapshots").map((x) => ({
    device: str(x.device),
    at: num(x.at) * 1000,
    availableMb: num(x.available) - 1,
  }));
  const hangsCur = classifyHangs(r("hangs_cur").map(toHangRow), snapshots);
  const hangsPrev = classifyHangs(r("hangs_prev").map(toHangRow), snapshots);
  const appBlocked = (hs: ClassifiedHang[]) => hs.filter((h) => h.cause === "app_blocked");
  const errors = r("errors_cur")
    .filter((x) => num(x.count) > 0)
    .map((x) => {
      const key = errKey(x);
      let cur = num(x.count);
      let prev = errPrev.get(key) ?? 0;
      let devices = num(x.devices);
      let topDevice = errTopDevice.get(key) ?? 0;
      let split: HangSplit | undefined;
      // Without hang rows (the query returned none) the aggregate stands as it is.
      if (key === HANG_KEY && hangsCur.length > 0) {
        const blocked = appBlocked(hangsCur);
        const perDevice = new Map<string, number>();
        for (const h of blocked) perDevice.set(h.device, (perDevice.get(h.device) ?? 0) + h.weight);
        split = splitOf(hangsCur);
        cur = split.app_blocked;
        prev = splitOf(hangsPrev).app_blocked;
        devices = perDevice.size;
        topDevice = Math.max(0, ...perDevice.values());
      }
      const status: HealthPayload["errors"][number]["status"] = !errBaseline.has(key)
        ? "new"
        : cur >= RISING_FACTOR * prev && cur >= RISING_MIN_COUNT && risesBeyondOneDevice(key, cur, devices, topDevice)
          ? "rising"
          : "steady";
      return {
        category: str(x.category),
        code: str(x.code),
        cur,
        prev,
        devices,
        status: split && cur === 0 ? ("steady" as const) : status,
        ...(split ? { split } : {}),
      };
    })
    .sort((a, b) => STATUS_ORDER[a.status] - STATUS_ORDER[b.status] || b.cur - a.cur);

  const clusterKey = (x: RawRow) => [x.sig, x.fault, x.ver, x.board].map(str).join("\u0000");
  const crashPrev = new Map(r("crashes_prev").map((x) => [clusterKey(x), num(x.count)]));
  const crash_clusters = r("crashes_cur")
    .filter((x) => num(x.count) > 0)
    .map((x) => ({
      signal: str(x.sig),
      fault_code: str(x.fault),
      version: str(x.ver),
      board: str(x.board),
      cur: num(x.count),
      prev: crashPrev.get(clusterKey(x)) ?? 0,
      devices: num(x.devices),
    }))
    .sort((a, b) => b.cur - a.cur);

  // Closed days only: latest is today-1, the median covers today-8 .. today-2,
  // and a day with no row counts 0. Rows outside those 8 days are ignored.
  const closedDays = Array.from({ length: MEDIAN_DAYS + 1 }, (_, i) => shiftDay(today, -(i + 1)));
  const countsByKind = new Map<string, Map<string, number>>();
  for (const x of r("kind_days")) {
    const kind = str(x.event);
    const day = str(x.day).slice(0, 10);
    if (CRON_KINDS.has(kind) || !closedDays.includes(day)) continue;
    const byDay = countsByKind.get(kind) ?? new Map<string, number>();
    byDay.set(day, (byDay.get(day) ?? 0) + num(x.count));
    countsByKind.set(kind, byDay);
  }
  const data_health = [...countsByKind]
    .map(([kind, byDay]) => {
      const latest = byDay.get(closedDays[0]) ?? 0;
      const med = median(closedDays.slice(1).map((d) => byDay.get(d) ?? 0));
      return { kind, latest, median: med, low: med >= LOW_MIN_MEDIAN && latest < med * LOW_SHARE };
    })
    .sort((a, b) => a.kind.localeCompare(b.kind));

  const cdn = r("cdn_latest")[0];
  return {
    fleet: {
      actives_cur: num(r("actives_cur")[0]?.actives),
      actives_prev: num(r("actives_prev")[0]?.actives),
      cdn_sources: cdn ? num(cdn.sources) : null,
      cdn_date: cdn ? str(cdn.date).slice(0, 10) : null,
    },
    releases,
    errors,
    crash_clusters,
    data_health,
    not_applicable: notApplicable,
  };
}
