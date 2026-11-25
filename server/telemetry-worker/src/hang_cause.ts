// SPDX-License-Identifier: GPL-3.0-or-later
// Splits main_loop_hang reports into their causes: an app the system suspended, a host that ran
// out of memory, an app that blocked, or a report that no evidence classifies. Pure; imports
// only the thresholds and the version order so the dashboard can import it.

import {
  HANG_ANDROID_SUSPENDED_BELOW_VERSION,
  HANG_APP_RSS_SHARE,
  HANG_HOST_MEMORY_LOW_MB,
  HANG_SNAPSHOT_WINDOW_SEC,
  HANG_SUSPENDED_STALL_SEC,
  HANG_UPLOAD_BATCH_SEC,
  HANG_WARNING_WINDOW_SEC,
} from "./loop/thresholds";
import { compareVersions } from "./versions";

export type HangCause = "suspended" | "host_memory" | "app_blocked" | "unknown";

/** One hang report. `memAvailableMb` and `pressure` are set only by builds that report them with the hang. */
export interface HangRow {
  device: string;
  /** Epoch milliseconds the Worker stored the report; every event of one upload shares it. */
  at: number;
  /** The device's uptime when it reported, 0 when absent. */
  uptimeSec: number;
  memAvailableMb: number | null;
  /** The client's pressure level ("none", "elevated", "warning", "critical"), or "" when absent. */
  pressure: string;
  /** The platform the app ran on ("android", "pi", ...), "" when the report carries none. */
  platform: string;
  /** The app's version, "" when the report carries none. */
  appVersion: string;
  /** The report's context, "stalled_<N>s" with N the seconds the main loop had stalled; "" when it carries none. */
  context: string;
  /** The main thread's scheduler state (one letter) and wait channel at the stall; each is "" when the report does not carry it. */
  mainState: string;
  mainWchan: string;
  /** Rows the stored row stands for under sampling. */
  weight: number;
}

/** One device's memory snapshot: host memory available at `at`. */
export interface MemSnapshot {
  device: string;
  /** Epoch milliseconds the Worker stored the snapshot. */
  at: number;
  /** The device's uptime when it took the snapshot, 0 when absent. */
  uptimeSec: number;
  availableMb: number;
}

/**
 * One memory warning: the pressure the device's monitor reported, with the app's and the
 * host's memory at that moment.
 */
export interface MemWarning {
  device: string;
  /** Epoch milliseconds the Worker stored the warning. */
  at: number;
  /** The device's uptime when it reported, 0 when absent. */
  uptimeSec: number;
  /** The level the monitor reported ("elevated", "warning", "critical"), "" when absent. */
  level: string;
  /** The reason as a code ("available_warning", "rss_critical", "growth", ...), "" when absent. */
  reason: string;
  /** The app's resident set, KB; 0 when the row carries none. */
  rssKb: number;
  /** The host's total memory, MB; 0 when the row carries none. */
  totalMb: number;
}

export interface HangSplit {
  suspended: number;
  host_memory: number;
  app_blocked: number;
  unknown: number;
}

export const emptySplit = (): HangSplit => ({ suspended: 0, host_memory: 0, app_blocked: 0, unknown: 0 });

/** Every report a split counts, whatever their causes. */
export const splitTotal = (split: HangSplit): number =>
  split.suspended + split.host_memory + split.app_blocked + split.unknown;

const lowMemory = (availableMb: number): boolean => availableMb < HANG_HOST_MEMORY_LOW_MB;

/** The seconds of stall a hang's context names ("stalled_66s"), 0 when it names none. */
export function stalledSeconds(context: string): number {
  const match = /^stalled_(\d+)s$/.exec(context);
  return match ? Number(match[1]) : 0;
}

/**
 * An Android app of a build that reports the parked event loop of a backgrounded app as a hang.
 * A build of HANG_ANDROID_SUSPENDED_BELOW_VERSION or later does not report that stall, so its
 * Android reports are foreground hangs.
 */
const backgrounded = (hang: HangRow): boolean =>
  hang.platform === "android" && compareVersions(hang.appVersion, HANG_ANDROID_SUSPENDED_BELOW_VERSION) < 0;

/**
 * A stall reported past HANG_SUSPENDED_STALL_SEC: the monitor thread, which samples every 5 s
 * and reports as a stall crosses 60 s, did not run either, so the whole process was stopped.
 */
const longStall = (hang: HangRow): boolean => stalledSeconds(hang.context) > HANG_SUSPENDED_STALL_SEC;

