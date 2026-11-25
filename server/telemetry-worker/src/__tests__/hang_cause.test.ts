// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from "vitest";
import { classifyHang, classifyHangs, splitOf, type HangRow, type MemSnapshot, type MemWarning } from "../hang_cause";
import { buildHealth, type HealthRowKey } from "../health";
import { findingsFromHealth } from "../loop/findings";
import {
  HANG_APP_RSS_SHARE,
  HANG_HOST_MEMORY_LOW_MB,
  HANG_SNAPSHOT_WINDOW_SEC,
  HANG_SUSPENDED_STALL_SEC,
  HANG_UPLOAD_BATCH_SEC,
  HANG_WARNING_WINDOW_SEC,
} from "../loop/thresholds";
import { fetchHealth } from "../health_source";
import { afterEach, vi } from "vitest";
import { hangEventsSql, hangSnapshotsSql, hangWarningsSql, windowClause, HEALTH_WINDOWS, SQL_FALSE } from "../queries";
import { col } from "../schema";
import { hangRowOf } from "../health";

const HANG_UPTIME = 100_000;
const T0 = Date.UTC(2026, 10, 21, 12, 0, 0);

const hang = (over: Partial<HangRow> = {}): HangRow => ({
  device: "d1",
  at: T0,
  uptimeSec: HANG_UPTIME,
  memAvailableMb: null,
  pressure: "",
  platform: "",
  appVersion: "",
  context: "",
  mainState: "",
  mainWchan: "",
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
/** A memory warning of the hang's upload taken `secondsBefore` of uptime before the hang; the app holds 50 of the host's 972 MB. */
const warn = (secondsBefore: number, over: Partial<MemWarning> = {}): MemWarning => ({
  device: "d1",
  at: T0,
  uptimeSec: HANG_UPTIME - secondsBefore,
  level: "warning",
  reason: "available_warning",
  rssKb: 50 * 1024,
  totalMb: 972,
  ...over,
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

describe("classifyHang, an older hang beside a memory warning", () => {
  it("is host memory when a warning says the host's available memory ran low", () => {
    const causes: [MemWarning["reason"], MemWarning["level"]][] = [
      ["available_warning", "warning"],
      ["available_critical", "critical"],
      ["sustained_low_available", "critical"],
    ];
    for (const [reason, level] of causes) {
      expect(classifyHang(hang(), [], [warn(600, { reason, level })]), reason).toBe("host_memory");
    }
  });
  it("reads a warning within the window of uptime before the hang, and none beyond it or after it", () => {
    expect(classifyHang(hang(), [], [warn(HANG_WARNING_WINDOW_SEC)])).toBe("host_memory");
    expect(classifyHang(hang(), [], [warn(HANG_WARNING_WINDOW_SEC + 1)])).toBe("unknown");
    expect(classifyHang(hang(), [], [warn(-5)])).toBe("unknown");
  });
  it("reads only warnings of the hang's device and upload", () => {
    expect(classifyHang(hang(), [], [warn(60, { at: T0 + HANG_UPLOAD_BATCH_SEC * 1000 })])).toBe("host_memory");
    expect(classifyHang(hang(), [], [warn(60, { at: T0 + (HANG_UPLOAD_BATCH_SEC + 1) * 1000 })])).toBe("unknown");
    expect(classifyHang(hang(), [], [warn(60, { device: "other" })])).toBe("unknown");
  });
  it("ignores a warning that is not at warning or critical, or that is about the app's own RSS", () => {
    expect(classifyHang(hang(), [], [warn(60, { level: "elevated" })])).toBe("unknown");
    expect(classifyHang(hang(), [], [warn(60, { level: "none" })])).toBe("unknown");
    for (const reason of ["rss_warning", "rss_critical", "growth", "hysteresis", "none", ""]) {
      expect(classifyHang(hang(), [], [warn(60, { reason })]), reason).toBe("unknown");
    }
  });
  it("is unknown without an uptime to place the warning by", () => {
    expect(classifyHang(hang({ uptimeSec: 0 }), [], [warn(60)])).toBe("unknown");
  });
  it("leans on the warning over a snapshot that reads earlier", () => {
    // The snapshot an hour before read 500 MB; the warning ten minutes before says the host ran low.
    expect(classifyHang(hang(), [snap(3000, 500)], [warn(600)])).toBe("host_memory");
  });
  it("falls back to the snapshot rule with no such warning", () => {
    const growth = [warn(60, { level: "elevated", reason: "growth" })];
    expect(classifyHang(hang(), [snap(3000, 5)], growth)).toBe("host_memory");
    expect(classifyHang(hang(), [snap(3000, 500)], growth)).toBe("app_blocked");
    expect(classifyHang(hang(), [], growth)).toBe("unknown");
  });
  it("leaves a hang that carries its own memory fields to them", () => {
    expect(classifyHang(hang({ memAvailableMb: 900 }), [], [warn(60)])).toBe("app_blocked");
  });
  it("is still suspended for an Android app with a warning against it", () => {
    expect(classifyHang(hang({ platform: "android" }), [], [warn(60)])).toBe("suspended");
  });
  it("reads only the hang's own device's warnings", () => {
    expect(classifyHangs([hang()], [], [warn(5, { device: "other" })])[0].cause).toBe("unknown");
  });
});

describe("classifyHang, the app's own share of the host's memory", () => {
  /** A warning `secondsBefore` the hang with the app `mb` MB over the share of a 256 MB host; at the share when `mb` is 0. */
  const atShare = (secondsBefore: number, mb: number, over: Partial<MemWarning> = {}): MemWarning =>
    warn(secondsBefore, { rssKb: HANG_APP_RSS_SHARE * 256 * 1024 + mb * 1024, totalMb: 256, ...over });
  const growth = { level: "elevated", reason: "growth" } as const;

  it("is app blocked, not host memory, when the app held more of the host than the share", () => {
    // By a warning, by a snapshot with the share read from a warning of any level, and by the hang's own memory fields.
    expect(classifyHang(hang(), [], [atShare(600, 10)])).toBe("app_blocked");
    expect(classifyHang(hang(), [snap(600, 5)], [atShare(600, 10, growth)])).toBe("app_blocked");
    expect(classifyHang(hang({ memAvailableMb: 30, pressure: "critical" }), [], [atShare(600, 10)])).toBe("app_blocked");
  });
  it("is host memory when the app held no more than the share", () => {
    expect(classifyHang(hang(), [], [atShare(600, 0)])).toBe("host_memory");
    expect(classifyHang(hang(), [], [atShare(600, 0.5)])).toBe("app_blocked");
    expect(classifyHang(hang(), [snap(600, 5)], [atShare(600, -10, growth)])).toBe("host_memory");
  });
  it("reads the share from the newest warning before the hang", () => {
    expect(classifyHang(hang(), [], [atShare(600, 10), atShare(60, -10, { ...growth })])).toBe("host_memory");
    expect(classifyHang(hang(), [], [atShare(600, -10), atShare(60, 10, { ...growth })])).toBe("app_blocked");
  });
  it("reads the share only from a warning in the window before the hang that carries the host's total", () => {
    const before = { ...growth, uptimeSec: HANG_UPTIME - HANG_WARNING_WINDOW_SEC - 1 };
    expect(classifyHang(hang(), [snap(600, 5)], [atShare(600, 10, before)])).toBe("host_memory");
    expect(classifyHang(hang(), [], [atShare(600, 10, { totalMb: 0 })])).toBe("host_memory");
    expect(classifyHang(hang(), [snap(600, 5)], [atShare(600, 10, { ...growth, totalMb: 0 })])).toBe("host_memory");
  });
  it("stays unknown without evidence of low memory, whatever the app's share", () => {
    expect(classifyHang(hang(), [], [atShare(600, 10, growth)])).toBe("unknown");
  });
  it("keeps a suspended app suspended", () => {
    expect(classifyHang(hang({ platform: "android" }), [], [atShare(600, 10)])).toBe("suspended");
  });
});

describe("classifyHang, an app the system suspended", () => {
  /** Android on a build that reports a backgrounded stall, with memory to spare. */
  const android = (over: Partial<HangRow> = {}): HangRow => hang({ platform: "android", appVersion: "1.2.0-alpha.1", ...over });

  it("is suspended for an Android app of such a build, whatever its memory says", () => {
    expect(classifyHang(android(), [])).toBe("suspended");
    expect(classifyHang(android({ memAvailableMb: 5, pressure: "critical" }), [])).toBe("suspended");
    expect(classifyHang(android(), [snap(5, 5)])).toBe("suspended");
  });
  it.each([
    ["", "suspended"],
    ["1.1.0-rc.4", "suspended"],
    ["1.2.0-alpha.1", "suspended"],
    ["1.2.0-alpha.2", "app_blocked"],
    ["1.2.0-alpha.10", "app_blocked"],
    ["1.2.0-beta.1", "app_blocked"],
    ["1.3.0", "app_blocked"],
  ])("reads an Android app with memory to spare on version %s as %s", (appVersion, cause) => {
    expect(classifyHang(hang({ platform: "android", appVersion, memAvailableMb: 900 }), [])).toBe(cause);
  });
  it("classifies an Android app of a build that reports no backgrounded stall by its evidence, like any other platform", () => {
    const foreground = { platform: "android", appVersion: "1.2.0-alpha.2" };
    expect(classifyHang(hang(foreground), [])).toBe("unknown");
    expect(classifyHang(hang(foreground), [snap(5, 5)])).toBe("host_memory");
    expect(classifyHang(hang(foreground), [], [warn(60)])).toBe("host_memory");
  });
  it("is still suspended for an Android app of any version when the stall ran past the line", () => {
    expect(classifyHang(hang({ platform: "android", appVersion: "1.2.0-alpha.2", context: "stalled_161s", memAvailableMb: 900 }), [])).toBe("suspended");
  });
  it("is suspended on any platform when the stall ran past the line, and not at it", () => {
    const line = HANG_SUSPENDED_STALL_SEC;
    expect(classifyHang(hang({ context: `stalled_${line + 1}s` }), [])).toBe("suspended");
    expect(classifyHang(hang({ platform: "pi", context: "stalled_4071s" }), [])).toBe("suspended");
    // At the line the monitor thread was still sampling, so the memory evidence decides.
    expect(classifyHang(hang({ context: `stalled_${line}s` }), [])).toBe("unknown");
    expect(classifyHang(hang({ context: `stalled_${line}s`, memAvailableMb: 5 }), [])).toBe("host_memory");
  });
  it("is not suspended by a context that names no stall length", () => {
    for (const context of ["", "stalled_s", "161", "stalled_161s ", "resumed_161s"]) {
      expect(classifyHang(hang({ context }), [snap(5, 5)]), JSON.stringify(context)).toBe("host_memory");
    }
  });
});

describe("classifyHang, a stall past the line beside low memory", () => {
  const stalled = `stalled_${HANG_SUSPENDED_STALL_SEC + 10}s`;

  it("is host memory when the hang's own fields say memory was low", () => {
    // A host thrashing out of memory starves the monitor thread too, so a long stall is not a pause.
    expect(classifyHang(hang({ context: stalled, memAvailableMb: 5 }), [])).toBe("host_memory");
    expect(classifyHang(hang({ context: stalled, memAvailableMb: 900, pressure: "critical" }), [])).toBe("host_memory");
  });
  it("is host memory when a warning beside the hang says memory was low", () => {
    expect(classifyHang(hang({ context: stalled }), [], [warn(300)])).toBe("host_memory");
  });
  it("lets the RSS guard turn it to the app's", () => {
    expect(classifyHang(hang({ context: stalled }), [], [warn(300, { rssKb: 110 * 1024, totalMb: 114 })])).toBe("app_blocked");
  });
  it("is suspended when the hang's own fields say memory was not low", () => {
    expect(classifyHang(hang({ context: stalled, memAvailableMb: 900 }), [])).toBe("suspended");
  });
  it("is suspended with no evidence but the stall", () => {
    expect(classifyHang(hang({ context: stalled }), [])).toBe("suspended");
    expect(classifyHang(hang({ context: stalled }), [], [warn(300, { level: "elevated", reason: "growth" })])).toBe("suspended");
  });
  it("is suspended over a low snapshot, which may be an hour old", () => {
    expect(classifyHang(hang({ context: stalled }), [snap(3000, 5)])).toBe("suspended");
  });
});

describe("splitOf", () => {
  it("sums sample weights per cause", () => {
    const hs = classifyHangs(
      [
        hang({ memAvailableMb: 1, weight: 3 }),
        hang({ memAvailableMb: 500 }),
        hang({ device: "d2" }),
        hang({ platform: "android", weight: 2 }),
      ],
      [],
    );
    expect(splitOf(hs)).toEqual({ suspended: 2, host_memory: 3, app_blocked: 1, unknown: 1 });
  });
});

const KEYS: HealthRowKey[] = [
  "actives_cur", "actives_prev", "top_versions", "release_cur", "release_prev",
  "release_devices_cur", "release_devices_prev", "prints_cur", "prints_prev",
  "print_sessions_cur", "print_sessions_prev", "errors_cur", "errors_prev", "errors_baseline",
  "errors_by_device", "hangs_cur", "hangs_prev", "hang_snapshots", "hang_warnings", "crashes_cur", "crashes_prev",
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
    expect(e).toMatchObject({ cur: 3, prev: 1, devices: 3, total: 10, split: { suspended: 0, host_memory: 7, app_blocked: 3, unknown: 0 } });
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
  it("carries the plain total, so a week that could not be classified does not read as a clean zero", () => {
    const h = buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 3, devices: 3 }],
        hangs_cur: [hangRow("d0", 0), hangRow("d1", 0), hangRow("d2", 0)],
      }),
      "2026-11-24",
    );
    expect(h.errors[0]).toMatchObject({
      cur: 0,
      prev: 0,
      total: 3,
      split: { suspended: 0, host_memory: 0, app_blocked: 0, unknown: 3 },
    });
  });
  it("counts a suspended hang in the split and the total, and in neither count", () => {
    const h = buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 5, devices: 5 }],
        hangs_cur: [
          hangRow("d0", 501, { platform: "android" }),
          hangRow("d1", 11, { platform: "android" }),
          hangRow("d2", 0, { context: "stalled_161s" }),
          hangRow("d3", 11),
          hangRow("d4", 501),
        ],
      }),
      "2026-11-24",
    );
    expect(h.errors[0]).toMatchObject({
      cur: 1,
      prev: 0,
      devices: 1,
      total: 5,
      status: "new",
      split: { suspended: 3, host_memory: 1, app_blocked: 1, unknown: 0 },
    });
  });
  it("is steady with a zero count when every hang was suspended", () => {
    const h = buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 2, devices: 2 }],
        hangs_cur: [hangRow("d0", 0, { platform: "android" }), hangRow("d1", 0, { context: "stalled_4071s" })],
      }),
      "2026-11-24",
    );
    expect(h.errors[0]).toMatchObject({ cur: 0, devices: 0, total: 2, status: "steady", split: { suspended: 2, app_blocked: 0 } });
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
    expect(h.errors[0].split).toEqual({ suspended: 0, host_memory: 1, app_blocked: 1, unknown: 1 });
  });
  it("classifies an older hang by a memory warning of its upload", () => {
    const h = buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 3, devices: 3 }],
        hangs_cur: [hangRow("a", 0), hangRow("b", 0), hangRow("c", 0)],
        // a's warning says the host ran low, b's only says the app grew, and c sent none.
        hang_warnings: [
          { device: "a", at: T0 / 1000, uptime: HANG_UPTIME - 600, level: "warning", reason: "available_warning", rss: 50 * 1024, total: 972 },
          { device: "b", at: T0 / 1000, uptime: HANG_UPTIME - 600, level: "elevated", reason: "growth", rss: 50 * 1024, total: 972 },
        ],
      }),
      "2026-11-24",
    );
    expect(h.errors[0].split).toEqual({ suspended: 0, host_memory: 1, app_blocked: 0, unknown: 2 });
  });
  it("reads a warning stored before reason codes by its text", () => {
    const h = buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 1, devices: 1 }],
        hangs_cur: [hangRow("a", 0)],
        hang_warnings: [
          {
            device: "a",
            at: T0 / 1000,
            uptime: HANG_UPTIME - 300,
            level: "critical",
            reason: "System available 19MB below critical threshold 32MB",
            rss: 50 * 1024,
            total: 485,
          },
        ],
      }),
      "2026-11-24",
    );
    expect(h.errors[0].split).toEqual({ suspended: 0, host_memory: 1, app_blocked: 0, unknown: 0 });
  });
  it("counts a hang in low memory as the app's, and so in the count, when the app held most of the host", () => {
    const h = buildHealth(
      rows({
        errors_cur: [{ ...hangCode, count: 2, devices: 2 }],
        hangs_cur: [hangRow("a", 0), hangRow("b", 0)],
        // a's app holds 110 of the host's 256 MB; b's holds 50 of 972 MB.
        hang_warnings: [
          { device: "a", at: T0 / 1000, uptime: HANG_UPTIME - 600, level: "warning", reason: "available_warning", rss: 110 * 1024, total: 256 },
          { device: "b", at: T0 / 1000, uptime: HANG_UPTIME - 600, level: "warning", reason: "available_warning", rss: 50 * 1024, total: 972 },
        ],
      }),
      "2026-11-24",
    );
    expect(h.errors[0]).toMatchObject({ cur: 1, devices: 1, total: 2, split: { host_memory: 1, app_blocked: 1, unknown: 0 } });
  });
  it("classifies an older hang by its snapshot alone when no warning rows could be read", () => {
    const r = rows({
      errors_cur: [{ ...hangCode, count: 1, devices: 1 }],
      hangs_cur: [hangRow("a", 0)],
      hang_snapshots: [{ device: "a", at: T0 / 1000, uptime: HANG_UPTIME - 600, available: 12 }],
    });
    Reflect.deleteProperty(r, "hang_warnings");
    expect(buildHealth(r, "2026-11-24").errors[0].split).toEqual({ suspended: 0, host_memory: 1, app_blocked: 0, unknown: 0 });
  });
  it("shows the split in the issue summary and files on the app-blocked count", () => {
    const f = findingsFromHealth(build(7), [], "2026-W47");
    expect(f[0].summary).toMatchObject({
      count: 3,
      hangs_app_blocked: 3,
      hangs_host_out_of_memory: 7,
      hangs_app_suspended: 0,
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
  it("rises over a previous week of suspended hangs, which are classified", () => {
    const suspendedPrev = Array.from({ length: 8 }, (_, i) => hangRow(`p${i}`, 0, { platform: "android" }));
    expect(mk(suspendedPrev, cur).errors[0]).toMatchObject({ cur: 8, prev: 0, status: "rising" });
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
  it("reads the warnings of the named uploads only, sanitized, each upload once", () => {
    const sql = hangWarningsSql([{ device: "a1", at: 1000 }, { device: "a1", at: 1000 }, { device: "b'; DROP", at: 2000 }]);
    expect(sql).toContain("index1 = 'memory_warning'");
    expect(sql.match(/'a1'/g)).toHaveLength(1);
    expect(sql).toContain(`${col("memory_warning", "device_id")} = 'b DROP'`);
    expect(sql).not.toContain("';");
    expect(sql).toContain(`toUnixTimestamp(timestamp) >= ${1000 - HANG_UPLOAD_BATCH_SEC}`);
    expect(sql).toContain(`toUnixTimestamp(timestamp) <= ${2000 + HANG_UPLOAD_BATCH_SEC}`);
    expect(hangWarningsSql([])).toContain(SQL_FALSE);
  });
  it("reads each warning's stored time, uptime, level, reason, the app's RSS and the host's total", () => {
    const sql = hangWarningsSql([{ device: "a1", at: 1000 }]);
    const columns: [string, string][] = [
      ["device_id", "device"],
      ["uptime_sec", "uptime"],
      ["level", "level"],
      ["reason", "reason"],
      ["rss_kb", "rss"],
      ["system_total_mb", "total"],
    ];
    for (const [field, alias] of columns) expect(sql).toContain(`${col("memory_warning", field)} as ${alias}`);
    expect(sql).toContain("toUnixTimestamp(timestamp) as at");
  });
  it("leaves out snapshots written before the column existed", () => {
    expect(hangSnapshotsSql(["a1"], [0, 15])).toContain("double6 > 0");
  });
  it("reads each hang's main thread state and wait channel", () => {
    const sql = hangEventsSql(HEALTH_WINDOWS.cur);
    expect(sql).toContain(`${col("error_encountered", "main_state")} as state`);
    expect(sql).toContain(`${col("error_encountered", "main_wchan")} as wchan`);
  });
  it("reads each hang's platform, version and context", () => {
    const sql = hangEventsSql(HEALTH_WINDOWS.cur);
    expect(sql).toContain(`${col("error_encountered", "app_platform")} as platform`);
    expect(sql).toContain(`${col("error_encountered", "app_version")} as version`);
    expect(sql).toContain(`${col("error_encountered", "context")} as context`);
  });
});

describe("hangRowOf", () => {
  it("reads a hang's main thread state and wait channel, and an absent or empty one as empty", () => {
    const row = { device: "d1", at: T0 / 1000, uptime: HANG_UPTIME, mem: 501, weight: 1 };
    expect(hangRowOf({ ...row, state: "S", wchan: "futex" })).toMatchObject({ mainState: "S", mainWchan: "futex" });
    expect(hangRowOf({ ...row, state: "", wchan: "" })).toMatchObject({ mainState: "", mainWchan: "" });
    expect(hangRowOf(row)).toMatchObject({ mainState: "", mainWchan: "" });
  });
  it("reads a hang's platform, version and context, each empty when the report carries none", () => {
    const row = { device: "d1", at: T0 / 1000, platform: "android", version: "1.1.0", context: "stalled_161s" };
    expect(hangRowOf(row)).toMatchObject({ platform: "android", appVersion: "1.1.0", context: "stalled_161s" });
    expect(hangRowOf({ device: "d1" })).toMatchObject({ platform: "", appVersion: "", context: "" });
  });
});

describe("fetchHealth hang causes", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("splits an Android hang out as suspended from the hang rows alone", async () => {
    vi.stubGlobal("fetch", async (_url: string, init: { body: string }) => {
      const sql = init.body;
      const data = sql.includes("'main_loop_hang'")
        ? [
            { device: "d0", at: 1, uptime: 5, mem: 0, pressure: "", platform: "android", weight: 1 },
            { device: "d1", at: 1, uptime: 5, mem: 0, pressure: "", platform: "pi", weight: 1 },
          ]
        : sql.trimEnd().endsWith("GROUP BY category, code")
          ? [{ category: "ui", code: "main_loop_hang", count: 2, devices: 2 }]
          : [];
      return new Response(JSON.stringify({ data }), { status: 200 });
    });
    const h = await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    expect(h.errors[0]).toMatchObject({ cur: 0, total: 2, split: { suspended: 1, unknown: 1 } });
  });

  it("counts an Android hang of a build that reports no backgrounded stall, once its version reaches the gate", async () => {
    vi.stubGlobal("fetch", async (_url: string, init: { body: string }) => {
      const sql = init.body;
      const data = sql.includes("'main_loop_hang'")
        ? [
            { device: "d0", at: 1, uptime: 5, mem: 501, pressure: "none", platform: "android", version: "1.2.0-alpha.1", weight: 1 },
            { device: "d1", at: 1, uptime: 5, mem: 501, pressure: "none", platform: "android", version: "1.2.0-alpha.2", weight: 1 },
          ]
        : sql.trimEnd().endsWith("GROUP BY category, code")
          ? [{ category: "ui", code: "main_loop_hang", count: 2, devices: 2 }]
          : [];
      return new Response(JSON.stringify({ data }), { status: 200 });
    });
    const h = await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    expect(h.errors[0]).toMatchObject({ cur: 1, devices: 1, total: 2, split: { suspended: 1, app_blocked: 1 } });
  });

  const storedAt = 1_700_000_000;
  /** Answers the Health queries with one older hang, its error row and `warnings` for the warning query; returns each SQL asked. */
  const serve = (warnings: unknown[] | Response): string[] => {
    const asked: string[] = [];
    vi.stubGlobal("fetch", async (_url: string, init: { body: string }) => {
      const sql = init.body;
      asked.push(sql);
      const respond = (data: unknown) => new Response(JSON.stringify({ data }), { status: 200 });
      if (sql.includes("'memory_warning'")) return warnings instanceof Response ? warnings : respond(warnings);
      if (sql.includes("'main_loop_hang'")) {
        return respond([{ device: "d0", at: storedAt, uptime: HANG_UPTIME, mem: 0, pressure: "", platform: "pi", weight: 1 }]);
      }
      if (sql.trimEnd().endsWith("GROUP BY category, code")) {
        return respond([{ category: "ui", code: "main_loop_hang", count: 1, devices: 1 }]);
      }
      return respond([]);
    });
    return asked;
  };

  it("reads the warnings of the upload that hung and classifies an older hang by them", async () => {
    serve([
      { device: "d0", at: storedAt, uptime: HANG_UPTIME - 600, level: "warning", reason: "available_warning", rss: 50 * 1024, total: 972 },
    ]);
    const h = await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    expect(h.errors[0]).toMatchObject({ cur: 0, total: 1, split: { host_memory: 1 } });
  });
  it("moves a hang in low memory to the app's when the warning beside it shows the app holding most of the host", async () => {
    serve([
      { device: "d0", at: storedAt, uptime: HANG_UPTIME - 600, level: "warning", reason: "available_warning", rss: 110 * 1024, total: 256 },
    ]);
    const h = await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    expect(h.errors[0]).toMatchObject({ cur: 1, total: 1, split: { host_memory: 0, app_blocked: 1 } });
  });
  it("asks for each hung upload's warnings around the time that upload was stored", async () => {
    const asked = serve([]);
    await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    const sql = asked.find((b) => b.includes("'memory_warning'"));
    expect(sql).toBeDefined();
    expect(sql).toContain(`${col("memory_warning", "device_id")} = 'd0'`);
    expect(sql).toContain(`toUnixTimestamp(timestamp) >= ${storedAt - HANG_UPLOAD_BATCH_SEC}`);
    expect(sql).toContain(`toUnixTimestamp(timestamp) <= ${storedAt + HANG_UPLOAD_BATCH_SEC}`);
  });
  it("classifies an older hang by its snapshot alone when the warning query fails, and logs", async () => {
    serve(new Response("boom", { status: 500 }));
    const log = vi.spyOn(console, "error").mockImplementation(() => {});
    const h = await fetchHealth({ accountId: "a", apiToken: "t" }, "2026-11-24");
    expect(h.errors[0]).toMatchObject({ cur: 0, total: 1, split: { host_memory: 0, unknown: 1 } });
    expect(log).toHaveBeenCalledWith(expect.stringContaining("hang warnings failed"));
    log.mockRestore();
  });
});
