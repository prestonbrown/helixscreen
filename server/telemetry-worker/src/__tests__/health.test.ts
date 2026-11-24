// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from "vitest";
import { buildHealth, LOW_MIN_MEDIAN, releaseRates, type HealthPayload, type ReleaseCounts } from "../health";
import { HEALTH_WINDOWS, filterFor, healthQueries, versionInClause, windowClause, type HealthQueryKey } from "../queries";

const KEYS: HealthQueryKey[] = [
  "actives_cur", "actives_prev", "top_versions", "release_cur", "release_prev",
  "release_devices_cur", "release_devices_prev", "prints_cur", "prints_prev",
  "print_sessions_cur", "print_sessions_prev", "errors_cur", "errors_prev", "errors_baseline", "errors_by_device", "hangs_cur", "hangs_prev", "crashes_cur", "crashes_prev",
  "cdn_latest", "kind_days",
];

function emptyRows(): Record<HealthQueryKey, unknown[]> {
  return Object.fromEntries(KEYS.map((k) => [k, []])) as unknown as Record<HealthQueryKey, unknown[]>;
}

describe("windowClause", () => {
  it("is open-ended for the current window", () => {
    expect(windowClause(HEALTH_WINDOWS.cur)).toBe("timestamp >= NOW() - INTERVAL '7' DAY");
  });
  it("is bounded on both sides for an earlier window", () => {
    expect(windowClause(HEALTH_WINDOWS.prev)).toBe(
      "timestamp >= NOW() - INTERVAL '14' DAY AND timestamp < NOW() - INTERVAL '7' DAY",
    );
  });
  it("covers the 28 days before cur for the baseline", () => {
    expect(windowClause(HEALTH_WINDOWS.baseline)).toContain("INTERVAL '35' DAY");
    expect(windowClause(HEALTH_WINDOWS.baseline)).toContain("< NOW() - INTERVAL '7' DAY");
  });
});

describe("healthQueries", () => {
  const q = healthQueries({ version: "1.0.4" }, ["1.0.4", "v1.0.3", "x'; DROP"]);
  const win = (k: HealthQueryKey): string => {
    if (k.endsWith("_prev")) return windowClause(HEALTH_WINDOWS.prev);
    if (k.endsWith("_baseline")) return windowClause(HEALTH_WINDOWS.baseline);
    return windowClause(HEALTH_WINDOWS.cur);
  };

  it("puts each window in every windowed query", () => {
    for (const k of KEYS) {
      // The session lookup reads up to its window's end; prints.test.ts covers it.
      if (k === "cdn_latest" || k === "kind_days" || k.startsWith("print_sessions")) continue;
      expect(q[k], k).toContain(win(k));
    }
  });
  it("applies the session filter to sessions, actives and top versions", () => {
    const f = filterFor("session", { version: "1.0.4" });
    expect(q.actives_cur).toContain(f);
    expect(q.top_versions).toContain(f);
    expect(q.release_cur).toContain(f);
    expect(q.release_devices_cur).toContain(f);
  });
  it("applies the kind's own filter to prints, errors and crashes", () => {
    expect(q.prints_cur).toContain(filterFor("print_outcome", { version: "1.0.4" }));
    expect(q.errors_cur).toContain(filterFor("error_encountered", { version: "1.0.4" }));
    expect(q.crashes_cur).toContain(filterFor("crash", { version: "1.0.4" }));
  });
  it("limits top_versions to 8", () => {
    expect(q.top_versions).toMatch(/LIMIT 8/);
  });
  it("restricts release queries to sanitized versions", () => {
    expect(q.release_cur).toContain("'1.0.3'");
    expect(q.release_cur).not.toContain("'v1.0.3'");
    expect(q.release_cur).not.toContain("';");
  });
  it("matches nothing when there are no versions", () => {
    expect(healthQueries().release_cur).toContain("1 = 0");
  });
  it("ignores filters for cdn_latest and kind_days", () => {
    expect(q.cdn_latest).not.toContain("IN ('1.0.4')");
    expect(q.kind_days).not.toContain("IN ('1.0.4')");
    expect(q.cdn_latest).toContain("cdn_fleet_daily");
  });
  it("uses row_count and aliases that are not column names", () => {
    for (const k of KEYS) {
      expect(q[k], k).not.toMatch(/\bas rows\b/i);
      expect(q[k], k).not.toMatch(/GROUP BY[^\n]*\bblob\d+/);
    }
  });
});