/** A device's row that carries its uptime: the stored time says which upload it came from. */
interface UptimeRow {
  device: string;
  at: number;
  uptimeSec: number;
}

/**
 * `rows` of the hang's device and upload whose uptime lies in the `windowSec` of uptime before
 * the hang's. The stored times say only which upload an event came from, so a row of the hang's
 * upload is one stored within HANG_UPLOAD_BATCH_SEC of it. A reboot restarts uptime, so a row of
 * an earlier boot qualifies only when its uptime also lies in that range.
 */
function beforeHang<T extends UptimeRow>(hang: HangRow, rows: readonly T[], windowSec: number): T[] {
  return rows.filter(
    (r) =>
      r.device === hang.device &&
      Math.abs(r.at - hang.at) <= HANG_UPLOAD_BATCH_SEC * 1000 &&
      r.uptimeSec > 0 &&
      r.uptimeSec <= hang.uptimeSec &&
      r.uptimeSec >= hang.uptimeSec - windowSec,
  );
}

/** The row of the largest uptime, or null when there is none. */
const newest = <T extends UptimeRow>(rows: readonly T[]): T | null =>
  rows.reduce<T | null>((latest, r) => (latest === null || r.uptimeSec > latest.uptimeSec ? r : latest), null);

/** The reasons of a warning at warning or critical that say the host's available memory ran low. */
const LOW_AVAILABLE_REASONS: ReadonlySet<string> = new Set(["available_warning", "available_critical", "sustained_low_available"]);

const lowAvailable = (w: MemWarning): boolean => (w.level === "warning" || w.level === "critical") && LOW_AVAILABLE_REASONS.has(w.reason);

/** True when the app's resident set in `w` is over HANG_APP_RSS_SHARE of its host's memory, which the row must carry. */
const appOverShareIn = (w: MemWarning): boolean => w.totalMb > 0 && w.rssKb > HANG_APP_RSS_SHARE * w.totalMb * 1024;

/**
 * An Android app of a build that reports backgrounded stalls, or any app whose stall ran past
 * HANG_SUSPENDED_STALL_SEC, is `suspended` whatever its memory says. A hang that carries its own
 * memory fields is classified by them: the host ran out of memory when the pressure is critical or
 * less than HANG_HOST_MEMORY_LOW_MB was available. An older hang is the host running out of
 * memory when one of its device's memory warnings in the HANG_WARNING_WINDOW_SEC of uptime
 * before it, at warning or critical, says available memory ran low. With no such warning it is
 * classified by the device's snapshot taken last before it, by the same threshold: among the
 * snapshots of the hang's upload whose uptime is at most HANG_SNAPSHOT_WINDOW_SEC under the
 * hang's, the largest uptime wins. Without a qualifying snapshot the cause is unknown. Low free
 * memory does not say who used it: none of these makes the host the cause when the newest of
 * those warnings shows the app holding more than HANG_APP_RSS_SHARE of the host's memory, since
 * the app then starved its host itself. Every other hang is an app that blocked.
 */
export function classifyHang(
  hang: HangRow,
  snapshots: readonly MemSnapshot[] = [],
  warnings: readonly MemWarning[] = [],
): HangCause {
  if (backgrounded(hang) || longStall(hang)) return "suspended";
  const near = beforeHang(hang, warnings, HANG_WARNING_WINDOW_SEC);
  let host: boolean;
  if (hang.memAvailableMb !== null || hang.pressure !== "") {
    host = hang.pressure === "critical" || (hang.memAvailableMb !== null && lowMemory(hang.memAvailableMb));
  } else if (near.some(lowAvailable)) {
    host = true;
  } else {
    const latest = newest(beforeHang(hang, snapshots, HANG_SNAPSHOT_WINDOW_SEC));
    if (latest === null) return "unknown";
    host = lowMemory(latest.availableMb);
  }
  const latestWarning = newest(near);
  const appOverShare = latestWarning !== null && appOverShareIn(latestWarning);
  return host && !appOverShare ? "host_memory" : "app_blocked";
}

/** A classified hang. */
export interface ClassifiedHang extends HangRow {
  cause: HangCause;
}

/** classifyHang over many hangs, each against its own device's rows. */
export function classifyHangs(hangs: readonly HangRow[], snapshots: readonly MemSnapshot[] = [], warnings: readonly MemWarning[] = []): ClassifiedHang[] {
  return hangs.map((h) => ({ ...h, cause: classifyHang(h, snapshots, warnings) }));
}

export function splitOf(hangs: readonly ClassifiedHang[]): HangSplit {
  const out = emptySplit();
  for (const h of hangs) out[h.cause] += h.weight;
  return out;
}
