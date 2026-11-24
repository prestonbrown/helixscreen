// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the Health queries and builds the payload. The dashboard's health route and the
// feedback loop both read it here.

import { hangSnapshotsSql, healthQueries, HEALTH_WINDOWS, notApplicableFor, executeQuery, type FilterParams, type QueryConfig } from "./queries";
import { buildHealth, type HealthBlock, type HealthPayload, type HealthRowKey } from "./health";

/** The Health blocks a filter can leave unreported. */
const HEALTH_BLOCKS = ["crashes", "errors", "prints", "updates"] as const satisfies readonly HealthBlock[];

/** `today` is the UTC day (YYYY-MM-DD) the payload is built for. */
export async function fetchHealth(config: QueryConfig, today: string, filters?: FilterParams): Promise<HealthPayload> {
  // top_versions first: the per-release queries are restricted to its versions.
  const topRes = (await executeQuery(config, healthQueries(filters).top_versions)) as {
    data?: Array<{ ver: string }>;
  };
  const versions = (topRes.data ?? []).map((r) => String(r.ver));
  const queries = healthQueries(filters, versions);
  const rest = Object.keys(queries).filter((k) => k !== "top_versions") as Exclude<HealthRowKey, "hang_snapshots">[];
  const results = await Promise.all(rest.map((k) => executeQuery(config, queries[k])));
  const rows = { top_versions: topRes.data ?? [] } as Record<HealthRowKey, unknown[]>;
  rest.forEach((k, i) => {
    rows[k] = (results[i] as { data?: unknown[] } | null)?.data ?? [];
  });
  // The devices that hung, so the snapshot query reads only their series.
  const hangDevices = [...new Set([...rows.hangs_cur, ...rows.hangs_prev].map((h) => String((h as { device: string }).device)))];
  rows.hang_snapshots = hangDevices.length === 0 ? [] : await hangSnapshots(config, hangDevices);
  return buildHealth(rows, today, notApplicableFor(HEALTH_BLOCKS, filters));
}

/** Devices per snapshot query, to keep the SQL a sane length. */
const SNAPSHOT_DEVICES_PER_QUERY = 100;

async function hangSnapshots(config: QueryConfig, devices: string[]): Promise<unknown[]> {
  const chunks: string[][] = [];
  for (let i = 0; i < devices.length; i += SNAPSHOT_DEVICES_PER_QUERY) {
    chunks.push(devices.slice(i, i + SNAPSHOT_DEVICES_PER_QUERY));
  }
  // The window reaches one day past the oldest hang so its latest earlier snapshot is in range.
  const window = [0, HEALTH_WINDOWS.prev[1] + 1] as const;
  const results = await Promise.all(chunks.map((c) => executeQuery(config, hangSnapshotsSql(c, window))));
  return results.flatMap((res) => (res as { data?: unknown[] } | null)?.data ?? []);
}
