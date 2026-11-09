# Telemetry Admin Guide

Administration guide for HelixScreen's telemetry pipeline and analytics dashboard.

## Architecture

```
[Devices] → POST /v1/events → [Worker] → R2 (raw archive, permanent)
                                        → Analytics Engine (queryable, 90-day)

[Cron 02:10 UTC] → [Worker.scheduled] → Cloudflare zone analytics (GraphQL)
                                      → R2 cdn/ (permanent)
                                      → Analytics Engine (cdn_fleet_daily)

[Vue Dashboard] → GET /v1/dashboard/* → [Worker] → Analytics Engine SQL API
      ↑
  Cloudflare Pages (analytics.helixscreen.org)
```

- **R2**: Permanent raw event archive in EU jurisdiction
- **Analytics Engine**: 90-day queryable store for dashboard queries
- **Worker**: `telemetry.helixscreen.org` — handles ingest + admin API + dashboard API
- **Dashboard**: `analytics.helixscreen.org` — Vue SPA on Cloudflare Pages

## Fleet size: two numbers that do not agree

The dashboard shows two population figures, and they measure different
things. Quoting the wrong one understates the project by roughly half.

| Tile | Source | What it counts |
|------|--------|----------------|
| **Active Devices** | `session` events in Analytics Engine | Installs whose owner **opted into telemetry** and that launched in the window |
| **Fleet (CDN)** | `cdn_fleet_daily`, from zone HTTP analytics | Distinct addresses that polled the update manifest, **regardless of telemetry opt-in** |

Telemetry is opt-in and defaults OFF, so `Active Devices` is a self-selected
floor. Every install polls `{channel}/manifest.json` 15s after startup and every
24h thereafter (`UpdateChecker`), whether or not telemetry is on - so the CDN
figure sees installs the telemetry pipeline never will. When first measured
(2026-08-14) they read 549 and ~470 respectively, but over incomparable windows:
549 was 30 days of opt-in devices, ~470 was **a single day** of distinct
addresses. The 8-day distinct union was 1,092.

**Read `Fleet (CDN)` as an estimate with error bars in both directions.** NAT
puts several printers behind one address and undercounts; dynamic residential
IPs split one printer across several days and overcount. It is a better
population estimate than `Active Devices`, not a headcount.

### Why a scheduled snapshot rather than a counter

`dl-worker` cannot count these polls. 99.9% of them are served by
`releases.helixscreen.org`, an R2 custom domain that never executes Worker code;
only ~0.1% reach `dl.helixscreen.org`. The numbers therefore come from
Cloudflare's zone HTTP analytics, which **retain only about 8 days** on the
current plan. The daily cron exists because anything not snapshotted inside that
window is gone permanently - there is no backfill beyond ~8 days, ever.

### Privacy

The snapshot computes the cardinality of client IPs in memory and discards the
addresses. Every persisted field is an integer or a date. `cdn_fleet_daily`
carries no `device_id` and no per-install identifier of any kind, which makes it
strictly less identifying than the opt-in telemetry it sits beside.

### Operating it

```bash
source .env.telemetry
BASE=https://telemetry.helixscreen.org/v1/admin/cdn-snapshot

# One specific UTC day.
curl -X POST -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" "$BASE?date=2026-08-14"

# Backfill the whole retention window - ONE DAY PER REQUEST, on purpose.
for i in $(seq 1 8); do
  D=$(date -u -d "$i days ago" +%F)
  curl -sS -X POST -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" "$BASE?date=$D"
  echo
done
```

Safe to repeat: reads aggregate with `max()`, so duplicate rows collapse rather
than double-count.

**`days` is clamped to 3, and a longer backfill must be looped one day per
request.** Analytics Engine silently drops `writeDataPoint` calls past a ceiling
within a single invocation. Measured 2026-08-15: a `days=8` run wrote 24 points
(8 days x 3 channels), persisted only the **first four days**, and still
reported all eight as `captured` - because the snapshot and the R2 write both
succeeded and only the fire-and-forget AE write was dropped. Re-running the
missing days individually persisted them correctly.

So `captured` in the response means "queried and archived to R2", **not**
"queryable in the dashboard". Confirm against the dashboard before believing a
large backfill:

```bash
curl -sS -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
  "https://telemetry.helixscreen.org/v1/dashboard/overview?range=30d" \
  | python3 -c "import json,sys; print(len(json.load(sys.stdin)['cdn_fleet']), 'days queryable')"
```

Allow a minute or two for AE ingestion before concluding a day is missing.

**A non-zero `errors` count on a channel means installs on it are failing their
update check.** This is not cosmetic: it was how we found that
/beta/manifest.json was returning 404 to ~19 devices a day before the 1.0
branch cut created it.

**Known gap:** the Overview query filters `blob2 = 'stable'`, so the tile's
error count only reflects the stable channel. Beta and dev rows *are* written
and stored - they are just not surfaced yet. To see them, query the snapshot
endpoint output directly or the R2 archive under `cdn/`. Adding a per-channel
breakdown to the dashboard is the obvious next step if beta traffic ever
matters more than it does today.

## Secrets & Configuration

### Worker Secrets (set via `wrangler secret put`)

| Secret | Purpose |
|--------|---------|
| `INGEST_API_KEY` | Baked into the HelixScreen binary. Write-only ingest access. |
| `ADMIN_API_KEY` | Server-side secret. Read access to events, dashboard queries, AE counts. |
| `HELIX_ANALYTICS_READ_TOKEN` | Cloudflare API token for Analytics Engine SQL queries. |
| `RESEND_API_KEY`, `GITHUB_APP_PRIVATE_KEY` | Feedback loop email and issue filing; see Feedback loop. |
| `CF_ZONE_ANALYTICS_TOKEN` | Optional. Cloudflare API token with **Zone Analytics:Read** on `helixscreen.org`, for the daily CDN fleet snapshot. Falls back to `HELIX_ANALYTICS_READ_TOKEN` if unset - which works only if that token also carries zone analytics scope (Account Analytics:Read alone is not enough). |

### Worker Config (in `wrangler.toml`)

| Var | Purpose |
|-----|---------|
| `CLOUDFLARE_ACCOUNT_ID` | Your Cloudflare account ID (not secret). |
| `CDN_ZONE_ID` | Zone whose HTTP analytics carry the manifest polls (`helixscreen.org`). Not secret. |

### Local Environment

Create `.env.telemetry` in the project root (gitignored):
```bash
HELIX_TELEMETRY_ADMIN_KEY=your-admin-api-key-here
```

This is auto-loaded by `telemetry-pull.sh`.

## Dashboard

### URL

`https://analytics.helixscreen.org`

### Login

Enter the `ADMIN_API_KEY` value. Stored in browser sessionStorage (clears when tab closes).

