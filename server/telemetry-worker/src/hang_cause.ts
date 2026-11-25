// SPDX-License-Identifier: GPL-3.0-or-later
// Splits main_loop_hang reports into their causes: an app the system suspended, a host that ran
// out of memory, an app that blocked, or a report that no evidence classifies. Pure; imports
// only the thresholds so the dashboard can import it.

import { HANG_HOST_MEMORY_LOW_MB, HANG_SNAPSHOT_WINDOW_SEC, HANG_SUSPENDED_STALL_SEC, HANG_UPLOAD_BATCH_SEC } from "./loop/thresholds";

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
 * An app the system paused, so that its main loop stopping says nothing about the app. Android
 * parks the event loop while the activity is in the background. Anywhere, a stall reported past
 * HANG_SUSPENDED_STALL_SEC means the monitor thread, which samples every 5 s and reports as a
 * stall crosses 60 s, did not run either: the whole process was stopped.
 */
const suspended = (hang: HangRow): boolean =>
  hang.platform === "android" || stalledSeconds(hang.context) > HANG_SUSPENDED_STALL_SEC;

/**
 * A suspended app is `suspended` whatever its memory says. A hang that carries its own memory
 * fields is classified by them. An older hang is classified by the device's snapshot taken
 * last before it, by the same threshold. The stored times say only which upload an event came
 * from, so a snapshot counts when it came from the hang's upload and its uptime is at or below
 * the hang's and at most HANG_SNAPSHOT_WINDOW_SEC under it; the largest such uptime wins. A
 * reboot restarts uptime, so a snapshot of an earlier boot qualifies only when its uptime also
 * lies in that range. Without a qualifying snapshot the cause is unknown. `snapshots` holds the
 * hang's device's snapshots.
 */
export function classifyHang(hang: HangRow, snapshots: readonly MemSnapshot[]): HangCause {
  if (suspended(hang)) return "suspended";
  if (hang.memAvailableMb !== null || hang.pressure !== "") {
    return hang.pressure === "critical" || (hang.memAvailableMb !== null && lowMemory(hang.memAvailableMb))
      ? "host_memory"
      : "app_blocked";
  }
  const oldest = hang.uptimeSec - HANG_SNAPSHOT_WINDOW_SEC;
  let latest: MemSnapshot | null = null;
  for (const s of snapshots) {
    const sameUpload = Math.abs(s.at - hang.at) <= HANG_UPLOAD_BATCH_SEC * 1000;
    if (!sameUpload || s.uptimeSec <= 0 || s.uptimeSec > hang.uptimeSec || s.uptimeSec < oldest) continue;
    if (latest === null || s.uptimeSec > latest.uptimeSec) latest = s;
  }
  return latest === null ? "unknown" : lowMemory(latest.availableMb) ? "host_memory" : "app_blocked";
}

/** A classified hang. */
export interface ClassifiedHang extends HangRow {
  cause: HangCause;
}

export function classifyHangs(hangs: readonly HangRow[], snapshots: readonly MemSnapshot[]): ClassifiedHang[] {
  const byDevice = new Map<string, MemSnapshot[]>();
  for (const s of snapshots) byDevice.set(s.device, [...(byDevice.get(s.device) ?? []), s]);
  return hangs.map((h) => ({ ...h, cause: classifyHang(h, byDevice.get(h.device) ?? []) }));
}

export function splitOf(hangs: readonly ClassifiedHang[]): HangSplit {
  const out = emptySplit();
  for (const h of hangs) out[h.cause] += h.weight;
  return out;
}