describe("releaseRates", () => {
  const counts: ReleaseCounts = {
    sessions: 200, crashes: 10, session_devices: 100, crash_devices: 4,
    update_failures: 1, update_successes: 3, print_successes: 90, print_total: 100,
  };

  it("computes every rate from its own numerator and denominator", () => {
    const r = releaseRates(counts);
    expect(r.crash_free).toBeCloseTo(0.96);
    expect(r.crash_rate).toBeCloseTo(0.05);
    expect(r.update_failure_rate).toBeCloseTo(0.25);
    expect(r.print_success).toBeCloseTo(0.9);
  });

  it("clamps crash-free to [0, 1]", () => {
    expect(releaseRates({ ...counts, session_devices: 2, crash_devices: 5 }).crash_free).toBe(0);
    expect(releaseRates({ ...counts, session_devices: 2, crash_devices: -1 }).crash_free).toBe(1);
  });

  it("is null, never 0 or NaN, at a zero denominator", () => {
    const zero: ReleaseCounts = {
      sessions: 0, crashes: 0, session_devices: 0, crash_devices: 0,
      update_failures: 0, update_successes: 0, print_successes: 0, print_total: 0,
    };
    expect(releaseRates(zero)).toEqual({ crash_free: null, crash_rate: null, update_failure_rate: null, print_success: null });
  });

  it("nulls only the crash rates when crashes are not reported", () => {
    const r = releaseRates({ ...counts, crashes: 0, crash_devices: 0 }, false);
    expect(r.crash_free).toBeNull();
    expect(r.crash_rate).toBeNull();
    expect(r.update_failure_rate).toBeCloseTo(0.25);
    expect(r.print_success).toBeCloseTo(0.9);
  });
});

