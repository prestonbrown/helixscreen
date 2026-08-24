// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the CDN fleet snapshot: the daily install-base estimate derived
// from update-manifest polls in Cloudflare zone analytics.

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import {
  snapshotDay,
  snapshotToDataPoints,
  snapshotR2Key,
  previousUtcDay,
  utcDay,
  TRACKED_CHANNELS,
} from "../cdn_fleet";

/** Build a GraphQL response body for one channel. */
function gqlOk(rows: Array<{ ip: string; country: string; status: number; count: number }>) {
  return {
    ok: true,
    json: async () => ({
      data: {
        viewer: {
          zones: [
            {
              httpRequestsAdaptiveGroups: rows.map((r) => ({
                count: r.count,
                dimensions: {
                  clientIP: r.ip,
                  clientCountryName: r.country,
                  edgeResponseStatus: r.status,
                },
              })),
            },
          ],
        },
      },
    }),
  } as unknown as Response;
}

function gqlError(message: string) {
  return {
    ok: true,
    json: async () => ({ errors: [{ message }] }),
  } as unknown as Response;
}

describe("date helpers", () => {
  it("formats a UTC day", () => {
    expect(utcDay(new Date("2026-08-15T18:33:00Z"))).toBe("2026-08-15");
  });

  it("returns the previous UTC day", () => {
    expect(previousUtcDay(new Date("2026-08-15T02:10:00Z"))).toBe("2026-08-14");
  });

  it("rolls back across a month boundary", () => {
    expect(previousUtcDay(new Date("2026-08-01T02:10:00Z"))).toBe("2026-07-31");
  });

  it("rolls back across a year boundary", () => {
    expect(previousUtcDay(new Date("2026-01-01T02:10:00Z"))).toBe("2025-12-31");
  });

  it("uses UTC, not local time, just before midnight UTC", () => {
    // 23:30 UTC on the 15th is still the 15th regardless of host timezone.
    expect(previousUtcDay(new Date("2026-08-15T23:30:00Z"))).toBe("2026-08-14");
  });
});

describe("snapshotR2Key", () => {
  it("partitions by year and month, outside the events/ prefix", () => {
    expect(snapshotR2Key("2026-08-14")).toBe("cdn/2026/08/2026-08-14.json");
  });

  it("never collides with the events/ prefix that /v1/events/list walks", () => {
    expect(snapshotR2Key("2026-08-14").startsWith("events/")).toBe(false);
  });
});

