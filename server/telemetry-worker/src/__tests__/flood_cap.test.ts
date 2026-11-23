// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, expect, it } from "vitest";
import { applyErrorCap, purgeOldTallies, tallyKey, type DeviceDayTally, type TallyStore } from "../flood_cap";

const A = "a".repeat(64);
const B = "b".repeat(64);
const DAY = "2026-11-23";

function memoryStore(initial: Record<string, DeviceDayTally> = {}): TallyStore & { data: Map<string, DeviceDayTally> } {
  const data = new Map(Object.entries(initial));
  return {
    data,
    get: async (k) => data.get(k) ?? null,
    put: async (k, v) => {
      data.set(k, v);
    },
  };
}

const err = (device_id: string, code = "parse_error") => ({ event: "error_encountered", device_id, category: "websocket", code });

describe("applyErrorCap", () => {
  it("keeps every event under the cap and tallies them", async () => {
    const store = memoryStore();
    const res = await applyErrorCap([err(A), err(A)], DAY, store, 3);
    expect(res.keep).toHaveLength(2);
    expect(res.dropped).toBe(0);
    expect(store.data.get(tallyKey(DAY, A))).toEqual({ stored: 2, dropped: 0 });
  });

  it("drops the events over the cap and counts them", async () => {
    const store = memoryStore({ [tallyKey(DAY, A)]: { stored: 2, dropped: 1 } });
    const res = await applyErrorCap([err(A), err(A), err(A)], DAY, store, 3);
    expect(res.keep).toHaveLength(1);
    expect(res.dropped).toBe(2);
    expect(store.data.get(tallyKey(DAY, A))).toEqual({ stored: 3, dropped: 3 });
  });

  it("caps each device on its own and passes other events through", async () => {
    const store = memoryStore({ [tallyKey(DAY, A)]: { stored: 3, dropped: 0 } });
    const other = { event: "session", device_id: A };
    const res = await applyErrorCap([err(A), err(B), other], DAY, store, 3);
    expect(res.keep).toEqual([err(B), other]);
    expect(res.dropped).toBe(1);
  });

  it("keeps everything when the store fails", async () => {
    const store: TallyStore = {
      get: async () => {
        throw new Error("r2 down");
      },
      put: async () => {
        throw new Error("r2 down");
      },
    };
    const res = await applyErrorCap([err(A), err(A)], DAY, store, 1);
    expect(res.keep).toHaveLength(2);
    expect(res.dropped).toBe(0);
  });

  it("does not touch the store for a batch with no error events", async () => {
    const store = memoryStore();
    await applyErrorCap([{ event: "session", device_id: A }], DAY, store, 1);
    expect(store.data.size).toBe(0);
  });
});

describe("purgeOldTallies", () => {
  it("deletes days before the keep window across list pages and leaves other prefixes", async () => {
    const keys = new Set(["caps/errors/2026-11-01/x.json", "caps/errors/2026-11-19/x.json", "caps/errors/2026-11-21/x.json", "events/2026/11/01/a.json"]);
    const pages = [["caps/errors/2026-11-01/x.json"], ["caps/errors/2026-11-19/x.json", "caps/errors/2026-11-21/x.json"]];
    const bucket = {
      list: async (o: { cursor?: string }) => {
        const i = o.cursor ? 1 : 0;
        return { objects: pages[i].map((key) => ({ key })), truncated: i === 0, cursor: i === 0 ? "c" : undefined };
      },
      delete: async (ks: string[]) => ks.forEach((k) => keys.delete(k)),
    };
    const n = await purgeOldTallies(bucket as never, undefined, "2026-11-23", 3);
    expect(n).toBe(2);
    expect([...keys].sort()).toEqual(["caps/errors/2026-11-21/x.json", "events/2026/11/01/a.json"]);
  });
});

describe("purgeOldTallies dropped counts", () => {
  it("writes an error_flood point for a stale tally that dropped events, then deletes it", async () => {
    const dev = "e".repeat(64);
    const stale = `caps/errors/2026-11-01/${dev}.json`;
    const quiet = `caps/errors/2026-11-01/${"f".repeat(64)}.json`;
    const bodies: Record<string, string> = { [stale]: '{"stored":100,"dropped":37}', [quiet]: '{"stored":4,"dropped":0}' };
    const deleted: string[] = [];
    const bucket = {
      list: async () => ({ objects: [{ key: stale }, { key: quiet }], truncated: false }),
      get: async (k: string) => ({ text: async () => bodies[k] }),
      delete: async (ks: string[]) => void deleted.push(...ks),
    };
    const points: unknown[] = [];
    await purgeOldTallies(bucket as never, { writeDataPoint: (p: unknown) => void points.push(p) } as never, "2026-11-23", 3);
    expect(points).toEqual([{ indexes: ["error_flood"], blobs: expect.arrayContaining([dev, "2026-11-01"]), doubles: expect.arrayContaining([37]) }]);
    expect(deleted).toEqual([stale, quiet]);
  });
});
