// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from "vitest";
import { attributePrints, printsByVersion, UNKNOWN_VERSION } from "../prints";
import { buildHealth } from "../health";
import { deviceSessionsSql, filterFor, healthQueries, printsQueries, releasePrintsSql, releaseScorecardQueries } from "../queries";

const sessions = [
  { device_id: "a", model: "Voron 2.4", ver: "1.0.4" },
  { device_id: "b", model: "K1", ver: "1.0.3" },
];

describe("attributePrints", () => {
  it("attributes a row with an empty version to its device's session version", () => {
    const out = attributePrints([{ device_id: "a", ver: "", print_successes: 8, print_total: 10 }], sessions, "ver", UNKNOWN_VERSION);
    expect([...out]).toEqual([["1.0.4", { successes: 8, total: 10 }]]);
  });

  it("keeps a row's own version over the session's", () => {
    const out = attributePrints([{ device_id: "a", ver: "1.0.2", print_successes: 1, print_total: 2 }], sessions, "ver", UNKNOWN_VERSION);
    expect([...out]).toEqual([["1.0.2", { successes: 1, total: 2 }]]);
  });

  it("sums a device's own-version and attributed rows separately", () => {
    const out = attributePrints(
      [
        { device_id: "a", ver: "", print_successes: 3, print_total: 4 },
        { device_id: "b", ver: "", print_successes: 1, print_total: 1 },
        { device_id: "a", ver: "1.0.3", print_successes: 2, print_total: 2 },
      ],
      sessions,
      "ver",
      UNKNOWN_VERSION,
    );
    expect(out.get("1.0.4")).toEqual({ successes: 3, total: 4 });
    expect(out.get("1.0.3")).toEqual({ successes: 3, total: 3 });
  });

  it("puts a device with no session in the window under the fallback", () => {
    const out = attributePrints([{ device_id: "z", ver: "", print_successes: 1, print_total: 4 }], sessions, "ver", UNKNOWN_VERSION);
    expect([...out]).toEqual([[UNKNOWN_VERSION, { successes: 1, total: 4 }]]);
  });

  it("attributes models the same way: print rows carry none", () => {
    const out = attributePrints(
      [
        { device_id: "a", ver: "", print_successes: 8, print_total: 10 },
        { device_id: "z", ver: "1.0.4", print_successes: 1, print_total: 4 },
      ],
      [...sessions, { device_id: "c", model: "", ver: "1.0.4" }],
      "model",
      "Unknown",
    );
    expect(out.get("Voron 2.4")).toEqual({ successes: 8, total: 10 });
    expect(out.get("Unknown")).toEqual({ successes: 1, total: 4 });
  });
});

describe("printsByVersion", () => {
  it("lists versions newest first", () => {
    const rows = printsByVersion(
      [
        { device_id: "x", ver: "1.0.3", print_successes: 1, print_total: 1 },
        { device_id: "x", ver: "1.0.10", print_successes: 1, print_total: 1 },
        { device_id: "x", ver: "1.1.0-beta.2", print_successes: 1, print_total: 1 },
      ],
      [],
    );
    expect(rows.map((r) => r.ver)).toEqual(["1.1.0-beta.2", "1.0.10", "1.0.3"]);
  });

  it("drops the unknown version from per-version rows", () => {
    const rows = printsByVersion(
      [
        { device_id: "a", ver: "", print_successes: "9", print_total: "10" },
        { device_id: "z", ver: "", print_successes: 0, print_total: 50 },
      ],
      sessions,
    );
    expect(rows).toEqual([{ ver: "1.0.4", print_successes: 9, print_total: 10 }]);
  });
});

describe("print attribution SQL", () => {
  it("groups release prints by device and keeps empty-version rows", () => {
    const sql = releasePrintsSql("timestamp >= NOW() - INTERVAL '7' DAY", ["1.0.4"]);
    expect(sql).toContain("GROUP BY device_id, ver");
    expect(sql).toMatch(/IN \('1\.0\.4'\) OR blob\d+ = ''/);
  });

  it("reads all versions when given none", () => {
    expect(releasePrintsSql("timestamp >= NOW() - INTERVAL '7' DAY")).not.toContain("1 = 0");
  });

  it("reads each device's latest session model and version", () => {
    const sql = deviceSessionsSql(0, { platform: "pi" });
    expect(sql).toMatch(/argMax\(blob\d+, timestamp\) as ver/);
    expect(sql).toMatch(/argMax\(blob\d+, timestamp\) as model/);
    expect(sql).toContain("GROUP BY device_id");
    expect(sql).toContain(filterFor("session", { platform: "pi" }));
  });

  it("looks back over all of AE's 90 days, so a device whose only session is before the window still has a version", () => {
    const sql = deviceSessionsSql(0);
    expect(sql).toContain("timestamp >= NOW() - INTERVAL '90' DAY");
    expect(sql).not.toContain("timestamp < ");
    for (const days of [7, 30]) {
      expect(releaseScorecardQueries(["1.0.4"], days)).toContain(deviceSessionsSql(0));
      expect(printsQueries(days)).toContain(deviceSessionsSql(0));
    }
  });

  it("reads no session written after the window ends", () => {
    expect(deviceSessionsSql(7)).toContain("timestamp >= NOW() - INTERVAL '90' DAY AND timestamp < NOW() - INTERVAL '7' DAY");
  });

  it("gives each health window the sessions up to its own end", () => {
    const q = healthQueries(undefined, ["1.0.4"]);
    expect(q.print_sessions_cur).toBe(deviceSessionsSql(0));
    expect(q.print_sessions_prev).toBe(deviceSessionsSql(7));
  });
});

describe("buildHealth print attribution", () => {
  it("attributes each window's prints through that window's sessions", () => {
    const keys = [
      "actives_cur", "actives_prev", "top_versions", "release_cur", "release_prev",
      "release_devices_cur", "release_devices_prev", "prints_cur", "prints_prev",
      "print_sessions_cur", "print_sessions_prev",
      "errors_cur", "errors_prev", "errors_baseline", "errors_by_device", "crashes_cur", "crashes_prev",
      "cdn_latest", "kind_days",
    ];
    const r = Object.fromEntries(keys.map((k) => [k, [] as unknown[]])) as never as Parameters<typeof buildHealth>[0];
    r.top_versions = [{ ver: "1.0.4" }, { ver: "1.0.3" }];
    r.prints_cur = [{ device_id: "a", ver: "", print_successes: 9, print_total: 10 }];
    r.prints_prev = [{ device_id: "a", ver: "", print_successes: 1, print_total: 2 }];
    r.print_sessions_cur = [{ device_id: "a", ver: "1.0.4" }];
    r.print_sessions_prev = [{ device_id: "a", ver: "1.0.3" }];
    const byVer = Object.fromEntries(buildHealth(r, "2026-10-15").releases.map((x) => [x.version, x]));
    expect(byVer["1.0.4"].cur.print_success).toBeCloseTo(0.9);
    expect(byVer["1.0.4"].prev.print_success).toBeNull();
    expect(byVer["1.0.3"].prev.print_success).toBeCloseTo(0.5);
    expect(byVer["1.0.3"].cur.print_success).toBeNull();
  });
});