describe("buildHealth", () => {
  it("gives empty arrays and zero actives for no rows", () => {
    const h = buildHealth(emptyRows(), "2026-10-15");
    expect(h.fleet).toEqual({ actives_cur: 0, actives_prev: 0, cdn_sources: null, cdn_date: null });
    expect(h.releases).toEqual([]);
    expect(h.errors).toEqual([]);
    expect(h.crash_clusters).toEqual([]);
    expect(h.data_health).toEqual([]);
    expect(h.not_applicable).toEqual({ crashes: null, errors: null, prints: null, updates: null });
  });

  it("gives null crash rates when the filter leaves crashes unreported", () => {
    const r = emptyRows();
    r.top_versions = [{ ver: "1.0.4", devices: 100 }];
    r.release_cur = [{ ver: "1.0.4", sessions: 200, crashes: 0, update_failures: 0, update_successes: 0 }];
    r.release_devices_cur = [{ ver: "1.0.4", event: "session", devices: 100 }];
    const na = { crashes: "model", errors: "model", prints: "model", updates: "model" } as const;
    const h = buildHealth(r, "2026-10-15", na);
    expect(h.releases[0].cur.crash_free).toBeNull();
    expect(h.releases[0].cur.crash_rate).toBeNull();
    expect(h.releases[0].cur.devices).toBe(100);
    expect(h.not_applicable).toEqual(na);
  });

  it("computes crash-free and the rates", () => {
    const r = emptyRows();
    r.top_versions = [{ ver: "1.0.4", devices: 100 }];
    r.release_cur = [{ ver: "1.0.4", sessions: 200, crashes: 10, update_failures: 1, update_successes: 3 }];
    r.release_devices_cur = [
      { ver: "1.0.4", event: "session", devices: 100 },
      { ver: "1.0.4", event: "crash", devices: 4 },
    ];
    r.prints_cur = [{ ver: "1.0.4", print_successes: 90, print_total: 100 }];
    const rel = buildHealth(r, "2026-10-15").releases[0];
    expect(rel.version).toBe("1.0.4");
    expect(rel.cur.devices).toBe(100);
    expect(rel.cur.crash_free).toBeCloseTo(0.96);
    expect(rel.cur.crash_rate).toBeCloseTo(0.05);
    expect(rel.cur.update_failure_rate).toBeCloseTo(0.25);
    expect([rel.cur.update_failures, rel.cur.update_successes]).toEqual([1, 3]);
    expect(rel.cur.print_success).toBeCloseTo(0.9);
  });

  it("gives null rates, never NaN, for an all-zero window", () => {
    const r = emptyRows();
    r.top_versions = [{ ver: "1.0.4", devices: 3 }];
    r.release_cur = [{ ver: "1.0.4", sessions: 0, crashes: 0, update_failures: 0, update_successes: 0 }];
    r.prints_cur = [{ ver: "1.0.4", print_successes: 0, print_total: 0 }];
    const rel = buildHealth(r, "2026-10-15").releases[0];
    for (const w of [rel.cur, rel.prev]) {
      expect(w).toEqual({
        devices: 0, update_failures: 0, update_successes: 0,
        crash_free: null, crash_rate: null, update_failure_rate: null, print_success: null,
      });
    }
    expect(JSON.stringify(buildHealth(r, "2026-10-15"))).not.toContain("NaN");
  });

  it("orders releases newest first", () => {
    const r = emptyRows();
    r.top_versions = [
      { ver: "1.0.3", devices: 9 },
      { ver: "1.0.4-beta.2", devices: 5 },
      { ver: "1.0.4", devices: 7 },
    ];
    expect(buildHealth(r, "2026-10-15").releases.map((x) => x.version)).toEqual(["1.0.4", "1.0.4-beta.2", "1.0.3"]);
  });

  describe("error status", () => {
    const err = (cur: number, prev: number, inBaseline: boolean): HealthPayload["errors"] => {
      const r = emptyRows();
      r.errors_cur = [{ category: "net", code: "E1", count: cur, devices: 2 }];
      r.errors_prev = prev ? [{ category: "net", code: "E1", count: prev, devices: 1 }] : [];
      r.errors_baseline = inBaseline ? [{ category: "net", code: "E1", count: prev, devices: 1 }] : [];
      return buildHealth(r, "2026-10-15").errors;
    };
    it("is new when baseline has no row", () => {
      expect(err(3, 0, false)[0].status).toBe("new");
    });
    it("is steady at cur 4, prev 1 (under 5)", () => {
      expect(err(4, 1, true)[0].status).toBe("steady");
    });
    it("is rising at cur 10, prev 5", () => {
      expect(err(10, 5, true)[0].status).toBe("rising");
    });
    it("is steady when cur is under double prev", () => {
      expect(err(9, 5, true)[0].status).toBe("steady");
    });
    it("is steady when one device accounts for more than half the count", () => {
      const r = emptyRows();
      r.errors_cur = [{ category: "net", code: "E1", count: 10, devices: 4 }];
      r.errors_prev = [{ category: "net", code: "E1", count: 2, devices: 1 }];
      r.errors_baseline = r.errors_prev;
      r.errors_by_device = [
        { category: "net", code: "E1", count: 6 },
        { category: "net", code: "E1", count: 3 },
      ];
      expect(buildHealth(r, "2026-10-15").errors[0].status).toBe("steady");
    });
    it("is rising when the busiest device holds exactly half", () => {
      const r = emptyRows();
      r.errors_cur = [{ category: "net", code: "E1", count: 10, devices: 4 }];
      r.errors_prev = [{ category: "net", code: "E1", count: 2, devices: 1 }];
      r.errors_baseline = r.errors_prev;
      r.errors_by_device = [{ category: "net", code: "E1", count: 5 }];
      expect(buildHealth(r, "2026-10-15").errors[0].status).toBe("rising");
    });
    it("is steady when a single device reports it", () => {
      const r = emptyRows();
      r.errors_cur = [{ category: "net", code: "E1", count: 10, devices: 1 }];
      r.errors_prev = [{ category: "net", code: "E1", count: 2, devices: 1 }];
      r.errors_baseline = r.errors_prev;
      expect(buildHealth(r, "2026-10-15").errors[0].status).toBe("steady");
    });
    it("drops rows with cur 0", () => {
      expect(err(0, 4, true)).toEqual([]);
    });
    it("orders new, rising, steady, then by cur", () => {
      const r = emptyRows();
      r.errors_cur = [
        { category: "a", code: "steady_big", count: 50, devices: 1 },
        { category: "a", code: "new_small", count: 1, devices: 1 },
        { category: "a", code: "rising", count: 12, devices: 2 },
        { category: "a", code: "steady_small", count: 6, devices: 1 },
        { category: "a", code: "new_big", count: 3, devices: 1 },
      ];
      r.errors_prev = [
        { category: "a", code: "steady_big", count: 40, devices: 1 },
        { category: "a", code: "rising", count: 5, devices: 1 },
        { category: "a", code: "steady_small", count: 6, devices: 1 },
      ];
      r.errors_baseline = r.errors_prev;
      expect(buildHealth(r, "2026-10-15").errors.map((e) => e.code)).toEqual([
        "new_big", "new_small", "rising", "steady_big", "steady_small",
      ]);
    });
  });

  it("lists crash clusters with cur > 0, largest first", () => {
    const r = emptyRows();
    r.crashes_cur = [
      { sig: "SIGSEGV", fault: "", ver: "1.0.4", board: "pi", count: 2, devices: 2 },
      { sig: "SIGABRT", fault: "x", ver: "1.0.4", board: "pi", count: 9, devices: 3 },
      { sig: "SIGBUS", fault: "", ver: "1.0.4", board: "pi", count: 0, devices: 0 },
    ];
    r.crashes_prev = [{ sig: "SIGSEGV", fault: "", ver: "1.0.4", board: "pi", count: 1, devices: 1 }];
    const c = buildHealth(r, "2026-10-15").crash_clusters;
    expect(c.map((x) => x.signal)).toEqual(["SIGABRT", "SIGSEGV"]);
    expect(c[1]).toEqual({ signal: "SIGSEGV", fault_code: "", version: "1.0.4", board: "pi", cur: 2, prev: 1, devices: 2 });
  });

  describe("data health", () => {
    // today is 2026-10-15: latest is 10-14, the median covers 10-07 .. 10-13.
    const day = (n: number) => `2026-10-${String(n).padStart(2, "0")}`;
    const rowsFor = (kind: string, byDay: Record<number, number>) =>
      Object.entries(byDay).map(([d, count]) => ({ day: day(Number(d)), event: kind, count }));
    const steady = (latest: number) => ({ 14: latest, 13: 100, 12: 100, 11: 100, 10: 100, 9: 100, 8: 100, 7: 100 });
    const run = (rows: unknown[]) => {
      const r = emptyRows();
      r.kind_days = rows;
      return buildHealth(r, "2026-10-15").data_health;
    };
    it("flags 40 against a median of 100", () => {
      expect(run(rowsFor("session", steady(40)))).toEqual([{ kind: "session", latest: 40, median: 100, low: true }]);
    });
    it("does not flag 60 against a median of 100", () => {
      expect(run(rowsFor("session", steady(60)))[0].low).toBe(false);
    });
    it("flags a kind with rows on the 7 prior days and none yesterday, latest 0", () => {
      const { 14: _y, ...prior } = steady(0);
      expect(run(rowsFor("session", prior))).toEqual([{ kind: "session", latest: 0, median: 100, low: true }]);
    });
    it("never flags a kind whose median is under LOW_MIN_MEDIAN, even at 0", () => {
      const low = (median: number) => {
        const days = Object.fromEntries(Array.from({ length: 7 }, (_, i) => [7 + i, median]));
        return run(rowsFor("update_failed", days))[0];
      };
      expect(low(2)).toEqual({ kind: "update_failed", latest: 0, median: 2, low: false });
      expect(low(LOW_MIN_MEDIAN - 1).low).toBe(false);
      expect(low(LOW_MIN_MEDIAN).low).toBe(true);
    });
    it("never flags a kind with sporadic rows (median 0)", () => {
      expect(run(rowsFor("crash", { 14: 1, 9: 2 }))[0]).toEqual({ kind: "crash", latest: 1, median: 0, low: false });
      expect(run(rowsFor("crash", { 13: 5 }))[0].low).toBe(false);
    });
    it("ignores today's partial row and the partial today-9 row", () => {
      const rows = [...rowsFor("session", steady(100)), ...rowsFor("session", { 15: 1, 6: 1 })];
      expect(run(rows)).toEqual([{ kind: "session", latest: 100, median: 100, low: false }]);
    });
    it("counts a missing day inside the median window as 0", () => {
      // 10-07 .. 10-13 holds 3 days of 100 and 4 missing: median 0
      const h = run(rowsFor("session", { 14: 1, 13: 100, 12: 100, 11: 100 }));
      expect(h[0].median).toBe(0);
      expect(h[0].low).toBe(false);
    });
    it("leaves out cdn_fleet_daily", () => {
      expect(run(rowsFor("cdn_fleet_daily", steady(1)))).toEqual([]);
    });
  });

  it("reads the newest cdn row", () => {
    const r = emptyRows();
    r.cdn_latest = [{ date: "2026-10-14", sources: 321 }];
    expect(buildHealth(r, "2026-10-15").fleet).toMatchObject({ cdn_sources: 321, cdn_date: "2026-10-14" });
  });

  it("carries both active counts", () => {
    const r = emptyRows();
    r.actives_cur = [{ actives: 50 }];
    r.actives_prev = [{ actives: 40 }];
    expect(buildHealth(r, "2026-10-15").fleet).toMatchObject({ actives_cur: 50, actives_prev: 40 });
  });
});

describe("versionInClause", () => {
  it("strips a v prefix and quotes each version", () => {
    expect(versionInClause("blob2", ["1.0.4", "v1.0.3"])).toBe("blob2 IN ('1.0.4', '1.0.3')");
  });
  it("drops quotes and semicolons", () => {
    expect(versionInClause("blob2", ["1.0';x"])).toBe("blob2 IN ('1.0x')");
  });
  it("is SQL_FALSE for an empty or all-empty list", () => {
    expect(versionInClause("blob2", [])).toBe("1 = 0");
    expect(versionInClause("blob2", ["';"])).toBe("1 = 0");
  });
});
