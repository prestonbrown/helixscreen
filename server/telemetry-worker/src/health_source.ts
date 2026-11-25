// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the Health queries and builds the payload. The dashboard's health route and the
// feedback loop both read it here.

import {
  hangSnapshotsSql,
  hangWarningsSql,
  healthQueries,
  HEALTH_WINDOWS,
  notApplicableFor,
  executeQuery,
  type FilterParams,
  type HangUpload,
  type HealthQueryKey,
  type QueryConfig,
} from "./queries";
import { buildHealth, type HealthBlock, type HealthPayload, type HealthRowKey } from "./health";
import { num, str, type RawRow } from "./schema";

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
  const rest = Object.keys(queries).filter((k) => k !== "top_versions") as HealthQueryKey[];
  const results = await Promise.all(rest.map((k) => executeQuery(config, queries[k])));
  const rows = { top_versions: topRes.data ?? [] } as Record<HealthRowKey, unknown[]>;
  rest.forEach((k, i) => {
    rows[k] = (results[i] as { data?: unknown[] } | null)?.data ?? [];
  });
  // The uploads that hung: their devices' snapshots and their own warnings are the evidence
  // that classifies a hang, so those queries read no other series.
  const uploads = [...rows.hangs_cur, ...rows.hangs_prev].map(hangUploadOf);
  const hangDevices = [...new Set(uploads.map((u) => u.device))];
  const [snapshots, warnings] = await Promise.all([
    readEvidence("hang snapshots", () => hangSnapshots(config, hangDevices)),
    readEvidence("hang warnings", () => hangWarnings(config, uploads)),
  ]);
  // Left unset, the snapshot rows make buildHealth report main_loop_hang as the plain total,
  // and the warning rows leave an older hang to its snapshot alone.
  if (snapshots !== undefined) rows.hang_snapshots = snapshots;
  if (warnings !== undefined) rows.hang_warnings = warnings;
  return buildHealth(rows, today, notApplicableFor(HEALTH_BLOCKS, filters));
}

/** The upload a hang row came from: the device that reported, and the epoch seconds the Worker stored it. */
const hangUploadOf = (h: unknown): HangUpload => {
  const x = h as RawRow;
  return { device: str(x.device), at: num(x.at) };
};

/**
 * `read`'s rows, or undefined after logging why `what` failed: evidence that cannot be read
 * is left out of the payload rather than failing the whole of it.
 */
async function readEvidence(what: string, read: () => Promise<unknown[]>): Promise<unknown[] | undefined> {
  try {
    return await read();
  } catch (err) {
    console.error(`[health] ${new Date().toISOString()} ${what} failed: ${err instanceof Error ? err.message : String(err)}`);
    return undefined;
  }
}

/** Rows per query, to keep the SQL a sane length. */
const ROWS_PER_QUERY = 100;

/** `sql` over `items` a chunk at a time; every chunk's rows in one list. */
async function rowsPerChunk<T>(config: QueryConfig, items: readonly T[], sql: (chunk: T[]) => string): Promise<unknown[]> {
  const chunks: T[][] = [];
  for (let i = 0; i < items.length; i += ROWS_PER_QUERY) chunks.push(items.slice(i, i + ROWS_PER_QUERY));
  const results = await Promise.all(chunks.map((c) => executeQuery(config, sql(c))));
  return results.flatMap((res) => (res as { data?: unknown[] } | null)?.data ?? []);
}

async function hangSnapshots(config: QueryConfig, devices: string[]): Promise<unknown[]> {
  // The window reaches one day past the oldest hang so its latest earlier snapshot is in range.
  const window = [0, HEALTH_WINDOWS.prev[1] + 1] as const;
  return rowsPerChunk(config, devices, (chunk) => hangSnapshotsSql(chunk, window));
}

async function hangWarnings(config: QueryConfig, uploads: HangUpload[]): Promise<unknown[]> {
  return rowsPerChunk(config, uploads, hangWarningsSql);
}
