// SPDX-License-Identifier: GPL-3.0-or-later
// Daily snapshot of CDN update-manifest polls, used to estimate the install
// base without depending on opt-in telemetry.
//
// Why this exists: every install polls {channel}/manifest.json on startup and
// then every 24h (UpdateChecker: 15s initial delay, 24h periodic). That poll
// happens regardless of whether the user enabled telemetry, so it is the only
// signal that sees the whole fleet. The `session` event in Analytics Engine
// only ever sees the opt-in subset, which measured roughly half the real
// population.
//
// Why a scheduled snapshot rather than instrumenting a Worker: 99.9% of polls
// are served by releases.helixscreen.org, which is an R2 custom domain and
// never runs Worker code. Only ~0.1% reach dl-worker. The counts therefore
// have to come from Cloudflare's zone HTTP analytics, and those roll off after
// about 8 days on the current plan - so anything not snapshotted is lost.
//
// PRIVACY: client IPs are used only to compute a cardinality, in memory, and
// are never persisted. Every field written out is an integer or a date. This
// metric carries no device_id and no per-install identifier of any kind.

const GRAPHQL_ENDPOINT = "https://api.cloudflare.com/client/v4/graphql";

/** Channels whose manifest polls we track. */
export const TRACKED_CHANNELS = ["stable", "beta", "dev"] as const;
export type TrackedChannel = (typeof TRACKED_CHANNELS)[number];

/** Cloudflare caps a single adaptive-analytics group query; flag truncation. */
const ROW_LIMIT = 10000;

/** One channel's poll activity for one UTC day. */
export interface ChannelDaySnapshot {
  channel: TrackedChannel;
  /** Successful polls (2xx/304). */
  polls: number;
  /** Distinct client IPs seen. Cardinality only - no addresses retained. */
  sources: number;
  /** Distinct client countries seen. */
  countries: number;
  /** Polls that returned >=400. A non-zero value here means installs on this
   *  channel are failing their update check. */
  errors: number;
  /** True if the row limit was hit and `sources` is an undercount. */
  truncated: boolean;
}

/** A full day across all tracked channels. */
export interface FleetDaySnapshot {
  /** UTC day being described, YYYY-MM-DD. */
  date: string;
  channels: ChannelDaySnapshot[];
}

interface GraphQLRow {
  count: number;
  dimensions: {
    clientIP: string;
    clientCountryName: string;
    edgeResponseStatus: number;
  };
}

const QUERY = `
query($zone:String!,$start:Time!,$end:Time!,$path:String!){
  viewer{ zones(filter:{zoneTag:$zone}){
    httpRequestsAdaptiveGroups(
      limit:${ROW_LIMIT},
      filter:{datetime_geq:$start, datetime_lt:$end, clientRequestPath:$path},
      orderBy:[count_DESC]
    ){ count dimensions{clientIP clientCountryName edgeResponseStatus} }
  }}}
`;

/** Format a Date as a UTC YYYY-MM-DD day string. */
export function utcDay(d: Date): string {
  return d.toISOString().slice(0, 10);
}

/**
 * The UTC day before `now`. The cron runs after midnight and snapshots the
 * day that just closed, so the window is always complete.
 */
export function previousUtcDay(now: Date): string {
  return utcDay(new Date(Date.UTC(
    now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 1)));
}

async function queryChannel(
  token: string,
  zoneId: string,
  date: string,
  channel: TrackedChannel,
): Promise<ChannelDaySnapshot> {
  const start = `${date}T00:00:00Z`;
  const end = `${date}T23:59:59Z`;

  const res = await fetch(GRAPHQL_ENDPOINT, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      query: QUERY,
      variables: {
        zone: zoneId,
        start,
        end,
        path: `/${channel}/manifest.json`,
      },
    }),
  });

  if (!res.ok) {
    throw new Error(`zone analytics HTTP ${res.status} for ${channel}`);
  }

  const body = (await res.json()) as {
    errors?: Array<{ message: string }>;
    data?: { viewer?: { zones?: Array<{ httpRequestsAdaptiveGroups?: GraphQLRow[] }> } };
  };

  if (body.errors?.length) {
    throw new Error(`zone analytics: ${body.errors[0].message}`);
  }

  const rows = body.data?.viewer?.zones?.[0]?.httpRequestsAdaptiveGroups ?? [];

  // Cardinality is computed here and the sets are discarded when this function
  // returns. Nothing downstream ever sees an address.
  const ips = new Set<string>();
  const countries = new Set<string>();
  let polls = 0;
  let errors = 0;

  for (const row of rows) {
    const status = row.dimensions.edgeResponseStatus;
    if (status >= 400) {
      errors += row.count;
      continue;
    }
    polls += row.count;
    ips.add(row.dimensions.clientIP);
    countries.add(row.dimensions.clientCountryName);
  }

  return {
    channel,
    polls,
    sources: ips.size,
    countries: countries.size,
    errors,
    truncated: rows.length >= ROW_LIMIT,
  };
}

/**
 * Snapshot one UTC day across every tracked channel.
 *
 * A channel that fails to query does not abort the others - a partial day is
 * more useful than none, and the retention window means there is no second
 * chance to collect it.
 */
export async function snapshotDay(
  token: string,
  zoneId: string,
  date: string,
): Promise<FleetDaySnapshot> {
  const results = await Promise.allSettled(
    TRACKED_CHANNELS.map((c) => queryChannel(token, zoneId, date, c)),
  );

  const channels: ChannelDaySnapshot[] = [];
  const reasons: string[] = [];
  for (const r of results) {
    if (r.status === "fulfilled") {
      channels.push(r.value);
    } else {
      reasons.push(r.reason instanceof Error ? r.reason.message : String(r.reason));
    }
  }

  if (channels.length === 0) {
    // Carry the underlying reason. A bare "all channels failed" is
    // indistinguishable between a bad token, a scope the token lacks, and a
    // zone that returned nothing - which are three very different fixes.
    const why = [...new Set(reasons)].join("; ") || "no reason reported";
    throw new Error(`all channels failed for ${date}: ${why}`);
  }

  return { date, channels };
}

/**
 * Analytics Engine data points for one day.
 *
 * One point per channel, keyed `cdn_fleet_daily`. blob1 carries the day being
 * described rather than a device_id - this event has no device - so dashboard
 * queries must group on blob1 and must NOT apply the platform/version/model
 * filter clause, none of which exist here.
 *
 * Re-running a snapshot for the same day appends duplicate rows, so every read
 * of this data aggregates with max() rather than sum(). That makes backfills
 * and retries idempotent in effect.
 */
export function snapshotToDataPoints(snap: FleetDaySnapshot): Array<{
  blobs: string[];
  doubles: number[];
  indexes: string[];
}> {
  return snap.channels.map((c) => ({
    indexes: ["cdn_fleet_daily"],
    blobs: [snap.date, c.channel, c.truncated ? "truncated" : "", "", "", "", "", "", "", "", "", ""],
    doubles: [c.polls, c.sources, c.countries, c.errors, 0, 0, 0, 0],
  }));
}

/** R2 key for the permanent copy, outside the `events/` prefix that
 *  /v1/events/list walks. */
export function snapshotR2Key(date: string): string {
  return `cdn/${date.slice(0, 4)}/${date.slice(5, 7)}/${date}.json`;
}
