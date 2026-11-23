// SPDX-License-Identifier: GPL-3.0-or-later
// A per-device daily cap on the error events written to Analytics Engine, so one
// misbehaving device cannot swamp an error code's totals. The raw batch still
// goes to R2 whole; only the dashboards' copy is capped.
//
// The tally lives in one small R2 object per device per UTC day, read and
// written once per ingest request that carries error events from that device.
// Concurrent requests can race on it, so the cap is approximate by a few events.

import { isDeviceId } from "./devices";
import type { EventRecord } from "./schema";

/** Error events one device may add to Analytics Engine in a UTC day. */
export const ERROR_EVENTS_PER_DEVICE_PER_DAY = 100;

/** Event types the cap applies to. */
export const CAPPED_EVENTS: ReadonlySet<string> = new Set(["error_encountered"]);

/** What a device has sent today: events kept, and events dropped over the cap. */
export interface DeviceDayTally {
  stored: number;
  dropped: number;
}

export interface TallyStore {
  get(key: string): Promise<DeviceDayTally | null>;
  put(key: string, tally: DeviceDayTally): Promise<void>;
}

export const tallyKey = (day: string, deviceId: string): string => `caps/errors/${day}/${deviceId}.json`;

/** The Worker's R2 bucket as a TallyStore. */
export function r2TallyStore(bucket: R2Bucket): TallyStore {
  return {
    async get(key) {
      const obj = await bucket.get(key);
      if (!obj) return null;
      const v = JSON.parse(await obj.text()) as Partial<DeviceDayTally> | null;
      return { stored: Number(v?.stored) || 0, dropped: Number(v?.dropped) || 0 };
    },
    async put(key, tally) {
      await bucket.put(key, JSON.stringify(tally), { httpMetadata: { contentType: "application/json" } });
    },
  };
}

export interface CapResult {
  /** The events to write to Analytics Engine, in their original order. */
  keep: EventRecord[];
  /** Capped events left out of `keep`. */
  dropped: number;
}

/**
 * Applies the daily cap to a batch. A store failure keeps every event: losing
 * the cap for one request is better than losing data.
 */
export async function applyErrorCap(
  events: EventRecord[],
  day: string,
  store: TallyStore,
  cap = ERROR_EVENTS_PER_DEVICE_PER_DAY,
): Promise<CapResult> {
  const idOf = (e: EventRecord): string | null => {
    const id = e.device_id;
    return CAPPED_EVENTS.has(String(e.event)) && typeof id === "string" && isDeviceId(id) ? id : null;
  };

  const incoming = new Map<string, number>();
  for (const e of events) {
    const id = idOf(e);
    if (id) incoming.set(id, (incoming.get(id) ?? 0) + 1);
  }

  const room = new Map<string, number>();
  const tallies = new Map<string, DeviceDayTally>();
  for (const [id, count] of incoming) {
    try {
      const tally = (await store.get(tallyKey(day, id))) ?? { stored: 0, dropped: 0 };
      const free = Math.max(0, cap - tally.stored);
      room.set(id, free);
      tallies.set(id, { stored: tally.stored + Math.min(count, free), dropped: tally.dropped + Math.max(0, count - free) });
    } catch {
      room.set(id, count);
    }
  }

  const keep: EventRecord[] = [];
  let dropped = 0;
  for (const e of events) {
    const id = idOf(e);
    const free = id ? room.get(id) ?? 0 : 0;
    if (id && free > 0) {
      room.set(id, free - 1);
      keep.push(e);
    } else if (id) {
      dropped++;
    } else {
      keep.push(e);
    }
  }

  for (const [id, tally] of tallies) {
    try {
      await store.put(tallyKey(day, id), tally);
    } catch {
      // The tally is advisory; a failed write only loosens the cap.
    }
  }
  return { keep, dropped };
}