describe("snapshotDay", () => {
  let fetchMock: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("counts distinct IPs, not polls", async () => {
    // 3 polls from 2 addresses must report 2 sources, 3 polls.
    fetchMock.mockResolvedValue(
      gqlOk([
        { ip: "1.1.1.1", country: "US", status: 200, count: 2 },
        { ip: "2.2.2.2", country: "US", status: 200, count: 1 },
      ]),
    );

    const snap = await snapshotDay("tok", "zone", "2026-08-14");
    const stable = snap.channels.find((c) => c.channel === "stable")!;
    expect(stable.sources).toBe(2);
    expect(stable.polls).toBe(3);
    expect(stable.countries).toBe(1);
  });

  it("separates error polls from successful ones", async () => {
    // A 404 must not inflate the fleet estimate, but must be reported: it means
    // installs on that channel are failing their update check.
    fetchMock.mockResolvedValue(
      gqlOk([
        { ip: "1.1.1.1", country: "US", status: 200, count: 5 },
        { ip: "9.9.9.9", country: "DE", status: 404, count: 7 },
      ]),
    );

    const snap = await snapshotDay("tok", "zone", "2026-08-14");
    const stable = snap.channels.find((c) => c.channel === "stable")!;
    expect(stable.polls).toBe(5);
    expect(stable.errors).toBe(7);
    // The erroring address must not be counted as a live install.
    expect(stable.sources).toBe(1);
    expect(stable.countries).toBe(1);
  });

  it("queries every tracked channel", async () => {
    fetchMock.mockResolvedValue(gqlOk([]));
    await snapshotDay("tok", "zone", "2026-08-14");

    expect(fetchMock).toHaveBeenCalledTimes(TRACKED_CHANNELS.length);
    const paths = fetchMock.mock.calls.map(
      (c) => JSON.parse((c[1] as RequestInit).body as string).variables.path,
    );
    for (const ch of TRACKED_CHANNELS) {
      expect(paths).toContain(`/${ch}/manifest.json`);
    }
  });

  it("bounds the query to the requested UTC day", async () => {
    fetchMock.mockResolvedValue(gqlOk([]));
    await snapshotDay("tok", "zone", "2026-08-14");

    const vars = JSON.parse((fetchMock.mock.calls[0][1] as RequestInit).body as string).variables;
    expect(vars.start).toBe("2026-08-14T00:00:00Z");
    expect(vars.end).toBe("2026-08-14T23:59:59Z");
    expect(vars.zone).toBe("zone");
  });

  it("keeps the channels that succeeded when one fails", async () => {
    // A partial day beats no day: zone analytics will not still have it
    // tomorrow, so one bad channel must not discard the others.
    fetchMock
      .mockResolvedValueOnce(gqlOk([{ ip: "1.1.1.1", country: "US", status: 200, count: 4 }]))
      .mockResolvedValueOnce(gqlError("boom"))
      .mockResolvedValueOnce(gqlOk([]));

    const snap = await snapshotDay("tok", "zone", "2026-08-14");
    expect(snap.channels.length).toBe(2);
    expect(snap.channels.some((c) => c.channel === "stable")).toBe(true);
  });

  it("throws when every channel fails", async () => {
    fetchMock.mockResolvedValue(gqlError("nope"));
    await expect(snapshotDay("tok", "zone", "2026-08-14")).rejects.toThrow();
  });

  it("carries the underlying reason when every channel fails", async () => {
    // A bare "all channels failed" cannot distinguish a bad token from a
    // missing scope from an empty zone, and those need different fixes.
    fetchMock.mockResolvedValue(gqlError("Unauthorized to access requested resource"));
    await expect(snapshotDay("tok", "zone", "2026-08-14")).rejects.toThrow(
      /Unauthorized to access requested resource/,
    );
  });

  it("does not repeat an identical reason once per channel", async () => {
    fetchMock.mockResolvedValue(gqlError("same problem"));
    const err = await snapshotDay("tok", "zone", "2026-08-14").catch((e: Error) => e);
    expect((err as Error).message.match(/same problem/g)?.length).toBe(1);
  });

  it("surfaces an HTTP failure rather than recording an empty day", async () => {
    // Recording zeroes on an auth failure would look like the fleet vanished.
    fetchMock.mockResolvedValue({ ok: false, status: 403 } as unknown as Response);
    await expect(snapshotDay("tok", "zone", "2026-08-14")).rejects.toThrow();
  });

  it("sends the token as a bearer credential", async () => {
    fetchMock.mockResolvedValue(gqlOk([]));
    await snapshotDay("sekrit", "zone", "2026-08-14");
    const headers = (fetchMock.mock.calls[0][1] as RequestInit).headers as Record<string, string>;
    expect(headers.Authorization).toBe("Bearer sekrit");
  });
});

describe("snapshotToDataPoints", () => {
  const snap = {
    date: "2026-08-14",
    channels: [
      { channel: "stable" as const, polls: 1184, sources: 475, countries: 56, errors: 0, truncated: false },
      { channel: "beta" as const, polls: 0, sources: 0, countries: 0, errors: 150, truncated: false },
    ],
  };

  it("writes one point per channel under a single index", () => {
    const pts = snapshotToDataPoints(snap);
    expect(pts.length).toBe(2);
    expect(pts.every((p) => p.indexes[0] === "cdn_fleet_daily")).toBe(true);
  });

  it("puts the described day in blob1 and the channel in blob2", () => {
    // The dashboard groups on blob1 and filters on blob2; if these move, the
    // Fleet tile silently reads the wrong column.
    const [stable] = snapshotToDataPoints(snap);
    expect(stable.blobs[0]).toBe("2026-08-14");
    expect(stable.blobs[1]).toBe("stable");
  });

  it("orders doubles as polls, sources, countries, errors", () => {
    const [stable] = snapshotToDataPoints(snap);
    expect(stable.doubles[0]).toBe(1184);
    expect(stable.doubles[1]).toBe(475);
    expect(stable.doubles[2]).toBe(56);
    expect(stable.doubles[3]).toBe(0);
  });

  it("carries a channel's error count even when it saw no successful polls", () => {
    const beta = snapshotToDataPoints(snap)[1];
    expect(beta.doubles[1]).toBe(0);
    expect(beta.doubles[3]).toBe(150);
  });

  it("pads to the 12 blob / 8 double shape the dataset expects", () => {
    const [stable] = snapshotToDataPoints(snap);
    expect(stable.blobs.length).toBe(12);
    expect(stable.doubles.length).toBe(8);
  });

  it("flags a truncated day so an undercount is not read as a real drop", () => {
    const pts = snapshotToDataPoints({
      date: "2026-08-14",
      channels: [
        { channel: "stable" as const, polls: 1, sources: 1, countries: 1, errors: 0, truncated: true },
      ],
    });
    expect(pts[0].blobs[2]).toBe("truncated");
  });
});
