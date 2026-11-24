// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from "vitest";
import { classifyHang, classifyHangs, splitOf, type HangRow, type MemSnapshot } from "../hang_cause";
import { buildHealth, type HealthRowKey } from "../health";
import { findingsFromHealth } from "../loop/findings";
import { HANG_HOST_MEMORY_LOW_MB, HANG_SNAPSHOT_WINDOW_SEC, HANG_UPLOAD_BATCH_SEC } from "../loop/thresholds";
import { fetchHealth } from "../health_source";
import { afterEach, vi } from "vitest";
import { hangEventsSql, hangSnapshotsSql, windowClause, HEALTH_WINDOWS, SQL_FALSE } from "../queries";

const HANG_UPTIME = 100_000;
const T0 = Date.UTC(2026, 10, 21, 12, 0, 0);

const hang = (over: Partial<HangRow> = {}): HangRow => ({
  device: "d1",
  at: T0,
  uptimeSec: HANG_UPTIME,
  memAvailableMb: null,
  pressure: "",
  weight: 1,
  ...over,
});
/** A snapshot of the hang's upload taken `secondsBefore` of uptime before the hang. */
const snap = (secondsBefore: number, availableMb: number, device = "d1", atOffsetMs = 0): MemSnapshot => ({
  device,
  at: T0 + atOffsetMs,
  uptimeSec: HANG_UPTIME - secondsBefore,
  availableMb,
});

describe("classifyHang, a hang that carries its own memory", () => {
  it("is host memory below the threshold and app blocked at it", () => {
    expect(classifyHang(hang({ memAvailableMb: HANG_HOST_MEMORY_LOW_MB - 1 }), [])).toBe("host_memory");
    expect(classifyHang(hang({ memAvailableMb: HANG_HOST_MEMORY_LOW_MB }), [])).toBe("app_blocked");
  });
  it("treats 0 MB available as a real reading", () => {
    expect(classifyHang(hang({ memAvailableMb: 0 }), [])).toBe("host_memory");
  });
  it("is host memory at critical pressure however much is free", () => {
    expect(classifyHang(hang({ memAvailableMb: 900, pressure: "critical" }), [])).toBe("host_memory");
    expect(classifyHang(hang({ pressure: "critical" }), [])).toBe("host_memory");
  });
  it("is app blocked at lower pressure with plenty free", () => {
    expect(classifyHang(hang({ memAvailableMb: 900, pressure: "warning" }), [])).toBe("app_blocked");
  });
  it("ignores snapshots", () => {
    expect(classifyHang(hang({ memAvailableMb: 900 }), [snap(1, 5)])).toBe("app_blocked");
  });
});

describe("classifyHang, an older hang", () => {
  const H = 3600;
  it("uses the snapshot of the largest uptime at or below the hang's", () => {
    expect(classifyHang(hang(), [snap(3000, 5), snap(600, 500)])).toBe("app_blocked");
    expect(classifyHang(hang(), [snap(3000, 500), snap(600, 5)])).toBe("host_memory");
    expect(classifyHang(hang(), [snap(600, 5), snap(3000, 500)])).toBe("host_memory");
  });
  it("picks the right hour when a whole day of snapshots shares one stored time", () => {
    // 24 hourly snapshots from one upload, the hang 30 minutes after the 14th; only the 14th and earlier qualify.
    const day = Array.from({ length: 24 }, (_, i) => ({
      device: "d1",
      at: T0,
      uptimeSec: 1000 + i * H,
      availableMb: i === 13 ? 5 : 800,
    }));
    const h = hang({ uptimeSec: 1000 + 13 * H + 1800 });
    expect(classifyHang(h, day)).toBe("host_memory");
    expect(classifyHang(hang({ uptimeSec: 1000 + 14 * H + 1800 }), day)).toBe("app_blocked");
  });
  it("applies the same threshold to the snapshot", () => {
    expect(classifyHang(hang(), [snap(5, HANG_HOST_MEMORY_LOW_MB - 1)])).toBe("host_memory");
    expect(classifyHang(hang(), [snap(5, HANG_HOST_MEMORY_LOW_MB)])).toBe("app_blocked");
  });
  it("is unknown without a snapshot, or with one more than the window of uptime before", () => {
    expect(classifyHang(hang(), [])).toBe("unknown");
    expect(classifyHang(hang(), [snap(HANG_SNAPSHOT_WINDOW_SEC + 1, 5)])).toBe("unknown");
    expect(classifyHang(hang(), [snap(HANG_SNAPSHOT_WINDOW_SEC, 5)])).toBe("host_memory");
  });
  it("ignores a snapshot taken after the hang", () => {
    expect(classifyHang(hang(), [snap(-5, 5)])).toBe("unknown");
  });
  it("ignores a snapshot of another upload, even one of plausible uptime", () => {
    expect(classifyHang(hang(), [snap(60, 5, "d1", (HANG_UPLOAD_BATCH_SEC + 1) * 1000)])).toBe("unknown");
    expect(classifyHang(hang(), [snap(60, 5, "d1", HANG_UPLOAD_BATCH_SEC * 1000)])).toBe("host_memory");
  });
  it("ignores an earlier boot's snapshot whose uptime is above the hang's", () => {
    expect(classifyHang(hang({ uptimeSec: 500 }), [{ device: "d1", at: T0, uptimeSec: 80_000, availableMb: 5 }])).toBe("unknown");
  });
  it("is unknown when the hang reports no uptime", () => {
    expect(classifyHang(hang({ uptimeSec: 0 }), [snap(60, 5)])).toBe("unknown");
  });
  it("reads only the hang's own device", () => {
    expect(classifyHangs([hang()], [snap(5, 5, "other")])[0].cause).toBe("unknown");
  });
});

