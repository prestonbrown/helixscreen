// SPDX-License-Identifier: GPL-3.0-or-later
// Splits main_loop_hang reports into a host that ran out of memory and an app that
// blocked. Pure; imports only the thresholds so the dashboard can import it.

import { HANG_HOST_MEMORY_LOW_MB, HANG_SNAPSHOT_WINDOW_SEC, HANG_UPLOAD_BATCH_SEC } from "./loop/thresholds";

export type HangCause = "host_memory" | "app_blocked" | "unknown";

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
  host_memory: number;
  app_blocked: number;
  unknown: number;
}

export const emptySplit = (): HangSplit => ({ host_memory: 0, app_blocked: 0, unknown: 0 });

const lowMemory = (availableMb: number): boolean => availableMb < HANG_HOST_MEMORY_LOW_MB;

/**
 * A hang that carries its own memory fields is classified by them. An older hang is
 * classified by the device's snapshot taken last before it, by the same threshold. The
 * stored times say only which upload an event came from, so a snapshot counts when it
 * came from the hang's upload and its uptime is at or below the hang's and at most
 * HANG_SNAPSHOT_WINDOW_SEC under it; the largest such uptime wins. A reboot restarts
 * uptime, so a snapshot of an earlier boot qualifies only when its uptime also lies in
 * that range. Without a qualifying snapshot the cause is unknown. `snapshots` holds the
 * hang's device's snapshots.
 */
export function classifyHang(hang: HangRow, snapshots: readonly MemSnapshot[]): HangCause {
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