### Tabs

Seven tabs, each built around one question. Health is the home tab.

| Tab | Question it answers | Shows |
|-----|---------------------|-------|
| **Health** (`/`) | Is anything getting worse this week? | This week against last week: fleet size (opt-in actives and CDN sources), a scorecard delta per release, new and rising error codes, crash clusters, and a data-health line that flags a point kind whose newest day dropped under half its prior 7-day median (kinds writing under 20 rows a day are never flagged) |
| **Releases** (`/releases`) | Is the newest release better than the one before it? | Scorecard per version (active devices, crash-free devices, crash rate, update failure rate with its failed and attempted counts, print success, p95 frame time, p95 RSS), the adoption curve, devices stuck on old versions. Opens on the newest eight versions with traffic in the range; the picker adds others, its older lines and betas collapsed to one chip as on the charts (a chip holding a shown version says how many) |
| **Stability** (`/stability`) | What crashes and errors, and where? | Crash clusters (signal, fault code, version, board), crash rate by version and platform, error codes by version, memory warnings by platform, connection stability, recent crashes, display anomalies |
| **Performance** (`/performance`) | Is the UI fast enough on each platform? | Frame time p50 and p95 trend, drop rate by platform, p95 frame time and RSS per platform and version, jankiest panels per platform, RSS trend |
| **Fleet** (`/fleet`) | Who runs it, on what? | Platforms, boards, install sources and host mods, printer models, kinematics, MCUs, AMS backends, host RAM, capabilities, CDN fleet, daily actives, weekly retention cohorts over 90 days |
| **Usage** (`/usage`) | What do people use and change? | Panels, overlays and widgets per device, settings changed from default, configured features, themes and locales |
| **Prints** (`/prints`) | Do prints succeed, and with what? | Success rate trend, success by model, version and material set, multi-material share, slicer, thumbnails, print source |

Old routes redirect to the tab that holds their content and keep their query
string: `/adoption` and `/hardware` go to `/fleet`; `/memory`, `/crashes` and
`/reliability` go to `/stability`; `/engagement`, `/features` and `/ux` go to
`/usage`.