describe("splitOf", () => {
  it("sums sample weights per cause", () => {
    const hs = classifyHangs(
      [hang({ memAvailableMb: 1, weight: 3 }), hang({ memAvailableMb: 500 }), hang({ device: "d2" })],
      [],
    );
    expect(splitOf(hs)).toEqual({ host_memory: 3, app_blocked: 1, unknown: 1 });
  });
});

const KEYS: HealthRowKey[] = [
  "actives_cur", "actives_prev", "top_versions", "release_cur", "release_prev",
  "release_devices_cur", "release_devices_prev", "prints_cur", "prints_prev",
  "print_sessions_cur", "print_sessions_prev", "errors_cur", "errors_prev", "errors_baseline",
  "errors_by_device", "hangs_cur", "hangs_prev", "hang_snapshots", "crashes_cur", "crashes_prev",
  "cdn_latest", "kind_days",
];
const rows = (over: Partial<Record<HealthRowKey, unknown[]>>) =>
  ({ ...Object.fromEntries(KEYS.map((k) => [k, []])), ...over }) as Record<HealthRowKey, unknown[]>;

const hangRow = (device: string, mem: number, extra: object = {}) => ({
  device,
  at: T0 / 1000,
  uptime: HANG_UPTIME,
  mem,
  pressure: "",
  weight: 1,
  ...extra,
});
const hangCode = { category: "ui", code: "main_loop_hang" };

/** Ten hangs from ten devices: `lowMem` of them with the host out of memory (mem is stored + 1). */
function tenHangs(lowMem: number) {
  return Array.from({ length: 10 }, (_, i) => hangRow(`d${i}`, i < lowMem ? 11 : 501));
}

describe("a main_loop_hang finding", () => {
  const build = (lowMem: number, prevBlocked = 1) =>
    buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 10, devices: 10 }],
        errors_prev: [{ ...hangCode, count: 1, devices: 1 }],
        errors_baseline: [{ ...hangCode, count: 1 }],
        hangs_cur: tenHangs(lowMem),
        hangs_prev: Array.from({ length: prevBlocked }, (_, i) => hangRow(`p${i}`, 501)),
      }),
      "2026-11-24",
    );

  it("counts only the app-blocked share and carries the split", () => {
    const e = build(7).errors[0];
    expect(e).toMatchObject({ cur: 3, prev: 1, devices: 3, split: { host_memory: 7, app_blocked: 3, unknown: 0 } });
  });
  it("does not rise when the host's memory explains the rise", () => {
    expect(build(7).errors[0].status).toBe("steady");
  });
  it("still rises when the app blocked", () => {
    expect(build(0).errors[0].status).toBe("rising");
  });
  it("is steady with a zero count when every hang was the host", () => {
    expect(build(10).errors[0]).toMatchObject({ cur: 0, status: "steady" });
  });
  it("leaves every other error code to the aggregate", () => {
    const h = buildHealth(
      rows({ errors_cur: [{ category: "ui", code: "other", count: 4, devices: 2 }], hangs_cur: tenHangs(10) }),
      "2026-11-24",
    );
    expect(h.errors[0]).toMatchObject({ cur: 4, devices: 2 });
    expect(h.errors[0].split).toBeUndefined();
  });
  it("classifies an older hang by its device's snapshot", () => {
    const h = buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 2, devices: 2 }],
        hangs_cur: [hangRow("a", 0), hangRow("b", 0), hangRow("c", 0)],
        // available is stored + 1: a is at 11 MB, b at 500 MB, c has no snapshot.
        hang_snapshots: [
          { device: "a", at: T0 / 1000, uptime: HANG_UPTIME - 600, available: 12 },
          { device: "b", at: T0 / 1000, uptime: HANG_UPTIME - 600, available: 501 },
        ],
      }),
      "2026-11-24",
    );
    expect(h.errors[0].split).toEqual({ host_memory: 1, app_blocked: 1, unknown: 1 });
  });
  it("shows the split in the issue summary and files on the app-blocked count", () => {
    const f = findingsFromHealth(build(7), [], "2026-W47");
    expect(f[0].summary).toMatchObject({
      count: 3,
      hangs_app_blocked: 3,
      hangs_host_out_of_memory: 7,
      hangs_cause_unknown: 0,
    });
    expect(f[0].actions).not.toContain("issue");
    expect(findingsFromHealth(build(0), [], "2026-W47")[0].actions).toContain("issue");
  });
});