All tabs support 7d / 30d / 90d time range selection. Platform, version, printer
model and board filters apply per point kind (see [Filters](#filters)).

### Drill-down

Every step is a URL, so back and forward walk the path and a link reproduces a view.

1. **Click a bar, slice or row** in a distribution. The value becomes a filter
   (platform, version, model or board) in the URL. A grouped bar, such as a
   version line or a canonical model, filters on every raw value in the group.
   A model whose raw name contains a character the filter sanitizer strips
   renders as not filterable.
2. **Devices link**, one link in the page header, opens `/devices?<filters>`: the devices
   matching the current filters, with short id, last seen, version, platform,
   board, model, and sessions, crashes and prints within the range. It is sorted
   by last seen, capped at 200 rows, and shows the true total above the table.
3. **Device page** `/device/:id`: model, platform, board, current version, first
   and last seen within Analytics Engine's 90 days, the latest hardware profile
   and settings snapshot, the version history, and a timeline of the newest 500
   events with each kind's non-empty fields by schema name. The id is the hashed
   device id; the page shows nothing the device did not send.

A breadcrumb above each tab shows the tab and the active filters, each removable. On a
device page it shows the filters, a "Devices" link back to the filtered device list, and
the device's short id. It does not record which tab a drill started from.

### Readability rules

Pure functions in `server/analytics-dashboard/src/utils/`, shared by every chart.

- **Versions.** The two newest `major.minor` lines show each version. Older
  lines collapse to one row (`0.99.x (34 versions)`). Prereleases collapse to
  `1.1.0 betas (9)` until that release exists, then fold into its line. A
  collapsed row expands in place on click.
- **Material sets.** `filament_type` is a `;`-separated slot list. A set is the
  distinct materials, uppercased, sorted and joined with ` + `, so
  `PLA;PLA;PETG;PLA` is `PETG + PLA`. Two or more materials is multi-material.
  Every set is listed; one with fewer than 5 prints (`LOW_COUNT_PRINTS` in
  `utils/materials.ts`) is drawn dimmed, bar and label.
- **Canonical models.** Names are lowercased, a trailing parenthesized variant
  removed, runs of space, `-` and `_` folded, then mapped through a small alias
  table. The label is the most common raw spelling.
- **Every value is shown.** No top-N cut and no "Other" bucket; grouping controls length.
- **Partial days.** Daily series drop the first day of the window and today.
- **Percentages.** Every ratio goes through one formatter; a missing ratio reads `n/a`.
  Update failure, whose denominators are small, shows its counts beside it (`100.0% (1/1)`).
  It counts `update_failed` rows except reason `print_in_progress`, which is the updater
  deferring while a print runs, over failures plus `update_success` rows.
- **Chart height** follows the row count so every label stays legible.
- **Captions.** Each chart states what it counts and over what window.

The Usage, Stability and Performance sections that read the delta point
kinds show a "Waiting for devices on the new client (N so far)" state until
devices on a schema-3 client have reported (see [Delta-era
queries](#delta-era-queries-and-the-waiting-state)).

## Analytics Engine schema table

`server/telemetry-worker/src/schema.ts#POINT_KINDS` declares every Analytics Engine point kind (the
`index1` value) once: its blob and double fields, each with a stable name and a slot
position. Two things read it, so they cannot disagree about where a field lives:

- The writer (`server/telemetry-worker/src/analytics.ts#mapEventToDataPoints`) fills
  the blob and double arrays from the kind's columns.
- Queries name columns with `server/telemetry-worker/src/schema.ts#col` (`col(kind, field)`), never a literal `blobN`.

`server/telemetry-worker/src/schema.ts#EVENT_FANOUT` lists the point kinds each event type writes (`panel_usage` fans out to
`panel_usage`, `widget_interaction` and `overlay_visit`; `screensaver_usage` to one
`screensaver_usage` point per saver and one `screensaver_level` point per saver and settled
level; `settings_snapshot` to `settings_snapshot` and `widget_placement`; `settings_changes`
to one `settings_change` point per change). `server/telemetry-worker/src/schema.ts#R2_ONLY` lists fields kept out of Analytics Engine on purpose.

**Slots are append-only.** Analytics Engine rows are read by position for 90 days, so a
field never changes its `blobN` / `doubleN`. A new field takes the next unused slot of its
own kind (blobs 13-20, doubles 9-20; delta kinds already hold blob 13 and doubles 9-10);
a retired field leaves its slot empty. `server/telemetry-worker/src/__tests__/schema.test.ts`
("keeps every column in its slot") pins every existing position.

To add a field: add a `blob(slot, field)` or `dbl(slot, field)` entry to the kind, add a
reader if the event nests it, add the key to the client builder, and use `col(kind, field)`
in the query. A client field the table neither maps nor lists in `R2_ONLY` never reaches
a query.

**The client and the table are checked against each other.** Each column records the client
fields it is written from (`ColumnDef.paths`: the field itself, or what its reader reads).
`tests/fixtures/telemetry/<event>.json` holds one event of each type as the client builds
it, and two tests share those files:

- `tests/unit/test_telemetry_contract.cpp` (`[telemetry][contract]`) rebuilds every event
  from fixed inputs and compares it to its file. Values from the clock, the host or the
  build (timestamps, ids, memory figures, version strings) are pinned to placeholders.
  After a deliberate client change, `HELIX_UPDATE_GOLDENS=1 ./build/bin/helix-tests
  "[telemetry][contract]"` rewrites the files.
- `server/telemetry-worker/src/__tests__/contract.test.ts` requires every field in each file
  to match a column path of the event's kinds or an `R2_ONLY` entry, and every `R2_ONLY`
  entry to match a field the event carries.

So any change to what the client sends fails the golden test until its file is rewritten.
The rewritten file then fails the worker test if it carries a field the table neither maps
nor lists in `R2_ONLY`, or if an `R2_ONLY` entry names a field the client no longer sends.

`print_outcome`, `print_start_context`, `error_encountered`, `connection_stability`,
`hardware_profile` and the delta kinds keep their original slots and carry `app_platform` and `app_board` in
slots 13 and up, so rows written before those fields were added read empty there.

## Filters

Dashboard requests take `platform`, `version`, `model` and `board` (comma-separated
values). `server/telemetry-worker/src/queries.ts#filterFor` resolves
each one to that kind's own column through the schema table:

| Filter | Field read | Notes |
|--------|-----------|-------|
| `platform` | `app_platform` | |
| `version` | `app_version` | `update_failed` reads `from_version`: the version the device was running |
| `model` | `printer_model` | Only `session` and `hardware_profile` report it |
| `board` | `app_board` | |

A kind that lacks the filtered field is excluded (`AND 0`) rather than matched against
whatever column shares that slot. A query over several kinds gets
`((index1='a' AND blobX IN (...)) OR (index1='b' AND blobY IN (...)))` from
`server/telemetry-worker/src/queries.ts#filterForKinds`, with excluded kinds dropped from the OR.

When a filter excludes every kind an endpoint reads, the endpoint's `filter_not_applicable`
names it and the view says "Not reported per printer model" (or platform, version,
board) instead of showing 0. Endpoints whose blocks read different kinds say it per block
in `not_applicable`: `health` (crashes, errors, prints, updates), `releases` (crashes,
prints, updates, performance), `stability` (crashes, errors), `prints` (prints) and
`devices` (the crashes and prints columns). Under such a filter a crash rate or crash-free
share is `null`, never 0% or 100%. Filter values keep spaces, so `model=Voron 2.4` matches; only
characters outside letters, digits, space and `. - _ ( ) / +` are stripped.

### Print outcomes per version and model

`print_outcome` rows carry no `printer_model`, and rows from current clients carry an empty
`app_version`. Print success per model (Prints), per version (Prints, the Releases scorecard
and the Health release windows) therefore joins through sessions in TypeScript:
`server/telemetry-worker/src/queries.ts#releasePrintsSql` returns weighted outcomes per
device and per the version the rows carry, `server/telemetry-worker/src/queries.ts#deviceSessionsSql`
each device's latest session model and version before the window ends, read over all 90
days, and `server/telemetry-worker/src/prints.ts#attributePrints` folds them. A session is
sent only on connect, so a device connected all week has none inside the window; it cannot
change version without restarting, which sends one, so its latest session before the
window's end names the version it ran. A row's own version wins; an empty one takes its
device's session version; a device with no session in 90 days lands under `unknown`
(model `Unknown`), which per-version rates leave out. The Health previous week reads only
sessions written before that week ended. A `version` filter still reads
the print rows' own column, so it drops rows with an empty version.

## Weighted counts

Analytics Engine samples rows under load and records, in `_sample_interval`, how many
rows each stored row stands for. Every count and sum in `queries.ts` is weighted by it:

| Plain SQL | Used here |
|-----------|-----------|
| `count()` | `sum(_sample_interval)` (`server/telemetry-worker/src/queries.ts#wCount`) |
| `sum(x)` | `sum(x * _sample_interval)` (`server/telemetry-worker/src/queries.ts#wSum`) |
| `avg(x)` | `sum(x * _sample_interval) / sum(_sample_interval)` (`server/telemetry-worker/src/queries.ts#wAvg`) |

`count(DISTINCT ...)` stays as it is: a device is one device however it was sampled.
Delta queries weight by `delta_weight`, the `_sample_interval` of the deduplicated row.
Quantiles use `quantileExactWeighted(q)(x, _sample_interval)`.

The time axis is Analytics Engine's own write time (`timestamp`). The client's
`timestamp` field is never used for bucketing: uploads lag by hours and a device without
a real-time clock reports arbitrary dates.

## Delta snapshots and the idempotency key

`panel_usage`, `connection_stability` and `screensaver_usage` report what accumulated in
one window, not lifetime totals. Each event carries:

- `boot_id`: 16 random hex characters, new for every process start and never stored on
  the device
- `snapshot_seq`: 0, 1, 2... per boot
- `schema_version`: 3, which marks a delta-era client

`session_duration_sec` on a delta event is the length of the window it covers, so a boot's
length is the sum over its rows. `longest_disconnect_sec` is the longest outage of the
window; an outage that spans snapshots keeps its start time, so the maximum over rows is
the real one.

The idempotency key is `(device_id, boot_id, snapshot_seq)` (plus the panel, widget or
overlay id for fan-out kinds). A lost response makes the client resend a batch, and the
worker stores the resend as a second row; queries remove it (below) rather than the
ingest path trying to.

Client delivery (`src/system/telemetry_manager.cpp#fire_periodic_snapshot`, `#on_auto_send_tick` and `#enqueue_event`):

- Each periodic snapshot (`kSnapshotIntervalMs`, 4h) saves the event queue and the
  unsent accumulator together.
- The accumulator is also rewritten hourly by the auto-send timer. After an unclean exit the next boot sends it
  under its original `boot_id` and `snapshot_seq`, so the window is neither lost nor
  counted twice.
- At the queue cap (`kMaxQueueSize`) a new delta first tries to merge into the newest
  queued row of the same kind and the same `boot_id` that no send has attempted, adding
  counts and extending the window. Rows a send has attempted are never merge targets, since
  the server may already hold them. With no merge target, the oldest non-delta event is
  evicted instead. Only when the queue holds nothing evictable is the incoming delta
  dropped, with a warning that its window is lost.

Rows from clients before schema 3 hold lifetime totals that were resent every 4h and
carried over unclean exits. They are excluded from delta queries, not salvaged.

### Delta-era queries and the waiting state

`server/telemetry-worker/src/queries.ts#deltaSource` is
the one subquery every delta aggregate reads:

1. Keep only rows with `schema_version >= 3`.
2. Group by the key (`device_id`, `boot_id`, `snapshot_seq`, plus dims) so a resend
   collapses to one row. Each metric takes `argMax(metric, timestamp)`, the write time is
   `min(timestamp)`, and the weight is the `_sample_interval` of the kept row.
3. Aggregate over that result with the delta weight.

Endpoints over delta kinds (`engagement`, `reliability`, `stability`, `ux`, `features`)
return `delta_era_devices`, the distinct devices with schema-3 rows of those kinds in the
window. While it is 0 every value derived from deltas is `null`, and `features` is an
empty list; the dashboard renders `null` as "Waiting for devices on the new client (N so
far)". The fields are typed `number | null` in `server/analytics-dashboard/src/services/api.ts`.
If a filter excludes the delta kinds outright, `filter_not_applicable` is set and the
placeholder reads "Not reported per <filter>" instead.

### Feature adoption

The client sends no `feature_adoption` event. Adoption is derived: a feature counts as adopted on
a device when its overlay or widget has a count above 0 in the window, over
`delta_era_devices` (`server/telemetry-worker/src/index.ts#deriveFeatureAdoption`, served at `/v1/dashboard/features`). Overlays are
identified by the overlay root's object name, falling back to the lifecycle name only when
the root has none; widgets by their id. Flags with no overlay or widget to visit are
fields on `settings_snapshot` (see below), reported beside the derived adoption. The `feature_adoption` point kind stays in the
schema table so rows stored by older clients remain readable.

## Smoke test

`server/telemetry-worker/scripts/smoke.ts` hits every `/v1/dashboard/` endpoint across the
ranges `7d`, `30d` and `90d` with no filter, then `platform=pi`, the newest version and top model
from `adoption`, and `board=rpi4`. It also requests `releases?versions=` with the two newest
versions from `adoption`, then `devices`, `device/<first id>`, and checks that
`device/<64 zeros>` is 404 and `device/xyz` is 400. It prints a PASS/FAIL table and exits non-zero on
any unexpected status. Run it after every worker deploy:

```bash
(cd server/telemetry-worker && npx vite-node scripts/smoke.ts [base-url] [--delay-ms N])
```

The base URL defaults to `https://telemetry.helixscreen.org`. The admin key comes from
`HELIX_TELEMETRY_ADMIN_KEY` or `.env.telemetry`, as for `reconcile.ts`. A 200 proves the
endpoint's SQL runs under every filter; it does not prove the numbers are right.

Requests run one at a time with a pause between them (`--delay-ms`, default 300), because a
full run sent back to back trips Analytics Engine's rate limits and returns 502s. A 5xx is
retried once after 5 seconds; a pass on that retry reads `PASS (retry)`.

## Reconciling AE against R2

R2 keeps every accepted batch; Analytics Engine can shed or sample writes, so a
day's AE rows may be fewer than the points its R2 events map to. Two pieces measure
the gap for one UTC write day:

- `GET /v1/admin/ae-counts?date=YYYY-MM-DD` (admin key) returns, per point kind,
  `rows` (raw `count()`), `weighted` (`sum(_sample_interval)`) and `devices`
  (`count(DISTINCT device_id)`).
- `server/telemetry-worker/scripts/reconcile.ts` compares that with R2:

```bash
(cd server/telemetry-worker && npx vite-node scripts/reconcile.ts 2026-10-12)
```

The script reads `HELIX_TELEMETRY_ADMIN_KEY` from the environment, else from
`.env.telemetry` in the repo root. It lists and fetches that day's R2 batches over
the admin API, maps them with the worker's own `mapEventToDataPoints`, calls
`ae-counts` for the same day, and prints one row per kind. The R2 side runs
locally because a day is hundreds of objects, near the Worker's per-invocation
subrequest cap, and grows with the fleet.

| Column | Meaning |
|--------|---------|
| `r2_points` | Analytics Engine points the day's R2 events map to |
| `ae_rows` | Raw `count()` of rows AE holds for the kind |
| `ae_weighted` | `sum(_sample_interval)`: rows after correcting for AE sampling |
| `kept%` | `ae_weighted / r2_points`; well below 100 means AE dropped writes |

`ae_rows` next to `ae_weighted` shows how much AE sampled; `r2_points` next to
`ae_weighted` shows how much was lost outright. If `ae_weighted` matches
`r2_points`, the weighted queries above already account for the gap. If it stays
below, rows were dropped on write: investigate the write path rather than
adjusting a query. Lost AE rows are not re-written (there is no backfill:
`writeDataPoint` takes no timestamp, so a replay would land on the wrong day and
duplicate rows AE already holds). R2 stays the source of truth for the day.

## Worker API Endpoints

### Ingest (client binary)

```bash
POST /v1/events
X-API-Key: <INGEST_API_KEY>
Content-Type: application/json

[{ "schema_version": 3, "event": "session", "device_id": "...", "timestamp": "...",
   "app_version": "...", "app_platform": "...", "app_board": "...", "boot_id": "...", ... }]
```

- Batch of 1-20 events per request
- Validation checks only `schema_version` (number), `event`, `device_id` and `timestamp`;
  the rest of the envelope is not enforced, so older clients keep ingesting
- Rate limited: 10 requests/min per IP
- Dual-writes to R2 (permanent) + Analytics Engine (90-day)

### Admin Endpoints (all require `X-API-Key: <ADMIN_API_KEY>`)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/v1/events/list?prefix=events/2026/01/` | GET | List R2 event files |
| `/v1/events/get?key=events/2026/01/15/...json` | GET | Download a single event file |
| `/v1/admin/ae-counts?date=2026-10-12` | GET | Per-kind Analytics Engine row counts for one UTC day (`kind`, `rows`, `weighted`, `devices`) |
| `/v1/dashboard/overview?range=30d` | GET | Dashboard overview metrics |
| `/v1/dashboard/adoption?range=7d` | GET | Adoption/distribution metrics |
| `/v1/dashboard/prints?range=30d` | GET | Print reliability metrics |
| `/v1/dashboard/crashes?range=30d` | GET | Crash analytics |
| `/v1/dashboard/releases?versions=v0.9.18,v0.9.19` | GET | Per-version comparison |
| `/v1/dashboard/{crash-list,memory,memory-warnings,hardware,performance}` | GET | The remaining sections' data |
| `/v1/dashboard/{engagement,reliability,stability,ux,features}` | GET | Delta-era data: return `delta_era_devices` and `filter_not_applicable`, and `null` values while no schema-3 device has reported. `stability` also returns `crash_clusters` and `errors_by_version` |
| `/v1/dashboard/health` | GET | Health tab: this week against last week (`cur` and `prev`), release scorecards, error codes, crash clusters, data health |
| `/v1/dashboard/reports`, `/v1/dashboard/reports/<YYYY-Www>` | GET | The stored weekly reports: the list, and one week |
| `/v1/admin/loop/run?mode=daily\|weekly[&dry=0]` | POST | Run the feedback loop; dry unless `dry=0` (see Feedback loop) |
| `/v1/dashboard/devices` | GET | Devices matching the filters (200 rows, true `total`) |
| `/v1/dashboard/device/<64 hex id>` | GET | One device's page; 400 for any other id, 404 for an unknown one |
| `/v1/dashboard/fleet/retention` | GET | Weekly cohorts over 90 days |

`releases` takes an optional `versions` list and defaults to the eight versions with the most active devices in the range.

Dashboard endpoints take `range` (`7d`, `30d`, `90d`) and the `platform`, `version`,
`model` and `board` filters.

### Other

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check; reports `git_sha`, the commit the worker was deployed from (`unknown` if deployed without `GIT_SHA`) |
| `/v1/symbols/:version` | GET | List symbol files for crash resolution |

## Feedback loop

The loop turns the Health data into three outputs: an email (a weekly report and daily alerts), GitHub issues for patterns worth fixing, and printer-database candidate issues. It reads the same Health payload as the dashboard's Health tab, so what the email says is what the tab shows. The code is `server/telemetry-worker/src/loop/`.

### What runs when

| When (UTC) | What | Where |
|------------|------|-------|
| Daily 13:00 | Alert check: email the findings that carry an alert and were not alerted in the last `ALERT_REPEAT_DAYS` | Worker cron (`wrangler.toml` `[triggers]`, dispatched in `scheduled()` in `src/index.ts`) |
| Mondays 13:00 | Weekly run, after the daily one: snapshot the ISO week that just closed, sync its issues, email the summary | Same cron |
| Mondays 14:00 | Printer-database candidate issues | `.github/workflows/telemetry-printers.yml` |

The 02:10 cron is the unrelated CDN snapshot. Each loop run is isolated: a failure is logged as `[loop] ...` and neither stops the other run nor fails the cron.

### Findings and thresholds

`findings.ts` turns the Health payload and the update-failure groups into findings. Each finding has a kind (crash cluster, error code, update failure, release regression, data health, printer), a fingerprint, and the actions it earns: every finding appears in the report, some also raise an alert or file an issue. Every number that decides this lives in `server/telemetry-worker/src/loop/thresholds.ts`, with a comment each; change them there and nowhere else. That file also holds the cap on new issues per run (the rest wait for the next run) and the alert repeat interval.

A crash cluster finding is one (signal, fault code, version), summed across boards: the Health payload splits a cluster by board, and since a device reports one board, the per-board device counts add up exactly. The finding lists its boards, files at `CLUSTER_ISSUE_DEVICES` summed devices, and alerts only when no board saw the crash the week before. A run handles each fingerprint once. Its issue body names the prefix (`<SIGNAL>/<version>/`) that the crash worker's fingerprint gives matching crash-report issues, with a `label:crash` search for it, so a report can be linked by hand.

### Fingerprints and dedupe

A fingerprint is `<kind>:` plus the identifying parts joined with `|` (`fingerprint.ts`). Each part comes from devices, so it is sanitized first: whitespace collapsed and trimmed, lowercased, anything outside `[a-z0-9._:/+-]` replaced by `_`, cut to 64 characters. Device-supplied values in issue titles and tables go through one escaper (`codeSpan`, `issueTitle` in `issues.ts`): one line, `|` escaped, backticks replaced, `@` unable to mention. The Python producer applies the same rules, and `tests/fixtures/telemetry_issue_vectors.json` holds the vectors both test suites assert. Issues carry it in the body as `<!-- telemetry-fp: ... -->`. The Worker finds an issue by listing every `telemetry` issue, open and closed, once per run (`GET /repos/{repo}/issues?labels=telemetry&state=all`, following the `Link` header, pull requests skipped) and matching the marker exactly in each body; when two issues carry one marker, the open one wins, then the lowest number. GitHub search is not used: it is eventually consistent, so an issue filed seconds earlier in the same run would be missed. An issue the run creates joins that index at once. Before its first create in a run the Worker creates any missing label (`telemetry`, `crash`, `error-code`, `update-failure`, `printer-db`); a created issue that comes back without `telemetry` is listed in the run's `skipped`, since the next run's lookup cannot see it. An open issue gets at most one count comment per ISO week. A closed issue is reopened with a comment when its finding returns. Alerts are deduped by the fingerprint's last-emailed date in the loop state.

Issue titles and bodies carry counts and a dashboard link, never device ids.

### Reports

The weekly run stores its report as `reports/<YYYY-Www>.json` in the telemetry R2 bucket (`src/loop/report.ts`): the Health payload, the findings and the issue outcomes. A stored report is immutable. The dashboard serves them at `/reports` and `/reports/<week>`, backed by `GET /v1/dashboard/reports` and `GET /v1/dashboard/reports/<YYYY-Www>`. The email links to the report page.

### Loop state

The R2 object loop/state.json in the same bucket holds two maps: fingerprint to the date it was last alerted, and fingerprint to the ISO week of its last issue comment. A missing or unparsable file reads as empty, which only means alerts and comments may repeat once. An alert is recorded only after its email went out, so a failed send retries the next day.

### Printer candidates

`scripts/telemetry-printer-issues.py` reads the output of `scripts/telemetry-printer-profiles.py --json --candidates` and files one issue per unrecognized hardware cluster with a draft `printer_database.json` entry (built by `scripts/telemetry-update-printer-db.py`). It uses the same fingerprint form as the Worker, with kind `printer`. It looks issues up by listing `label:telemetry` issues and matching the marker locally. The device threshold and the new-issue cap are constants at the top of the script and mirror `PRINTER_ISSUE_DEVICES` and `NEW_ISSUES_PER_RUN`.

The workflow pulls the last 28 days of telemetry with `HELIX_TELEMETRY_ADMIN_KEY` and skips itself when that secret is unset. A scheduled run files for real only when the repository variable `TELEMETRY_PRINTER_ISSUES_LIVE` is `true`; otherwise it prints what it would file. A manual `workflow_dispatch` is dry unless `dry_run` is set to false. A printer issue is closed when its database entry lands, and unlike the Worker's issues it is not reopened.

### Release check

`scripts/release-health.sh` compares the newest prerelease of the `VERSION.txt` release seen in the adoption list with the current stable in telemetry and prints a table plus a `WARN` line per metric that is past its threshold. `scripts/release-gate.sh fast` runs it and its row shows the warning count (`PASS ... (N warnings)`); it never fails the gate, and prints one line saying why when there is nothing to compare. It reads the admin key from `HELIX_TELEMETRY_ADMIN_KEY` or `.env.telemetry`. See `docs/devel/RELEASE_PROCESS.md`.

### Operating it

**Secrets and variables.** Worker secrets, set with `wrangler secret put`:

| Name | Purpose |
|------|---------|
| `RESEND_API_KEY` | Sends the weekly report and alerts. Unset: the run reports `email: not configured` in `skipped` and sends nothing. |
| `GITHUB_APP_PRIVATE_KEY` | Private key of the GitHub App (`GITHUB_APP_ID` in `wrangler.toml`) that files issues. Unset (or `GITHUB_REPO` / `GITHUB_APP_ID` unset): the weekly run skips issue sync, lists one `github: not configured` in `skipped`, and the email and stored report say issues were not synced that week. |

The recipient, sender, repository and dashboard URL are plain vars in `wrangler.toml` (`NOTIFICATION_EMAIL`, `EMAIL_FROM`, `GITHUB_REPO`, `DASHBOARD_URL`). In the GitHub repository, set the Actions secret `HELIX_TELEMETRY_ADMIN_KEY` (same value as the Worker's `ADMIN_API_KEY`) and, when ready for real issues, the Actions variable `TELEMETRY_PRINTER_ISSUES_LIVE=true`.

**Dry run.** The admin route runs the loop with real reads and returns what it would do. Dry is the default: nothing is emailed, filed or written. Pass `dry=0` to act.

```bash
curl -s -X POST -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
  "https://telemetry.helixscreen.org/v1/admin/loop/run?mode=weekly" | jq
curl -s -X POST -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
  "https://telemetry.helixscreen.org/v1/admin/loop/run?mode=daily&dry=0"   # sends for real
```

The response carries `findings`, `issues` (each `created`, `commented`, `reopened`, `unchanged` or `held`), the built `email`, `alertsSent` and `skipped`. `mode` is `daily` or `weekly`.

**First run.** Deploy, then dry-run `weekly` and `daily` and read the issues and email they would produce. Fix thresholds if they are noisy. Set the secrets, run `weekly&dry=0` once, and check the issues, the email and the stored report. Leave `TELEMETRY_PRINTER_ISSUES_LIVE` unset until a manual `workflow_dispatch` dry run of the printer workflow looks right.

**Re-running a week.** The weekly run returns a stored report unchanged (`reused: true`) and does not file, comment or email again. To regenerate a week, delete its report first, from `server/telemetry-worker` (the bucket is in the EU jurisdiction): `npx wrangler r2 object delete helixscreen-telemetry/reports/<YYYY-Www>.json --remote -J eu`. Then run `weekly&dry=0`. The week is the ISO week before the run's date. If the weekly run fails to store its report (the run logs `weekly run failed` after its issues were already filed or commented on, and sends no email), nothing scheduled recovers that week: the next Monday's run covers the following week. Recover it by hand with `weekly&dry=0` before that Monday; the issue state was saved, so it does not comment twice.

**Silencing a finding.** Closing a telemetry issue only silences a finding that has gone away: while the finding is still over its threshold, the next weekly run reopens the issue with a comment. To silence a persistent finding, fix it or raise its threshold in `thresholds.ts`. Alerts have no switch; each repeats at most every `ALERT_REPEAT_DAYS`. Printer-database issues are the exception: a closed one stays closed.

**Where things live.** State and reports are in the `helixscreen-telemetry` R2 bucket under `loop/` and `reports/`. Cron and run failures log with a `[loop]` prefix in the Worker logs (`wrangler tail`).

## Scripts

All scripts live in `scripts/` and auto-load `.env.telemetry`.

### telemetry-pull.sh

Pull raw event data from R2 to local disk for offline analysis.

```bash
# Pull last 30 days (default)
./scripts/telemetry-pull.sh

# Pull specific date range
./scripts/telemetry-pull.sh --since 2026-01-01 --until 2026-02-01

# See what would be downloaded
./scripts/telemetry-pull.sh --dry-run
```

Downloaded to `.telemetry-data/events/` (gitignored).

### telemetry-analyze.py / telemetry-crashes.py

Offline Python analysis of pulled event data. Requires `.venv/`:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r scripts/telemetry-requirements.txt

# General analytics
python3 scripts/telemetry-analyze.py .telemetry-data/events/

# Crash analysis
python3 scripts/telemetry-crashes.py .telemetry-data/events/
```

## Deployment

### Worker

```bash
cd server/telemetry-worker
npx vitest run
wrangler deploy --var GIT_SHA:$(git rev-parse --short HEAD)
curl -s https://telemetry.helixscreen.org/health   # git_sha must match the commit just deployed
```

Always pass `GIT_SHA`: a deploy without it reports `"git_sha": "unknown"`, and then
nothing says which code is serving ingest. Deploys are manual and confirmed with the
maintainer first. A schema-table change deploys before any client that writes the new
fields, and a worker query change needs no client release.

### Dashboard

```bash
cd server/analytics-dashboard
npx vue-tsc --noEmit -p tsconfig.app.json   # the typecheck that actually checks
npm run build
wrangler pages deploy dist --project-name=helixscreen-analytics
```

`tsconfig.json` here only holds a project reference (`"files": []`), so the build script
passes `-p tsconfig.app.json` to type-check the `src/` tree.

### Order and checks

The worker and the dashboard Pages project deploy together: the `/v1/dashboard/features`
response shape is `features[].id` plus `configured`, with no `total_devices` or `by_version`,
so either half alone breaks the Usage tab. After deploying, `/health` `git_sha` must match.

Before the first deploy of a SQL change, run the Analytics Engine smoke below with a read
token (or a staging worker). A failure shows as a 502 on that endpoint.

1. `GET /v1/admin/ae-counts?date=<yesterday>`: `count() AS rows`, `sum(_sample_interval) AS weighted`, `toDate(timestamp) = 'YYYY-MM-DD'`.
2. Phase 0 decision: `server/telemetry-worker/scripts/reconcile.ts <day>` compares expected and AE counts per kind. Weighted counting is safe either way (it equals `count()` when unsampled), but if AE is still short when weighted, investigate the write path before trusting any number.
3. A filter-excluded kind: `... AND 0` in WHERE (overview with `model=X`: print success, total events).
4. Releases with no usable versions: `WHERE index1 IN (...) AND 0`.
5. Multi-kind OR filter: overview total events with `version=X` (about 19 `(index1 = 'k' AND blobN IN (...))` branches) and crash rate with `platform=X`.
6. `sumIf(_sample_interval, cond)` and `sum((expr) * _sample_interval)`, and ratios of them (prints, overview).
7. `deltaSource`: `GROUP BY blob1, blob13, double10[, blob4]` (a double in the GROUP BY), `argMax(doubleN, timestamp)`, `argMax(_sample_interval, timestamp)`, `min(timestamp) AS written_at`.
8. Outer queries over `deltaSource`: `toDate(written_at)` grouping, `count(DISTINCT boot_id)`, `count(DISTINCT device_id)` on subquery aliases (engagement, reliability, features).
9. Per-device subqueries exposing `argMax(_sample_interval, timestamp) AS device_weight` with outer `sum((toUInt32(bitAnd(...)) > 0) * device_weight)` (hardware caps) and `wAvg` over it (build volume, fan/sensor/macro, RAM).
10. `HAVING total > 0 AND dropped / total > 0.05` where both are weighted-sum aliases (performance query 5).
11. `double9 >= 3` on rows that never wrote double9, and reads of blob13-15 on rows written with 12 blobs: expect 0 / `''`, not an error or a NULL that breaks `!= ''`.
12. `server/telemetry-worker/scripts/smoke.ts` (see [Smoke test](#smoke-test)): every dashboard endpoint across ranges and filters returns 200.
13. Ingest round trip with a throwaway device id: post one schema-3 `panel_usage` and `connection_stability`, then read back `blob13, double9, double10` for that device; post the same batch again and confirm the engagement numbers for that device do not double. This writes two test rows into production AE; use staging if that is not acceptable.
14. Deploy worker and dashboard Pages together (see above); `/health` `git_sha` matches.

After the deploy, once a full UTC day has been written, run `server/telemetry-worker/scripts/reconcile.ts` for
yesterday's UTC date and confirm expected and AE counts agree per kind.

### Creating the Analytics Engine API Token

If `HELIX_ANALYTICS_READ_TOKEN` needs to be recreated:

1. Go to [Cloudflare API Tokens](https://dash.cloudflare.com/profile/api-tokens)
2. **Create Token** → **Create Custom Token**
3. Name: `helixscreen-analytics-read`
4. Permissions: Account → **Analytics** → **Read**
5. Account Resources: Include → your account
6. Create, then: `cd server/telemetry-worker && wrangler secret put HELIX_ANALYTICS_READ_TOKEN`

## Data Retention

- **R2**: Permanent. Raw event JSON files partitioned by date.
- **Analytics Engine**: 90-day rolling window. Dashboard queries only see the last 90 days.
- **Offline**: Run `telemetry-pull.sh` periodically to archive data locally beyond 90 days.

## Event Types

### Envelope

Every event is built through `src/system/telemetry_manager.cpp#stamp_envelope`,
so these fields are on all of them and builders never set them by hand:

| Field | Meaning |
|-------|---------|
| `schema_version` | `3` marks a delta-era client; `2` and earlier are older clients |
| `app_version` | HelixScreen version that sent the event |
| `app_platform` | Platform key (`mips` and similar are shared by several printers) |
| `app_board` | Resolved printer board, which splits a shared platform key |
| `boot_id` | 16 random hex characters, new per process start, never stored on the device |

Plus the per-event `event`, `device_id` (double-hashed) and `timestamp`. The update events
also carry their own `version` (the release moved to) and `from_version`; the worker maps
those explicitly, so the stamped `app_version` never overwrites the update's target.

### Events

| Event | Description | Key Fields |
|-------|-------------|------------|
| `session` | App startup | platform, board, version, printer_model, kinematics, display, ram, cpu_cores |
| `print_outcome` | Print completed/failed | outcome, filament_type, duration, temps, filament_used |
| `crash` | App crash | signal_name, version, platform, board, uptime, backtrace_depth |
| `update_failed` | Update attempt failed | reason, version, from_version, platform, http_code |
| `update_success` | Update completed | version, from_version, platform |
| `memory_snapshot` | Periodic memory check | trigger, uptime_sec, rss_kb, vm_size_kb, vm_peak_kb, vm_hwm_kb, private_dirty_kb, private_clean_kb, shared_clean_kb, pss_kb, system_total_mb, system_available_mb |
| `memory_warning` | Memory threshold breach | level, reason, rss_kb, vm_size_kb, vm_hwm_kb, vm_swap_kb, system_total_mb, system_available_mb, growth_5min_kb, private_dirty_kb, pss_kb |
| `hardware_profile` | Hardware/feature inventory | printer.*, mcus.*, extruders.*, fans.*, steppers.*, leds.*, sensors.*, probe.*, capabilities.*, ams.*, tools.*, macros.*, printer_objects[] |
| `settings_snapshot` | User configuration | theme, theme_name, brightness_pct, screensaver_timeout_sec, screen_blank_timeout_sec, screensaver_type, locale, sound_enabled, auto_update_channel, animations_enabled, time_format, home_widgets[], spoolman_configured, theme_changed, multi_printer |
| `panel_usage` | Navigation delta for one window | boot_id, snapshot_seq, session_duration_sec (window length), panel_time_sec.*, panel_visits.*, widget_interactions.*, overlay_visits.*, overlay_open_count |
| `screensaver_usage` | Screensaver delta for one window | boot_id, snapshot_seq, saver_shown_sec.*, saver_starts.*, saver_woke_by_touch.*, saver_stopped_other.*, saver_too_heavy.*, saver_settled_levels.* |
| `connection_stability` | Connection delta for one window | boot_id, snapshot_seq, session_duration_sec, connect_count, disconnect_count, total_connected_sec, total_disconnected_sec, longest_disconnect_sec, klippy_error_count, klippy_shutdown_count |
| `print_start_context` | Print metadata at start | source, has_thumbnail, file_size_bucket, slicer, ams_active |
| `error_encountered` | Rate-limited error log | category, code, context, uptime_sec |
| `performance_snapshot` | Frame-time percentiles and dropped-frame counts for the window | frame_time_p50/p95/p99_ms, dropped_frame_count, total_frame_count, worst_panel |
| `settings_changes` | Batched setting diffs | changes[]: setting, old_value, new_value |
| `async_lifetime_skips` | Deferred async work skipped at shutdown | skips[], window stats |

### Event Trigger Summary

| Event | When | Frequency |
|-------|------|-----------|
| `session` | After printer discovery | Once per launch |
| `hardware_profile` | After printer discovery | Once per launch |
| `settings_snapshot` | After printer discovery | Once per launch |
| `memory_snapshot` | Session start + hourly timer | ~1/hour |
| `memory_warning` | Memory threshold breach | Rate-limited: 1/level/5min |
| `panel_usage` | Periodic snapshot (4h) and app shutdown | ~every 4h; each carries only its own window |
| `connection_stability` | Periodic snapshot (4h) and app shutdown | ~every 4h; each carries only its own window |
| `screensaver_usage` | Periodic snapshot (4h) and app shutdown | ~every 4h, only for a window in which a saver ran |
| `print_outcome` | Print reaches terminal state | Per print |
| `print_start_context` | Print starts (metadata callback) | Per print |
| `error_encountered` | On non-fatal error | Rate-limited: 1/category/5min |
| `performance_snapshot` | Hourly timer and app shutdown | ~1/hour |
| `settings_changes` | Debounced after a setting change | ~30s after the last change |
| `async_lifetime_skips` | Periodic snapshot and app shutdown | With each `panel_usage` |
| `crash` | Next boot after crash | Once per crash |
| `update_failed` | Update failure | Per failure |
| `update_success` | Next boot after update | Once per update |

`panel_usage`, `connection_stability` and `screensaver_usage` are the delta events: see [Delta snapshots and
the idempotency key](#delta-snapshots-and-the-idempotency-key). The client sends no `feature_adoption` event. Adoption is derived from the `overlay_visit` and `widget_interaction` points
that `panel_usage` fans out to (see [Feature adoption](#feature-adoption)).

### settings_snapshot adoption flags

Three booleans cover features with no overlay or widget to visit:

| Field | True when |
|-------|-----------|
| `spoolman_configured` | Moonraker reports Spoolman available |
| `theme_changed` | The active theme is not the default |
| `multi_printer` | The config holds more than one printer |

Each is a tri-state `settings_snapshot` column (blobs 13-15): `"1"`, `"0"`, or empty for a
client too old to send it. The Features view lists them under `configured`: devices that
reported the flag on over devices that reported it at all
(`server/telemetry-worker/src/index.ts#configuredFeature`).

### screensaver_usage Event Details

`ScreensaverManager` accumulates the window on the main thread (`include/screensaver_usage.h`,
`helix::SaverUsage`) and `TelemetryManager` reads and resets it through
`set_screensaver_usage()`; the saver code has no telemetry dependency. Every map is keyed by
the saver's stable registry name (`kScreensavers` in `include/screensaver_registry.h`, the same
name `settings_snapshot.screensaver_type` carries), plus `black_screen` for the static screen
shown when every saver is too heavy for the board. A Test-button preview is not counted.

| Field | Meaning |
|-------|---------|
| `saver_shown_sec` | Seconds on screen in the window; an open run counts up to the snapshot, sub-second remainders carry into the next window |
| `saver_starts` | Runs started in the window, the fallback that replaces a too-heavy saver included |
| `saver_woke_by_touch` | Runs ended by user input |
| `saver_stopped_other` | Runs ended any other way: display sleep, an app wake (print done, remote control), a switch of saver, shutdown |
| `saver_too_heavy` | Runs the gate ended over budget at the lowest level, handing over to the fallback |
| `saver_settled_levels` | Ended runs per `"<name>:<level>"`, the gate level the run ended at (0 is the full effect); the black screen has none |

Each ended run counts in exactly one of the three ending maps. A run spanning a snapshot counts
its start in one window and its ending in a later one. The worker fans the five per-saver maps
out to one `screensaver_usage` point per name (`shown_sec`, `starts`, `woke_by_touch`,
`stopped_other`, `too_heavy`) and `saver_settled_levels` to one `screensaver_level` point per
key (`saver_name`, `level`, `runs`), both with the delta columns. No dashboard view reads them
yet.

### hardware_profile Event Details

Recorded once per session after printer discovery. Contains a full hardware inventory including name-level data for printer detection analysis.

**Name arrays** (for detection heuristic development):

| Field | Source | Cap | Description |
|-------|--------|-----|-------------|
| `fans.names[]` | `PrinterDiscovery::fans()` | 200 | Fan object names (e.g., `"heater_fan hotend_fan"`) |
| `sensors.temperature_names[]` | `PrinterDiscovery::sensors()` | 200 | Temperature sensor names (e.g., `"chamber"`, `"tvocValue"`) |
| `sensors.filament_names[]` | `PrinterDiscovery::filament_sensor_names()` | 200 | Filament sensor names |
| `leds.names[]` | `PrinterDiscovery::leds()` | 200 | LED/neopixel object names |
| `steppers.names[]` | `PrinterDiscovery::steppers()` | 200 | Stepper motor names |
| `macros.names[]` | `PrinterDiscovery::macros()` | 200 | G-code macro names |
| `printer_objects[]` | `PrinterDiscovery::printer_objects()` | 500 | Full Klipper object list |

These names are firmware/config-defined identifiers (no PII). Hostnames are explicitly excluded.

**Analysis:** Use `scripts/telemetry-printer-profiles.py` to analyze hardware profiles — finds discriminating names per detected model, clusters undetected printers, and generates candidate heuristics for `config/printer_database.json`.

### memory_warning Event Details

Fired when `MemoryMonitor` detects a threshold breach. Rate-limited to at most one event per pressure level per 5 minutes.

**Pressure levels:**

| Level | Meaning | Log Level |
|-------|---------|-----------|
| `elevated` | RSS growth exceeding 5-min threshold | INFO |
| `warning` | RSS or available memory past warning threshold | WARN |
| `critical` | RSS or available memory past critical threshold | ERROR |

**Thresholds by device tier:**

| Tier | Total RAM | Warn RSS | Critical RSS | Warn Available | Critical Available | Growth/5min |
|------|-----------|----------|--------------|----------------|--------------------|-------------|
| Constrained | <256 MB | 15 MB | 20 MB | 15 MB | 8 MB | 1 MB |
| Normal | 256-512 MB | 120 MB | 180 MB | 32 MB | 16 MB | 3 MB |
| Good | >512 MB | 180 MB | 230 MB | 48 MB | 24 MB | 5 MB |

**Event fields:**

```json
{
  "schema_version": 2,
  "event": "memory_warning",
  "device_id": "<hashed>",
  "timestamp": "<ISO8601>",
  "level": "warning",
  "reason": "RSS 185MB exceeds warning threshold 180MB",
  "uptime_sec": 3600,
  "rss_kb": 189440,
  "vm_size_kb": 524288,
  "vm_hwm_kb": 195000,
  "vm_swap_kb": 0,
  "system_total_mb": 1024,
  "system_available_mb": 512,
  "growth_5min_kb": 2048,
  "private_dirty_kb": 150000,
  "private_clean_kb": 20000,
  "shared_clean_kb": 15000,
  "shared_dirty_kb": 1000,
  "pss_kb": 170000,
  "swap_pss_kb": 0
}
```

**Implementation:** `MemoryMonitor` (background thread, 5s sampling) → `WarningCallback` → `TelemetryManager::record_memory_warning()`. Growth tracking uses a circular buffer of 10 RSS samples at 30s intervals (5-minute window).