describe("the first weeks after the snapshot column deploys", () => {
  const mk = (prevHangs: object[], curHangs: object[]) =>
    buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: curHangs.length, devices: curHangs.length }],
        errors_prev: [{ ...hangCode, count: prevHangs.length, devices: 1 }],
        errors_baseline: [{ ...hangCode, count: 1 }],
        hangs_cur: curHangs,
        hangs_prev: prevHangs,
      }),
      "2026-11-24",
    );
  // Eight app-blocked hangs this week from eight devices, mem stored + 1.
  const cur = Array.from({ length: 8 }, (_, i) => hangRow(`c${i}`, 501));
  // Last week's hangs carry no memory and no snapshot: unknown.
  const unknownPrev = Array.from({ length: 8 }, (_, i) => hangRow(`p${i}`, 0));

  it("holds the row steady when last week was mostly unclassifiable", () => {
    expect(mk(unknownPrev, cur).errors[0]).toMatchObject({ cur: 8, prev: 0, status: "steady" });
  });
  it("rises when last week was classifiable", () => {
    const classified = [hangRow("p0", 501), ...Array.from({ length: 3 }, (_, i) => hangRow(`q${i}`, 0))];
    // 1 of 4 classified is below the share: held.
    expect(mk(classified, cur).errors[0].status).toBe("steady");
    const mostly = [hangRow("p0", 501), hangRow("p1", 501), hangRow("p2", 0)];
    expect(mk(mostly, cur).errors[0].status).toBe("rising");
  });
  it("rises when there were no hangs last week", () => {
    expect(mk([], cur).errors[0].status).toBe("rising");
  });
});

describe("fetchHealth when the snapshot query fails", () => {
  afterEach(() => vi.unstubAllGlobals());

  const respond = (sql: string): unknown[] => {
    if (sql.includes("'main_loop_hang'")) return Array.from({ length: 6 }, (_, i) => ({ device: `d${i}`, at: 1, uptime: 5, mem: 501, pressure: "", weight: 1 }));
    if (sql.trimEnd().endsWith("GROUP BY category, code")) {
      return [{ category: "ui", code: "main_loop_hang", count: 6, devices: 6 }];
    }
    return [];
  };

  it("reports the plain total and logs", async () => {
    vi.stubGlobal("fetch", async (_url: string, init: { body: string }) => {
      if (init.body.includes("'memory_snapshot'")) return new Response("boom", { status: 500 });
      return new Response(JSON.stringify({ data: respond(init.body) }), { status: 200 });
    });
    const log = vi.spyOn(console, "error").mockImplementation(() => {});
    const h = await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    expect(h.errors[0]).toMatchObject({ code: "main_loop_hang", cur: 6, devices: 6 });
    expect(h.errors[0].split).toBeUndefined();
    expect(log).toHaveBeenCalledWith(expect.stringContaining("hang snapshots failed"));
    log.mockRestore();
  });
  it("splits when the snapshot query succeeds", async () => {
    vi.stubGlobal("fetch", async (_url: string, init: { body: string }) =>
      new Response(JSON.stringify({ data: init.body.includes("'memory_snapshot'") ? [] : respond(init.body) }), { status: 200 }));
    const h = await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    expect(h.errors[0].split).toBeDefined();
  });
});

describe("hang queries", () => {
  it("reads the hang code in the window", () => {
    const sql = hangEventsSql(HEALTH_WINDOWS.cur);
    expect(sql).toContain(windowClause(HEALTH_WINDOWS.cur));
    expect(sql).toContain("'main_loop_hang'");
    expect(sql).toContain("'error_encountered'");
  });
  it("reads snapshots only for the named devices, sanitized", () => {
    const sql = hangSnapshotsSql(["a1", "b'; DROP"], [0, 15]);
    expect(sql).toContain("IN ('a1',");
    expect(sql).not.toContain("DROP TABLE");
    expect(sql).not.toContain("b';");
    expect(hangSnapshotsSql([], [0, 15])).toContain(SQL_FALSE);
  });
  it("leaves out snapshots written before the column existed", () => {
    expect(hangSnapshotsSql(["a1"], [0, 15])).toContain("double6 > 0");
  });
});
