# HelixPrint Plugin (Developer Guide)

How the HelixPrint Moonraker plugin and the app divide work: the feature framework every
plugin feature registers with, how the app decides between the plugin and its own fallback,
where each feature's setting lives, and one section per shipped feature.

**Plugin reference** (endpoints, full record shapes, install, configuration):
[`moonraker-plugin/README.md`](../../moonraker-plugin/README.md)
**User-facing doc**: [HelixPrint Plugin](../user/guide/helixprint-plugin.md)
**Power-loss recovery** is a plugin feature with its own guide:
[POWER_LOSS_RECOVERY.md](POWER_LOSS_RECOVERY.md#generic-helixprint-plugin---asked) for the
app, [Inside the plugin](POWER_LOSS_RECOVERY.md#inside-the-plugin-generic) for the plugin

---

## Overview

HelixPrint is a Moonraker component (`[helix_print]` in `moonraker.conf`) that ships in this
repo under `moonraker-plugin/`. It runs inside Moonraker's process on the printer's host, which
gives it two things the app does not have:

- **It sees every job**, whichever client started it: the screen, Mainsail, Fluidd, a slicer
  upload with `print=true`, or Moonraker's job queue.
- **It runs where the files are**, usually on hardware stronger than the screen, and it is up
  whenever Moonraker is, whether or not any HelixScreen is connected.

The app works without the plugin. Every feature has an app-side fallback, or is an
optimization the app does without, and the app picks between plugin and fallback per feature,
never per plugin version. A remote screen (the K-Touch, a tablet, a desktop build) benefits
most, because it is the one that is not always connected and cannot afford to read whole files.

### Ground rules

- **Opt-in for anything that acts on a print the screen did not start.** Each such feature has
  its own switch, off by default.
- **Every feature attempts an app-side fallback.** Where the fallback is weak or impossible, the
  feature's section says so under **Fallback**; a feature with none is plugin-only, and its row
  says so (`NeedsPlugin`).
- **What Moonraker already does stays with Moonraker**: storing app settings (the app already
  uses the `helix-screen` namespace), phone notifications (`[notifier]`), timelapse, zeroconf.
- **No feature moves the machine on its own.** The plugin sends motion, heat or print-control
  G-code only where a feature's section says so and the user opted in.
- **The app renders backend G-code; the plugin only sends it.** A record the plugin sends holds
  finished G-code made by the same functions the screen's own start uses, so no backend's G-code
  is written twice, once in C++ and once in Python. The one exception is power-loss recovery,
  whose resume script is built from a file scan that runs in the plugin, serves every client
  (the K-Touch cannot scan files) and duplicates no backend's G-code.
- **App-only work stays in the app.** Learned ETA correction reads history the app already
  has, so it is not a plugin feature.

## Key Files

| File | Role |
|------|------|
| `moonraker-plugin/helix_print.py` | The component: `print_modified`, status, the feature framework (`HelixPrint#register_feature`, `always_on`, `opt_in_enabled`, `feature_entry`, `notify_features_changed`), the helix-initiated job marker (`HelixJobMarkers`, `helix_resumed_job`), history row rewrites (`patch_history_job`, `set_history_aux`), the print copies' directories and their lock (`staging_dirs`, `link_lock`), the free-space guard hook (`check_free_space`), what a waiting power-loss resume needs (`recovery_protects`), sibling loading (`_load_sibling`) |
| `moonraker-plugin/helix_<feature>.py` | One module per feature, loaded by `_load_sibling`; see each feature's section |
| `moonraker-plugin/helix_file_queue.py` | What a feature keeping one record per gcodes file shares: `file_event` / `FileTracker` (filelist_changed, directory events fanned out per record, hidden staging paths), `WorkQueue` (one at a time, `front`, a `ready` gate, an item's exception logged), `RecordStore` / `namespace_items` (a namespace keyed by path, with the `ns_items` fallback), `scan_files`, `IDLE_JOB_STATES`; and the upload-hook helpers `upload_hook_shape_ok`, `upload_print_request`, `store_without_print`, `print_result`, `already_started`, `start_or_queue` and the hold limit `UPLOAD_HOLD_S`. Used by analysis, .3mf conversion and the fit check |
| `moonraker-plugin/helix_capabilities.py` | `print_extensions`: the extensions Moonraker prints, read from its own file manager |
| `moonraker-plugin/helix_gcode_reader.py` | Shared pure helpers beyond the G-code reader: `WorkerProcess`, `worker_argv`, `worker_nice`, `worker_timeout`, `atomic_write_bytes`, `atomic_publish`, `fsync_dir` |
| `moonraker-plugin/remote-install.sh` | Remote installer: a release's tarball from the release CDN, else that release's own copy from GitHub; `PLUGIN_FILES` names every file it ships. The CDN copy, https://releases.helixscreen.org/plugin/remote-install.sh, is overwritten by every release, so it keeps accepting `<ref> [--uninstall]` |
| `scripts/package-helix-plugin.sh` | Packs `PLUGIN_FILES` into the `helix-print-plugin.tar.gz` the release workflow uploads to `releases/v<version>/` |
| `moonraker-plugin/tests/test_helix_features.py` | Framework tests: status entries, a sibling that fails to import, opt-in defaults |
| `include/helix_plugin_features.h`, `src/printer/helix_plugin_features.cpp` | `parse_plugin_features`, `plugin_feature_state`, `plugin_feature_opted_in`, `plugin_switch_mode`, `fallback_sender_role`, the `local_screen` presence record, `kKnownPluginFeatures`, `PluginFeatureHome`, `plugin_feature_lines` |
| `include/fallback_sender.h`, `src/printer/fallback_sender.cpp` | `resolve_fallback_sender_role()`: reads the presence record and answers `fallback_sender_role` for every fallback that acts on the printer |
| `include/printer_plugin_status_state.h` | `PrinterPluginStatusState`: the parsed map, `plugin_feature(name, min_version)`, `kPluginSwitchFeatures`, the row subjects, `stored_opt_ins()` |
| `src/application/discovery_steps_core.cpp#helix_plugin_check_step` | Calls `server.helix.status` during discovery |
| `include/plugin_status_refresh.h`, `src/printer/plugin_status_refresh.cpp` | `PluginStatusRefresh`: reads `server.helix.status` again at each job start and on `notify_helix_features_changed`, announcing only a changed map (`FeatureAnnounce::IfChanged`, `include/helix_plugin_features.h`) |
| `include/history_job_events.h` | `HistoryJobEvents`, `parse_history_job_event`: the one history `added` / `finished` subscription the app's fallbacks and the refresh share |
| `ui_xml/plugin_feature_toggle_row.xml`, `src/ui/plugin/ui_plugin_feature_row.cpp` | The shared settings row for a plugin feature's switch |
| `src/ui/plugin/ui_plugin_overview_overlay.cpp` | Advanced > HelixPrint Plugin, the read-only overview |
| `src/plugin/plugin_permissions.cpp#is_readonly_moonraker_method` | The read-only method list, including the HelixPrint reads a Lua plugin may call |
| `src/api/moonraker_gcode_guards.cpp#reject_plugin_rpc` | Refuses every other `server.helix.*` method from a Lua plugin |
| `tests/unit/test_helix_plugin_features.cpp`, `tests/unit/test_plugin_feature_row.cpp` | `[plugin_feature]`: the decision functions and the row |

---

## The Feature Framework

### The status `features` map

`server.helix.status` carries `api` (the shape of the reply itself; 2 is the first with
`features`) and a `features` map, one entry per feature:

```json
"api": 2,
"features": {
  "file_analysis": {"version": 1, "available": true, "enabled": true, "reason": null},
  "print_hooks":   {"version": 1, "available": true, "enabled": false, "reason": null}
}
```

| Field | Meaning |
|-------|---------|
| `version` | The feature's own contract version, bumped when its endpoints change shape |
| `available` | Its module loaded and its requirements hold |
| `enabled` | Switched on. Always `false` while not available (`feature_entry`) |
| `reason` | Why it is unavailable, `null` otherwise: `plugin_disabled`, `module_missing`, `load_failed: ...`, `start_failed: ...`, or the module's own probe reason |

A plugin without `api` predates the map.

**`[helix_print] enabled: False` turns off every feature.** No module starts, so no handler,
endpoint or worker of one exists: a call to any feature's endpoint (`print_edited`, `config`,
`config_edit`, `journal`, `analysis`, `print_hooks/screen_start`, `filament_usage`, `storage`,
`storage_cleanup`) finds no such endpoint, and `print_modified` answers 503. Every entry reports
`available: false` with reason `plugin_disabled` (`feature_availability`), whatever its own
switch or probe says.

**Each feature starts in isolation.** `component_init` runs every module's start through
`_start_isolated(name, acts, start)`. A module that fails to import registers with its load
error; one whose start raises registers `available: false` with
`start_failed: <exception>`. Either way the component and every other feature keep running.

### Registering a feature

Each feature's `_start_<feature>()` in `helix_print.py` calls `register_feature`:

```python
self.register_feature(
    helix_journal.JOURNAL_FEATURE, helix_journal.JOURNAL_FEATURE_VERSION, acts=False,
    switch=always_on,
)
```

| Argument | Meaning |
|----------|---------|
| `acts` | `True` for a feature that acts on a print, a spool or the user's files. Its `plugin_features` default is off; an observer's is on |
| `switch` | An async callable that owns the feature's on/off. With one, the `plugin_features` key is never read |
| `probe` | Live availability, `(available, reason)`, read on each status call instead of the fixed `available` / `reason` |
| `extra` | Extra fields merged into the feature's status entry (PLR uses it) |

When the module is missing, the same name is registered with `available=False` and
`reason=SIBLING_LOAD_ERRORS.get(<module>, "module_missing")`, so the app can tell "not
installed" from "too old".

### Switches: `plugin_features`, own switches and `always_on`

A feature's on/off comes from exactly one place, never two:

- **The opt-in record.** `helix-screen` / `plugin_features` in Moonraker's database,
  `{"<feature>": true|false, ...}`. The app's settings rows write one field through a dotted
  key; the plugin reads the whole record (`_plugin_feature_opt_ins`) at the moment the feature
  acts, so a change applies without a restart or a notification. A missing key, or one that is
  not a bool, means the default (`opt_in_enabled`: observers on, actors off). One store means
  every screen on the printer agrees.
- **An own switch**, for a feature whose on/off depends on more than a boolean. PLR reads
  `helix-screen` / `plr_user.enabled` with a storage-dependent default; `convert_3mf` reads
  `[helix_print] convert_3mf` in `moonraker.conf`. The status entry reports that switch.
- **`always_on`**, the own switch of a feature with no user setting: one that only observes
  (file analysis, event journal, storage report) or a transport the user drives from the screen
  or that only replaces a poll (server edits, config edit, vendor push). No `plugin_features`
  key turns it off.

A feature with an on/off part and an always-on part registers twice, one entry per switch:
the storage report (`storage`, `always_on`) and its automatic cleanup (`storage_auto_cleanup`,
opt-in). A feature whose switch is its whole purpose is named after the switch
(`fit_check_blocks_autostart`), so `plugin_features`, `plugin_switch_mode` and the overview
need no special case.

`HelixPrint#feature_enabled(name)` is what a module asks before it acts: registered, available,
and switched on.

### The helix-initiated job marker

A plugin path that starts a job itself marks it, so start-of-job features can leave it alone
(a resumed job must not re-run start hooks partway through a part):

- `mark_helix_initiated(filename, kind, history_name=None)` immediately before the single
  command that starts the print (`SDCARD_PRINT_FILE`, or the `M24` of a resume), never before a
  script that homes, heats or purges first. `kind` is one of `HELIX_JOB_KINDS`: `plr_resume`,
  `print_modified`, `print_edited`. A start through a link passes the original name as
  `history_name`, since the finished history entry is renamed to it; both names answer.
- `unmark_helix_initiated(filename)` when that command fails.
- When history adds the job (`history:history_changed`, action `added`), the marker binds to
  the job id and persists in namespace `helix_jobs` (key = job id, value
  `{"kind", "filenames", "start_time", "bound_at"}`, kept 30 days). An unclaimed marker expires
  after 60 s.
- A `plr_resume` marker also records the history job id of the job it continues (PLR's
  interrupted job, binding field `resumes`); `helix_resumed_job(job_id)` answers it, so a
  module that carries a job's state across a resume (filament accounting) matches by id, never
  by file name.
- `helix_initiated(job_id, filename, start_time) -> Optional[str]` answers for every module.
  History reuses job ids (the SQL table has no AUTOINCREMENT, and the older LMDB history resets
  its counter on delete-all), and no Moonraker announces a deleted job, so a binding answers
  only for its own `start_time` and names, and an `added` event with no marker drops any binding
  its id had.

**Start-of-job features act from history `added` (or later), never from the `job_state`
start.** `job_state` events carry no job id, so the binding cannot exist yet there and
`helix_initiated()` can still answer None for a job the plugin started. `helix_print` registers
its history handler before any feature module and binds before its first `await`, so a
module's own handler of the same `added` event sees the binding. Print hooks, filament
accounting and photos all key on it.

A feature that needs a marker of its own uses a private `HelixJobMarkers` instance with its own
kind (print hooks' `screen_start`), which `helix_initiated()` never answers.

### History rows

A feature that records something per job keeps it in its own namespace keyed by the job id
(the compatibility floor below), and also writes it into the history row's `auxiliary_data`
where Moonraker has `get_job` / `save_job` (0.9+). Every rewrite of a history row goes through
`HelixPrint#patch_history_job(job_id, update)`, which serializes them under one lock, so two
features finishing the same job never save over each other. `HelixPrint#set_history_aux(existing,
name, value, description)` is the one rule for an entry: it replaces only this plugin's entry of
that `name` (provider `helix_print`) and keeps every other entry, including another provider's.
`HelixPrint#drop_history_aux(existing, name)` is the same filter without the new entry, which
photos uses to unlink the photos a storage cleanup deletes. Users today: `modifications` (the linked-print rename), `photos`, `filament_spools`. A write
happens only when the row's `start_time` is the job's, so a reused id is left alone.

### The app side: `plugin_feature()` and one decision function

`PrinterPluginStatusState::plugin_feature(name, min_version)` returns a `PluginFeatureState`:

| State | When |
|-------|------|
| `Ready` | Plugin installed and enabled, entry present, `available`, `version >= min_version`, switched on |
| `Disabled` | The same, but switched off |
| `Absent` | Everything else: no plugin, a plugin without the map, no entry, a contract below `min_version`, `available: false` |

Callers gate on a feature and the minimum contract they need, never on the plugin's version
string. A mismatched plugin keeps serving whatever it reports, so an older plugin degrades
feature by feature; the version string still drives `PluginSelfHeal`.

**When the map is read.** Discovery reads it once per connection. A feature whose availability
can change while Moonraker runs (photos losing its camera) would otherwise leave every screen
deciding from a stale answer, so the map is also read again by `PluginStatusRefresh`: at every
history `added`, and whenever the plugin sends `notify_helix_features_changed`
(`HelixPrint#notify_features_changed`, event `helix_print:helix_features_changed`, sent by a
module when its availability flips). Every feature's decision benefits; nothing in the refresh
is feature-specific.

A map arriving bumps `helix_plugin_probe_seq`, which its observers read as "a probe answered"
and answer by re-reading opt-ins, rewriting their records and announcing presence. So
`set_features` takes a `FeatureAnnounce`: discovery and the settings rows' re-reads pass
`Always` (a new connection's observers must re-subscribe, and a switch just written must show),
while `PluginStatusRefresh` passes `IfChanged`, so a job start that finds the same map touches
no subject and re-runs no observer.

**The rule:** each consumer chooses between plugin path and fallback in exactly one place, a
pure function of `plugin_feature(...)` and its own inputs (`file_analysis_source`,
`choose_edit_transport`, `decide_journal_source`, `config_edit_route`,
`standing_options_sender`, `filament_accounting_sender`, `photo_taker`, `report_source`,
`vendor_state_source`). That function is tested over all states, and no caller re-derives
the choice.

### Compatibility floor

The plugin runs on every printer the app supports, measured on the fleet (2026-10-20,
`machine.system_info` and `server.info`):

| Device | Python | Moonraker |
|---|---|---|
| K1C (Buildroot 2020.02) | 3.8.2 | v0.9.3 |
| K2 Plus (OpenWrt 21.02) | 3.9.12 | API 1.4.0, no version string |
| QIDI Q2 (Debian 11) | 3.9.2 | API 1.4.0 |
| Voron V0 (Debian 11) | 3.9.2 | v0.11.0 |
| SonicPad (Debian 11) | 3.9.2 | v0.10.0 |
| AD5M (Forge-X 1.4.2) | 3.11.10 | API 1.4.0 |
| U1 (Buildroot 2024.02) | 3.11.8 | "1.6.0" (vendor string) |
| CC1 (OpenCentauri) | 3.12.12 | API 1.5.0 |
| AD5X (Buildroot 2025.02) | 3.12.10 | API 1.4.0 |
| CB1 Voron (Armbian trixie) | 3.13.5 | v0.10.0 |

- **Python 3.8 to 3.13.** Keep `from __future__ import annotations` and `typing` generics. No
  `match`, `str.removeprefix`, dict `|`, `zoneinfo` or other 3.9+ stdlib API; no asyncio APIs
  deprecated by 3.12/3.13 (implicit `get_event_loop`). CI runs `pytest moonraker-plugin/tests/`
  on 3.8 and 3.11 (`.github/workflows/build.yml`).
- **API 1.4.0 forks.** Half the fleet reports Moonraker API 1.4.0 with no `moonraker_version`.
  Gate on `api_version` and on probing the call, never on a version string.
- **History records live in the plugin's own namespace.** API 1.4.0 predates history's
  `auxiliary_data` / `save_job`, so any feature that records into history keeps its record in
  its own namespace keyed by job id, and treats `auxiliary_data` as a bonus where present
  ([History rows](#history-rows)).

---

## Settings Placement

No switch lives on a plugin page. Each switch sits on the settings page for what it affects,
as a `plugin_feature_toggle_row`, and the plugin gets one read-only overview.

### The switch row

`plugin_switch_mode(plugin_available, features, name, min_version, has_fallback, is_local)`
decides how the row shows, and the XML renders that mode (one place to change):

| Plugin state | Mode | The row |
|--------------|------|---------|
| Ready or Disabled | `Switch` | a normal switch |
| Plugin fine, entry present, `available: false`, plugin-only feature | `Hidden` | not shown: Install cannot help an unsupported printer. The reason shows on the overview only |
| No plugin, no map, no entry, or below `min_version` (or the entry unavailable); feature has a fallback; screen on the printer's host | `Switch` | a working switch, no hint |
| The same on a remote screen | `FallbackInstallHint` / `FallbackUpdateHint` | a working switch, "Only while this app is connected. Install (or Update) the HelixPrint plugin to cover every print.", with Install for a missing plugin |
| No plugin, no map, no entry, or below `min_version`; plugin-only feature | `NeedsPlugin` / `NeedsUpdate` | greyed, "Requires the HelixPrint plugin" with Install, or "Requires a newer HelixPrint plugin" |

Before the plugin probe answers, the row is hidden. An outdated plugin gets no button: the
install flow refuses a plugin that is already installed, and version mismatch is
`PluginSelfHeal`'s job. Locality is `PrinterState::moonraker_locality()`; Unknown counts as
remote. With no plugin entry, a fallback switch shows the stored opt-in
(`PrinterPluginStatusState::stored_opt_ins()`, read on each probe) and still writes it, since
`plugin_features` lives in Moonraker's database and needs no plugin.

A feature that owns its switch (PLR) passes its own subject and callback and gets the same
modes.

**Which features get a switch.** A feature that acts on a print, a spool or the user's files,
and an observer with an app fallback the user may want off (photos). Pure observers (file
analysis, event journal, storage report) and transports (server edits, config edit, vendor
push) get no switch anywhere. Homes, each a `PluginFeatureHome`:

| Feature | Page | `PluginFeatureHome` | Fallback |
|---------|------|---------------------|----------|
| `print_hooks` | Settings > Printing | `Printing` | yes |
| `plr` | Settings > Printing | `Printing` | no |
| `fit_check_blocks_autostart` | Settings > Safety & Alerts | `Safety` | no |
| `filament_accounting` | Settings > Devices > Spoolman, Weight Sync group | `Spoolman` | yes |
| `photos` | Settings > Devices, under Camera | `Devices` | yes |
| `storage_auto_cleanup` | Settings > System > Storage | `Storage` | no |

The event journal has a view but no switch: Printer Events, opened from the notification
history header.

### The overview

Advanced > HelixPrint Plugin (`PluginOverviewOverlay`) shows the plugin version and one line per
status-map feature, built by `plugin_feature_lines()`: Active, Off, or "Unavailable: <reason>".
A feature whose `kKnownPluginFeatures` row names a `PluginFeatureHome` leads to that page; a
feature with no setting, or one this app does not know (shown by its key), shows status only.
A feature that serves one kind of hardware lists, in its `kKnownPluginFeatures` row's
`hidden_reasons`, the reasons that mean that hardware is absent. While it is unavailable for one
of them it gets no line (`hidden_on_overview`), so a printer lists only the vendor transport it
could use. Any other reason (a load failure, `plugin_disabled`) still shows as Unavailable with
its reason, like every other feature. Only `vendor_ad5x` and `vendor_ace` carry such a list. The Advanced panel shows Install while the plugin
is known absent, and the overview and Uninstall while it is installed.

The reason is shown in words, not as the plugin's code: `plugin_reason_text()`
(`include/helix_plugin_features.h`) looks the code up in `kPluginReasonTexts`, reading it up to
any `": detail"` suffix (`snapshot_unreachable: <url>` maps on `snapshot_unreachable`), and
shows any reason not in the table as the plugin wrote it. A module that adds a reason code adds
its row there.

| Code | Reported by | Text |
|------|-------------|------|
| `plugin_disabled` | every feature, `enabled: False` in `[helix_print]` | The plugin is turned off in moonraker.conf. |
| `module_missing` | a feature whose module file is absent | Part of the plugin is missing. Reinstall the plugin. |
| `load_failed` | a feature whose module raised on import | Part of the plugin failed to load. Reinstall the plugin. |
| `start_failed` | a feature whose start raised | This feature failed to start. The Moonraker log says why. |
| `stopped` | `convert_3mf`, its converter stopped | This feature stopped. Restart Moonraker to start it again. |
| `needs_file_analysis` | `fit_check_blocks_autostart` | This needs the plugin's file analysis, which isn't running. |
| `upload_hook_unsupported` | `fit_check_blocks_autostart` | This Moonraker version can't hold uploads. Update Moonraker. |
| `no_gcodes_dir` | `file_analysis` | Moonraker has no G-code folder. |
| `python_too_old` | `plr` | Moonraker's Python is too old for this feature. |
| `no_virtual_sdcard` | `plr` | Klipper has no [virtual_sdcard] section. |
| `store_unwritable` | `plr` | The plugin can't save its recovery data to disk. |
| `not_listed` | `plr` | Power-loss recovery isn't supported on this printer yet. |
| `snapshot_unreachable` | `photos` | The plugin can't reach the camera, so this screen takes the photos. |
| `no_adventurer_json` | `vendor_ad5x` (hidden) | This printer has no IFS. |
| `adventurer_json_unreadable` | `vendor_ad5x` | The IFS state file can't be read. |
| `no_ace_bridge` | `vendor_ace` (hidden) | No ACE unit was found. |
| `ace_objects_carry_slots` | `vendor_ace` (hidden) | Klipper already reports the ACE slots. |
| `bridge_unreadable` | `vendor_ace` | The ACE state can't be read. |

`storage` and `storage_auto_cleanup` report a sentence (`root_problem` in `helix_storage.py`),
not a code, so it shows as written.

### One fallback sender per printer

An attached screen and a tablet must never both act for a fallback on the same printer.
`fallback_sender_role(is_local, local_screen_present)`: the instance on the printer's host
acts; a remote instance acts only when no local HelixScreen is connected, and stands by when
that cannot be told.

Moonraker exposes no list of connected clients (`server.connection.identify` only registers
one; `server.extensions.list` lists agents only), so a local HelixScreen announces itself in
`helix-screen` / `local_screen` = `{"seen_at": <unix seconds>}` on each probe and every
`kLocalScreenPresenceIntervalS` (10 minutes) while connected. A remote instance reads it when it
would act: younger than two intervals (20 minutes) is present, a missing key is absent,
anything else is unknown. A crashed local screen keeps the remote standing by for up to 20
minutes, which fails toward not acting. Every fallback that acts on the printer, or writes a
record the plugin reads, asks it through `resolve_fallback_sender_role()`
(`include/fallback_sender.h`): print hooks' send and record, filament accounting's spool switch
and `filament_spools` record, and photos' capture.

---

## The Lua-Plugin Allowlist

Lua plugins reach Moonraker through `helix.moonraker.call`, which runs every request through
`reject_plugin_rpc`. For `server.helix.*`:

- **Reads** pass without `moonraker_write`: `server.helix.status`, `plr.status`,
  `3mf_conversions`, `analysis`, `journal`, `config`, `filament_usage`, `storage`. They are in
  the one read-only list, `is_readonly_moonraker_method`, which the permission check and the
  guard both read.
- **`print_modified` and `print_edited`** (with `moonraker_write`) start a print the way
  `printer.print.start` does and carry its checks: both paths validated, refused while spools
  lie on the bed.
- **Everything else is refused**, naming the method ("`<method>` is not available to
  plugins"): the PLR wizard calls, `config_edit`, `print_hooks.screen_start`,
  `storage_cleanup`, and any method added later. They heat, move, delete or drive the resume
  wizard, which only a confirmed tap on the screen may do.

A new read-only endpoint joins `kReadonlyMethods` in the same change, under its exact JSON-RPC
name. A mismatch fails closed. [PLUGIN_DEVELOPMENT.md](PLUGIN_DEVELOPMENT.md) has the Lua side.

---

## File Analysis (`file_analysis` v1)

The plugin reads every G-code file under `gcodes` once, in the background after an upload, a
change or startup, and keeps one record per file. The app uses the record as its first source
for per-tool facts and for the fit check, and builds its streaming preview index from the layer
index the analysis writes. The K-Touch, which has no viewer and no whole-file parse, gets
thumbnails for thumbless files and per-tool amounts only this way: the one plugin-required case.
Observer: `always_on`, no switch.

| File | Role |
|------|------|
| `moonraker-plugin/helix_analysis.py` | Queue, backfill, endpoint, records (namespace `helix_analysis`), thumbnail publishing, `record_for(path, timeout)` (the current record, or analyze ahead of the queue and wait); run as a script, the worker that analyzes one file |
| `moonraker-plugin/helix_gcode_reader.py` | The one G-code reader shared with PLR: `scan_to_offset` with a `ScanObserver`, the layer index (`write_layer_index`, `LayerIndexWriter`, `read_layer_index`), `file_identity`, `prune_layer_indexes`, `WorkerProcess` |
| `include/file_analysis.h`, `src/printer/file_analysis.cpp` | `file_analysis_source`, `parse_file_analysis_reply` (the record's `bounds` and `header` into `FileAnalysisRecord::fit`), `apply_file_analysis`, `request_file_analysis`, `with_layer_index_sidecar`; exception-free, compiled for the K-Touch |
| `src/printer/print_start_inputs.cpp#load` | `FileStartFactsLoader::load`: metadata, record, footer, scan, in that order |
| `src/rendering/gcode_layer_index.cpp` | `GCodeLayerIndex::build_from_sidecar`, `layer_index_file_hash` |
| `src/ui/print/ui_print_select_detail_view.cpp`, `src/ui/print/print_preview_controller.cpp` | The two previews: fetch the sidecar, then load |

**Endpoint.** `server.helix.analysis {"filename"}` answers `ready` with the record, `pending`
(queued at the front; `notify_analysis_ready {"filename"}` follows), `failed` (this version
failed twice; a change retries, a restart does not: namespace `helix_analysis_failures` keeps
`[size, mtime, attempts]` per path) or `disabled`. 400 for a path that is not a visible
`.gcode/.g/.gco`, 404 for a missing file. Record shape (schema 1: tools with per-field
`sources`, bounds, header, objects, layers, pauses, thumbnail): README § File Analysis and the
`helix_analysis.py` docstring. `thumbnail` is set only when the plugin published one (the
largest embedded PNG, else a generated top-down render) because Moonraker's metadata had none;
listing it in Moonraker's metadata is best effort across versions. `objects` keeps label-only
objects (`; printing object`, `M486`) that Klipper cannot exclude.

**Layer index sidecar.** `.helix_index/<path>.layers.jsonl`, shared with power-loss recovery:
a header with the file's identity (size, mtime, sha1 of the first and last 64 KB), one line per
layer start with offset, Z and the full modal state, then `progress_lines` (pause, M73 and
SET_PRINT_STATS_INFO lines with their offsets, replayed by the screen's own pause scan),
`summary` (model box without purge and tower, palette, tools) and `complete`. Analysis adds per
layer `tools`, `print_z` and `feature`, so the screen builds its streaming index from the
sidecar alone. A layer starts at its slicer marker line, since the state before the marker is
what a resume scan needs. One writer per file: analysis writes `<index>.tmp` and publishes it
whole (`atomic_publish`); the PLR follower appends during a print and never touches a complete
one. While a print runs, the follower may build the printing file's index on any host, reading
no further than `virtual_sdcard.file_position` at a capped rate. One prune rule:
`prune_layer_indexes` removes an index whose source is gone or changed, `.external` indexes and
orphaned `.tmp` files, except the paths PLR protects (the one it follows and the one its waiting
snapshot resumes from) and the index of the file the worker is reading. A `.tmp` is orphaned
(`temp_index_orphaned`) once its source is gone or it is older than `worker_timeout()` for its
source's size, so PLR's job-start prune, which knows nothing of analysis, never removes one a
worker is still writing. Analysis starts, and so prunes, only after PLR has loaded its job.

**Worker.** One file at a time in a child process (Moonraker's interpreter, nice 19,
`ionice -c 3`, killed on shutdown): a pure-Python pass in Moonraker's own process would hold the
GIL its event loop needs. The timeout, `worker_timeout()` in `helix_gcode_reader.py`, is 600 s +
60 s/MB, sized for a nice-19 worker on a MIPS or Cortex-A7 host; `print_edited`'s worker uses the
same rule. While printing it runs only on a multi-core host; a print starting on a one-core host
kills the worker and requeues the file. A file deleted while its worker ran is forgotten, sidecar
included; one changed meanwhile loses the sidecar just written and is queued again. An embedded
thumbnail block is dropped once it passes its cap or meets a line that is not base64, and the
comments after it are read as usual; an extruding move far off the footprint grid is clipped to
it before stepping.

**Decisions.** `file_analysis_source(plugin_feature("file_analysis", 1), filename)`: Plugin
only when Ready and the file is not a `.3mf`. `apply_file_analysis()`: the record wins per field
and per tool slot; grams only when every printing tool has them. `build_from_sidecar()`: only a
complete sidecar whose size and hash match the local copy, ending at its first `complete`
record.

**Fallback.** Without the plugin, with the feature unavailable, or for a pending or failed
record, the loader runs metadata, footer read and `Tn` scan, and a preview indexes the file
itself. The K-Touch then has no thumbnail for a thumbless file and no per-tool grams beyond the
footer.

**Testing.** `pytest moonraker-plugin/tests/test_helix_analysis.py
moonraker-plugin/tests/test_helix_gcode_reader.py` (needs pytest-asyncio; slicer fixtures in
`moonraker-plugin/tests/fixtures/analysis/`). `make t F='[plugin_file_analysis]'` covers the
sidecar-vs-file index equality and the loader against the mock
(`MoonrakerClientMock::set_mock_file_analysis`); `[layer_index]` covers the sidecar reader.

---

## File-Fits-Printer Check (`fit_check_blocks_autostart` v1)

Compares a file with the printer's own limits and gives findings `{kind, severity, detail}`:
`out_of_bounds` and `temp_over_max` (block: stop a start), `too_many_tools` (block, or warn
when only `T` macros raised the tool count), `nozzle_mismatch` (warn: the screen asks before a
start), `nozzle_flow_mismatch` (warn, either way round: file tool i is compared with extruder i,
and a printer's own filament mapping can put it on another head) and `printer_model_mismatch` (note: shown in the file details only). Nozzles come
from `configfile.settings`, replaced per extruder by the `nozzle_diameter` and
`nozzle_volume_type` an extruder reports in its status where the firmware keeps the fitted
nozzle outside the config (the Snapmaker U1); the file's flow types come from Orca's
`nozzle_volume_type` config comment, else Moonraker's `filament_volume_type` metadata.
Nozzles are compared only for the tools the file uses (all extruders when it names none),
file tool i against extruder i. The screen runs it
as a print-start gate and shows it in the file details; the plugin, when opted in, holds a
slicer's upload-and-print that would be blocked. Acts on a print: opt-in through
`plugin_features.fit_check_blocks_autostart`, default off, plugin-only. Switch: Settings >
Safety & Alerts > Hold slicer prints that do not fit.

| File | Role |
|------|------|
| `moonraker-plugin/helix_fitcheck.py` | Pure `machine_limits(settings)`, `machine_with_live_nozzles(limits, live)`, `nozzle_volume_type(text)`, `check_fit(machine, facts)`, `fit_file_from_record(record)`, `may_autostart(enabled, findings)`, `holds_upload(form_args, print_exts)`; `FitCheck` (upload hook, converter gate, `hold_wanted`, notification; queries each configured extruder's `nozzle_diameter` / `nozzle_volume_type`) |
| `moonraker-plugin/helix_file_queue.py` | `UPLOAD_HOLD_S`, `already_started`, `start_or_queue`, `store_without_print`, shared with the .3mf converter |
| `moonraker-plugin/helix_3mf.py` | `Helix3mfConverter.start_gate` / `hold_wanted`: a converted upload-and-print asks the gate, under the same deadline |
| `include/fit_check.h`, `src/printer/fit_check.cpp` | The C++ twin: `FitSeverity` (Note, Warn, Block), `fit_machine_from_settings`, `fit_machine_with_live_nozzles`, `fit_nozzle_volume_type`, `check_fit`, `fit_blocks`, `fit_warns`, `fit_worst`, `fit_machine_for_screen`, `fit_head_facts`, `fit_file_from_sources`, `fit_findings_text`; exception-free, compiled for the K-Touch |
| `include/gcode_wait_line.h#heater_target_line` | The heater target lines the head scan reads (M104/M109/M140/M190/SET_HEATER_TEMPERATURE), beside the wait-line classifier |
| `tests/fixtures/fit_check/cases.json` | The one table both checkers are held to (limit cases and check cases) |
| `src/printer/print_start_checks.cpp#gate_fit_check` | The gate, right after `printer_stopping_command` in `default_print_start_gates` |
| `src/printer/print_start_inputs.cpp#current_fit_machine` | Limits from `PrinterDiscovery::fit_machine()` (parsed from `configfile.settings` at discovery), plus the database model, the filament-system rule and the nozzles the extruders report (`ExtruderInfo::nozzle_diameter` / `nozzle_volume_type`, subscribed in `src/api/moonraker_discovery_sequence.cpp`) |
| `src/ui/print/ui_print_select_detail_view.cpp#refresh_fit_check` | The detail card, above the filament card: `detail_fit_check_severity` (0 no finding, then one past the worst: 1 note, 2 warn, 3 block) and `detail_fit_check_text` (every finding) |

**Limits.** A delta reads `print_radius` (else `delta_radius`), Z from `minimum_z_position`
(0) to the lowest tower `position_endstop`; any other kinematics reads `stepper_[xyz]`
`position_min` (default 0) / `position_max`, widened by `dual_carriage`. Max temps: the hottest
`extruder*` `max_temp`, `heater_bed` `max_temp`. Tools: extruders and `gcode_macro t<n>`;
`tool_count_confirmed` holds when the extruders alone give the count. Nozzles:
`nozzle_diameter` by extruder number.

**Rules.** The file side checks travel bounds when the record has both ends, else print
bounds; exactly at a limit passes (epsilon 1e-4), one step over fails; a delta fails only when
a box edge (not a corner) is past the radius. A tool past a confirmed count blocks; past a
count only `T` macros raised, it warns (Start Anyway). Nozzles compare per extruder over the
shorter list (0.005 mm). Models compare by ASCII letters and digits, lowercased, and mismatch
only when neither contains the other. The file's model is `printer_model`, else Cura's
`TARGET_MACHINE.NAME`, else `MACHINE_NAME`, in that order on both sides
(`helix_analysis.header_from_config`, `fit_head_facts`); never a preset name such as
`printer_settings_id`, which is the user's label for a profile. No model, no finding. The
printer's model is the profile's `printer_type`, used only when the printer database lists it
with a manufacturer (`src/printer/print_start_inputs.cpp#known_printer_model`); the plugin has
none, so it never reports `printer_model_mismatch`. The gate asks only for a warn
(`fit_warns`) and stops only for a block (`fit_blocks`); a note shows on the detail card only.

**File facts on the screen.** `fit_file_from_sources(record, head, metadata, preview, tools)`:
the plugin's record (`FileAnalysisRecord::fit`, parsed from `bounds` / `header`) field by field,
else the 200 KB head scan (`ScanResult::fit_head`), metadata `first_layer_*_temp` and
`nozzle_diameter`, and `helix::ui::gcode_viewer_get_model_bounds()` (the full parse box or
`LayerIndexStats`). A queued start (`fill_file_context`) has the record and metadata only. The
detail view asks for the record once per open (`request_fit_record`, from `on_activate`); a
`pending` reply is ignored and the fallback facts stand until the view is opened again, since
nothing listens for `notify_analysis_ready` there.

**Tool count.** The screen nulls the tool count whenever any filament backend exists (the
unresolved-tools gate covers lanes); the plugin always nulls it, since it cannot see a filament
system's `T` commands. So `too_many_tools` never holds a slicer start and fires on the screen
only on a printer with no filament system.

**Plugin hold.** Which uploads: one into `gcodes` whose form asks for the print (`print=true`)
of a file Moonraker prints (`holds_upload`, with the extensions read from Moonraker's own
`VALID_GCODE_EXTS` by `helix_capabilities.print_extensions`: `.gcode`, `.g`, `.gco`, `.ufp`,
`.nc`), and a `.gcode.3mf` the converter starts. A start the plugin never sees as such a
request is not held: a slicer agent that uploads and then sends its own start command, a start
from another client after the upload, the job queue's next job. The plugin cannot tell those
apart from any other start.

- **Opt-in at entry.** `FitCheck.hold_wanted()` reads the switch when the hook takes the
  upload; off (or unreadable) means Moonraker's own upload, unchanged, for G-code and .3mf
  alike. On, the hook stores the file without the flag (`store_without_print`).
- **One deadline.** `UPLOAD_HOLD_S` (45 s) from hook entry covers storing, conversion and the
  wait for the analysis (`record_for(path, remaining)`): nginx in front of Moonraker answers 504
  after 60 s, and a hold that outlives the slicer's request would start a print the slicer
  reported as failed. A .3mf hold that reaches the deadline answers `print_started: false` and
  marks the request abandoned, so the conversion finishes but nothing starts it; a re-send that
  joins it gets its own chance.
- **No wait while busy.** With `print_stats` printing or paused (unknown counts as busy), or the
  analysis queue held, the check reads only a record already there (`record_for(path, 0)`): a
  blocked file is neither started nor queued, a file with no record is started or queued
  unchecked at once, as Moonraker would.
- **One start.** `start_or_queue` does nothing when `already_started` (`print_stats` printing or
  paused on that file, or the job queue holds it; a finished file's leftover `print_stats`
  filename does not count) or when a start of the same path is already in flight in this
  process (`_starts_in_flight`, claimed with no await between check and claim).
- **Refusal.** A block finding sends `notify_fit_check_refused {filename, findings}` and the
  upload response says `print_started: false`; the file stays uploaded. A warn or a note never
  holds.
- **Fails open.** No file analysis, no record before the deadline (still queued, failed, or a
  path analysis cannot read such as `.nc`), an unreadable `configfile`, or a check that raises:
  the print starts unchecked, and the plugin logs why at info. The only way an opted-in hold
  ends without a start and without a block is a .3mf whose conversion plus check passes the
  deadline.

Unavailable (`needs_file_analysis`, `upload_hook_unsupported`) without file analysis or a
hookable `finalize_upload`.

**Decision.** The screen always runs the checker; the plugin's record is only its best source
(`file_analysis_source`). The row is `plugin_switch_mode` with `has_fallback=false`: the screen
never sees a slicer's start, so nothing can stand in for the hold.

**Fallback.** For the screen's own starts, the checker over the facts above. Travel is checked
only once a preview has loaded: there is no streaming pass at print start, so a thumbnail-only
screen, a preview still loading, a queued start and the K-Touch (its viewer stub reports no
bounds) check temperatures, nozzle, model and tools but not travel. Preview bounds are the model
box (purge and wipe tower excluded), so this is less strict than the plugin's travel box.

**Testing.** `pytest moonraker-plugin/tests/test_helix_fitcheck.py` (the hook, busy printer,
one start, deadline) plus `test_helix_analysis.py::TestRecordFor` and
`test_helix_3mf.py::TestStartGate` / `TestHeldUploadDeadline`;
`make t F='[plugin_fitcheck],[wait_line]'`. A rule change edits
`tests/fixtures/fit_check/cases.json`, which both suites read.

---

## Print-Start Hooks (`print_hooks` v1)

A print started outside HelixScreen skips the screen's pre-print step. The part of that step
that still applies once a print runs is kept as finished G-code in Moonraker's database; the
plugin sends it at each foreign start, and the app sends it itself when the plugin cannot.
Acts on a print: opt-in through `plugin_features.print_hooks`, default off. Switch:
Settings > Printing > Apply start options to prints from other apps.

| File | Role |
|------|------|
| `moonraker-plugin/helix_print_hooks.py` | `standing_gcode()`, `hook_gcode()` (pure), `PrintHooks` (history handler, `screen_start` endpoint) |
| `include/ams_print_routing.h#standing_print_gcode` | `IPrintRouting::standing_print_gcode()`, the capability: per-print choices that still apply to a running print. Default empty |
| `src/printer/ams_backend_snapmaker.cpp#standing_print_gcode` | U1: the end-unload list, with `FORCE=1` because the print is already running |
| `include/standing_print_options.h` | `render_standing_gcode`, `standing_options_record`, `standing_options_sender`, `announce_screen_start`, `ScreenStartLedger` |
| `src/printer/standing_print_options_controller.cpp` | Keeps the record current, registers `notify_history_changed`, the app fallback send, the presence heartbeat |
| `include/moonraker_db_item.h` | `post_item_if_changed()` |

**What the record holds.** Only choices that still apply to a print already running, since
history adds the job once the print has started. Not in it: file rewrites and skip parameters
(too late once another client started the file), AMS routing (`SET_PRINT_EXTRUDER_MAP` /
`SET_PRINT_USED_EXTRUDERS` are refused mid-print), and timelapse (the screen's default is
moonraker-timelapse's own global `enabled`, which every start already honors). Moonraker's own
queue transitions are foreign starts like any other.

**Records and endpoints.** `helix-screen` / `standing_print_options` =
`{"version": 1, "gcode": "<newline-joined>"}`, rendered by the backends' own function and
written only when it differs from the stored value. The record is rendered from one screen's
settings, so only the screen whose `fallback_sender_role` is Sender writes it, and never an
empty one: before the filament backend reports, nothing is known, and no record beats a wrong
one (`src/printer/standing_print_options.cpp#writes_standing_record`). `POST /server/helix/print_hooks/screen_start
{"filename"}`: the screen announces its own direct start just before `printer.print.start`,
only while `print_hooks` is Ready; a private marker (`SCREEN_START_TTL_S`, 600 s: the start command can wait that long
behind Klipper's queue, as long as the app waits for it) that
`helix_initiated()` never answers. `{"filename", "withdraw": true}` takes it back after a
start that failed, so a start of the same file elsewhere still gets its hooks. The plugin acts
from history `added`, and sends only if `job_still_running()` holds when it is about to:
`print_stats` reports that file `printing`. Every await before the send can outlive the job.

**Decisions.**
- Plugin `hook_gcode(kind, screen_started, enabled, record)`: nothing for any helix-initiated
  kind, a screen start, opt-in off, or an absent, empty or other-version record.
- Plugin `job_still_running(status, filename)`: the last check before the send; an
  unreadable `print_stats` reads as not running.
- App `standing_options_sender(state, opted_in, screen_started, has_gcode, role)`: a screen
  start or nothing rendered sends nothing; Ready means the plugin sends; Disabled sends nothing;
  Absent sends from the app when opted in and `fallback_sender_role` says Sender.

**Foreign-start detection (fallback).** `MoonrakerJobAPI::start_print` notes the filename in
`screen_start_ledger()`; `start_modified_print` and the PLR commit note "any job" (the commit
with its 30-minute window). A history `added` the ledger cannot claim is foreign.

**Fallback coverage.** A screen on the printer's host covers every start except one during
boot or an app restart. A remote screen covers starts only while connected, which is what its
row's hint says.

**Testing.** `pytest moonraker-plugin/tests/test_helix_print_hooks.py`;
`make t F='[plugin_print_hooks],[plugin_feature],[plr][settings]'`. Mock:
`HELIX_MOCK_HELIX_PLUGIN=1` reports `print_hooks`, enabled from the mock database.

---

## Server-Side G-code Edits (`server_edits` v1)

A print the user starts from the screen sometimes prints a rewritten copy: a pre-print toggle
comments out a step the file runs itself, a skip parameter is appended to `PRINT_START`, or
Filament Mapping renumbers tools on firmware with no mapping table. With `server_edits` the app
sends the plugin the list of edits instead of the copy; the plugin rewrites beside the original
and starts the result through the same linked-print path as `print_modified`. Transport:
`always_on`, no switch.

| File | Role |
|------|------|
| `moonraker-plugin/helix_edits.py` | `parse_edits` / `resolve_spans`, `apply_plan` / `apply_file` (pure), `HelixEdits` (endpoint); run as a script, the worker that rewrites one file |
| `moonraker-plugin/helix_print.py` | `_start_edits`; `resolve_original`, `resolve_temp`, `start_linked_print(..., kind)` shared with `print_modified` |
| `include/gcode_edit_list.h`, `src/rendering/gcode_edit_list.cpp` | `EditList`, `to_server_edits`, `from_server_edits`, `choose_edit_transport` |
| `src/ui/print/ui_print_preparation_manager.cpp` | `modify_and_print`, `modify_and_print_with_remap` fill the server-edits route; `rewrite_and_print` picks it; `start_staged_print` starts either |
| `src/api/moonraker_job_api.cpp#start_edited_print` | JSON-RPC `server.helix.print_edited` |
| `src/rendering/gcode_file_modifier.cpp`, `src/rendering/gcode_tool_remapper.cpp` | The app's half of the grammar |
| `tests/fixtures/gcode_edits/` | Golden cases both suites apply |

**Endpoint.** `server.helix.print_edited {original_filename, temp_file_path?, edits[],
modifications[]}`. The reply is `print_modified`'s plus `lines_changed`. Errors: 400 for a
malformed or out-of-bounds list (lines 1..2147483647, `end_line` at least `line_number`, at
most `MAX_EDITS` = 10000 edits), a path outside gcodes, or a copy path that is not directly in
`temp_dir` / `.helix_temp` / `helix_staging` or is the original (`resolve_temp`, shared with
`print_modified`); 409 for a line that differs from its `expect_raw_line` or a copy path that
already exists; 507 when the copy would leave less than the storage floor free
([Storage](#storage-report-and-guard-storage-v1-storage_auto_cleanup-v1)); 500 when the worker
crashes or times out, or the write or start fails. With the plugin disabled the endpoint does
not exist. A range stays one span (`resolve_spans`), so a plan's size follows the edit count,
never a range's length. The rewrite runs in a worker process (`helix_edits.py` as a script,
nice 19), never in Moonraker's: its stdout is `{"changed"}` or `{"refused": line}`. It writes a
uniquely named `<name>.<random>.part` beside the copy, which is published with
`atomic_publish(exclusive=True)` and the original's mode: two requests for one path give one
whole copy and a 409. The job is marked `print_edited` immediately before `SDCARD_PRINT_FILE`.
Edit grammar (`comment_out`, `delete`, `inject_before`, `inject_after`, `replace`, `tool_remap`)
and line rules: README § Start Edited Print.

**Decision.** `choose_edit_transport(plugin_feature("server_edits", 1), archive_plate,
expressible)` returns Plugin only for Ready, not a `.3mf` plate, and a list `to_server_edits`
could write. A list cannot travel when it is empty, mixes edits and a remap, puts a range on a
kind without one, has a line 0 or negative tool, or holds text that is not UTF-8 (one validator for the app,
`decode_utf8` in `include/utf8_validation.h`).

**Fallback.** `DownloadRewrite`: the screen downloads the file, rewrites it and uploads the copy,
after `check_upload_space` (the storage floor, plugin or not). The copy ends with a newline
exactly when the original does, on both paths (Klipper never runs an unterminated final line).

**Testing.** `make t F='[plugin_server_edits]'` and `pytest moonraker-plugin/tests/test_helix_edits.py`.
Two implementations of one grammar are pinned by one fixture set: a new edit kind or line rule
needs a case in `tests/fixtures/gcode_edits/`, which both suites read and must turn into
byte-identical output.

---

## Filament Accounting (`filament_accounting` v1)

Moonraker's `[spoolman]` component is the only deductor: it charges ONE active spool the rise
of `toolhead.position[3]` above a high-water mark (retracts are not counted twice), re-basing
without a charge when `toolhead.extruder` changes or the active spool is switched. Usage is
right exactly when the active spool is switched at every tool or lane change. This feature
switches it for printers whose filament system does not, and records per job what each spool
was charged. **It never deducts**, so double counting is impossible by construction. Acts on a
spool: opt-in through `plugin_features.filament_accounting`, default off. Switch: Settings >
Devices > Spoolman > Switch spools at each tool change (Weight Sync group).

Who already switches the active spool (source survey, 2026-10-20):

| System | Sets Moonraker's active spool | `active_spool_owner` |
|---|---|---|
| AFC | yes, on lane load, and on a link of the loaded lane (`SET_SPOOL_ID`) | Backend |
| Happy Hare v3/v4, spoolman mode not off | yes, on a gate load (not on a gate-map edit) | Backend |
| AD5X IFS | no; its `manages_active_spool()` keeps Spoolman writes to explicit edits | Backend |
| Happy Hare, spoolman mode off | no | Helix |
| U1 with paxx SpoolLink | yes, on extruder change | Printer (stay out) |
| `T<n>` macros that call `SET_ACTIVE_SPOOL` (some INDX and klipper-toolchanger configs) | yes | Macros |
| U1 stock, CFS, ACE, toolchanger macros without `SET_ACTIVE_SPOOL` | no | Helix |

| File | Role |
|------|------|
| `moonraker-plugin/helix_filament.py` | Pure rules (`active_spool_owner`, `signal_key`, `parse_spool_map`, `feeding`, `spool_switch`, `UsageLedger`, `same_job`, `job_record`) and `FilamentAccounting` (status subscription, history handler, `filament_usage` endpoint, aux entry) |
| `tests/fixtures/filament_accounting/cases.json` | The ownership, key and switch rules, read by both suites; `tests/test_helpers/filament_accounting_cases.h` checks each real backend reports its row's `manages_active_spool` |
| `include/filament_accounting.h`, `src/printer/filament_accounting.cpp` | The same rules in C++, `printer_switches_active_spool`, `app_sets_spool_on_edit`, `filament_spools_record`, `extruder_feed_signal`, `filament_accounting_sender`, `filament_usage_available`, `parse_filament_usage`, `describe_spool_usage`; exception-free, compiled for the K-Touch |
| `include/filament_accounting_controller.h`, `src/printer/filament_accounting_controller.cpp` | `active_spool_owner_now`, `helix_sets_active_spool`, `helix_sets_active_spool_on_edit` (every app path that sets the active spool asks one of these); `FilamentAccountingController` keeps the map current and is the app fallback at a tool change |
| `include/ams_backend.h#feed_slot_signals` | Capability: the status fields that say which slot feeds now. INDX: `save_variables.variables.active_tool` (one extruder with docked nozzles, so `toolhead.extruder` never changes) |
| `include/ams_backend.h#sets_active_spool_on_link` | Capability: the firmware sets the active spool itself when the loaded lane is linked (AFC) |
| `src/printer/ams_state.cpp#commit_slot_edit`, `src/printer/ams_state_external_spool.cpp` | The explicit edits: a lane link or unlink, the external spool |
| `src/ui/print/ui_panel_history_list.cpp#fetch_spool_usage` | History detail's Spools lines |

**Records.** The app writes `helix-screen` / `filament_spools` = `{"version": 1,
"manages_active_spool", "printer_switches_active_spool", "macros_set_active_spool", "signals":
[{"object", "path", "values": {"<key>": {"tool", "lane", "spool_id" (null = unlinked),
"grams_per_mm"?}}}]}`, so the plugin learns no backend. Signals come from each backend's
`feed_slot_signals()` (slot -> `get_slot_info()` spool, `mapped_tool`, density from the
material) and, on a printer with more than one tool, `toolhead.extruder` with each tool's
extruder (an extruder two tools share is left out). The first signal whose value maps wins.
`manages_active_spool` is any backend's `manages_active_spool()`; `printer_switches_active_spool`
is `printer_switches_active_spool(hw)`; `macros_set_active_spool` is any `T<n>` macro whose
template switches the spool (`MacroParamCache::sets_active_spool`). Only the
`fallback_sender_role` Sender writes it, only when it has a signal, through
`post_item_if_changed`; it is rewritten when the plugin probe answers or the plugin is found
installed. The plugin keeps namespace `helix_filament`: `job_<job id>` = `{"schema": 1,
"job_id", "filename", "start_time", "status", "end_time", "continues", "spools": [{"tool",
"lane", "spool_id", "mm", "grams"}]}` (`grams` null when the screen could not resolve the
material's density), `active` (the open ledger, written every 30 s while filament moves, and at
switches and job start), `last` (the last ledger closed) and `last_added` (the id of the job
history added last, counted or not). Newest 500 job records kept. Where history has `get_job` /
`save_job`, the `spools` list is also the job's `filament_spools` aux entry, written when
history finishes the job ([History rows](#history-rows)); on API 1.4.0 the namespace record is
the only store.

**Endpoint.** `GET /server/helix/filament_usage?job_id=X[&start_time=T]`
(`server.helix.filament_usage`, in `kReadonlyMethods`): `{"job_id", "enabled", "record"}`: the
stored record, the live ledger (`status: "in_progress"`) of the job printing, or null. With
`start_time` (the app sends history's), a record of another job under a reused id answers null
(`same_job`, within 1 s). 400 without `job_id`.

**Plugin behavior.**
- **Subscriptions.** `toolhead` (position, extruder), `print_stats` and each signal object's
  first path field at klippy ready. A map written or rewritten later names objects that
  subscription missed, so at job start and at every switch the plugin subscribes any signal
  field it is not yet receiving (`_subscribe_signals`).
- **Reconnect.** The first reading of a connection only re-bases, so a reconnect mid-print never
  switches to a stale spool; a change made during the outage is picked up at the next change.
- **Switching.** While a job is printing or paused, a change of the first mapped signal value
  switches the spool, and history's `added` asserts it once from a fresh `query_objects`. Idle
  tool changes are left alone. Each switch re-reads the map, so a spool linked or unlinked
  mid-print counts at the next change. Switches run one at a time (`asyncio.Lock`) and decide
  from the newest status after their last await, so a quick T0 -> T1 -> T0 ends on T0's spool.
- **Ledger.** A job is counted when it starts with the feature on; the ledger charges the spool
  Moonraker actually has active, labelled with the feeding tool and lane, so the record is right
  on AFC and Happy Hare printers too, where the plugin switches nothing. A job closed by the next
  job's `added` with no finish has `status: null`.
- **Resume.** `plr_resume` continues the interrupted job's ledger (`continues`), and only that
  job's: the one `helix_resumed_job()` names (PLR's interrupted job id), else the job history
  added just before the resume (`last_added`; PLR resumes only the newest job). The base is the
  open ledger, or after a restart `last`, and it is used only when its job id is that job, so a
  QIDI resume printing a `.cache` copy under an absolute path still continues, and a resume
  after an uncounted job starts fresh.

**Decisions.**
- `active_spool_owner(spoolman, manages, printer_switches, macros)`: NoSpoolman, Backend,
  Printer, Macros or Helix. `printer_switches_active_spool(hw)` is the one place that knows
  SpoolLink (`PrinterDiscovery::has_spoollink()`: a `spoollink` object or `[spoollink]`
  section). Pinned by `cases.json` `ownership`.
- **Automatic switches** (the plugin, the app fallback, `AmsState::sync_current_loaded_from_backend`
  on a lane load, both AmsPanel posts) act only for Helix (`helix_sets_active_spool`).
- **Explicit edits** have their own rule, `app_sets_spool_on_edit(owner, backend_sets_on_link)`,
  because a switcher that acts on a load or a tool change never sees a link edit: Helix and
  Macros set; Backend sets unless the firmware sets it itself on a link (AFC,
  `sets_active_spool_on_link()`), so the app sets it for Happy Hare, AD5X and INDX; Printer
  (SpoolLink) and NoSpoolman leave it. `commit_slot_edit` sets only for the loaded lane, and
  the lane editor never posts a newly created spool itself. The external spool sets or clears
  only while it is what feeds (`AmsState::external_spool_feeds`: no filament system, a backend's
  bypass engaged, or no lane loaded). An unlink, of a lane or of the external spool, clears the
  active spool whoever switches spools (no firmware drops a link Moonraker holds), but only
  while it is still the spool unlinked (`AmsState::clear_active_spool_if`).
- `spool_switch` / `decide_spool_switch(feed, active, mapped)`: set a linked feed's spool; an
  unlinked feed clears only a spool the map links elsewhere. Pinned by `cases.json` `switch`.
- `signal_key` / `feed_signal_key`: integer (or integral float) as digits, non-empty string as
  itself. Pinned by `cases.json` `keys`.
- App `filament_accounting_sender(plugin_feature("filament_accounting", 1), opted_in, owner,
  role)`: owner not Helix -> None; Ready -> Plugin; Disabled -> None; Absent -> App when opted
  in and Sender.
- `filament_usage_available(state)`: the history detail asks the plugin when the feature is
  reported at all (Ready or Disabled; records outlive the switch).

**Fallback.** `FilamentAccountingController` switches the active spool when the active tool
changes mid-print, for tools that print from their own extruder with a spool assigned to the
tool (`ToolInfo::spoolman_id`, `backend_index < 0`): only while the feature is not Ready, only
when opted in and the owner is Helix, only for the Sender, and only after this connection's
plugin probe. It reads the current active spool (`get_spoolman_status`) and applies
`decide_spool_switch`. Backend-fed lanes on a Helix-owned printer are switched by the lane-load
sync. The fallback misses changes while the app is down, does nothing for single-extruder lane
changes, and records no per-job usage; the plugin closes those gaps.

**Not covered yet.** CFS and ACE publish no feed signal: CFS has two dialects
(`box.T<n>.filament` letters and the K1 wrapper's `loaded_slot`), and ACE's seat comes partly
from a REST poll, not a status field. Each needs its backend to name a status field in
`feed_slot_signals()`; the record's multi-signal shape already fits CFS's per-unit letters.
Until then the plugin does nothing there, and the screen's lane-load sync covers them while it
is connected. On a Moonraker that does not re-base at a spool switch, the record matches the
deduction only approximately.

**Testing.** `pytest moonraker-plugin/tests/test_helix_filament.py`;
`make t F='[plugin_filament],[active_spool],[commit],[spoolman_sync]'` (includes the INDX
feed-signal case and the Happy Hare, AFC and INDX case-table checks). A rule change edits
`tests/fixtures/filament_accounting/cases.json`, which both suites read. Mock:
`HELIX_MOCK_HELIX_PLUGIN=1` reports the feature (enabled from the mock database opt-in) and
serves a two-spool record for every job.

---

## Printer Event Journal (`event_journal` v1)

A record of what happened to the printer: klippy shutdowns with their `state_message`, lost
MCUs, klippy disconnect and ready, firmware restarts, pauses with their cause, runout trips
during a job, classified `!!` lines, and how each job ended. Each entry carries a wall-clock
time and the job history had open. The plugin keeps it whichever client is connected; without
it the screen keeps its own and backfills gaps on reconnect. Observer: `always_on`, no switch.

| File | Role |
|------|------|
| `moonraker-plugin/helix_journal.py` | Pure rules (`classify_error`, `is_mcu_disconnect`, `pause_cause`, `runout_trips`, `JournalStore`) and `HelixJournal` |
| `tests/fixtures/journal/error_line_classes.json` | Pins the Python and C++ `!!` classifiers to one table |
| `include/printer_event_journal.h`, `src/printer/printer_event_journal.cpp` | `JournalEntry`, `classify_journal_error`, `decide_journal_source`, wire and backfill parsers, `describe_journal_entry` (firmware-compiled) |
| `include/event_journal_recorder.h`, `src/application/event_journal_recorder.cpp` | `EventJournalRecorder` (the fallback recorder and reconnect backfill) and `LocalEventJournal` |
| `src/ui/printers/ui_printer_events_overlay.cpp`, `ui_xml/printer_events_overlay.xml` | The Printer Events view, opened from the notification history header |
| `src/system/debug_bundle_collector.cpp#build_event_journal` | The debug bundle's `event_journal` section |

**Endpoint.** `server.helix.journal {since=0, limit=100}` (limit capped at 500) returns
`{entries, truncated, latest_id, enabled}`, oldest first; `latest_id` is the next `since`. Each
append is broadcast as `notify_helix_journal_appended [entry]`. Entry kinds, classes and the
`detail` field: README § Printer Event Journal. Storage: namespace `helix_journal`, one key per
entry, 500 entries or 30 days. A `gcode_error` repeating one of the `REPEAT_LOOKBACK` (16)
newest entries (same text, class, detail and job) within `REPEAT_WINDOW_S` (60 s) of its
`last_time` is counted on it: `count` (absent means 1) and `last_time`. A repeat is written back
at most every `REPEAT_PERSIST_S` (10 s), and the next new entry flushes its final count; it is
broadcast only when first recorded.

**Decision.** `decide_journal_source(plugin_feature("event_journal", 1), recorder_built)`:
Plugin only for Ready; everything else is Screen with `record_locally` in a build that has the
recorder (`kLocalJournalRecorderBuilt`), and None in one that does not. The ESP32 firmware
(K-Touch) leaves the recorder out, so without a Ready journal its view shows "Printer events
need the HelixPrint plugin on this screen". The view, the recorder and the debug bundle all ask
it, and the view falls back to the screen journal on a plugin RPC error.

**Fallback.** `EventJournalRecorder` records only while the plugin's journal is not Ready, into
`<config dir>/event_journal.json` (200 entries, 30 days). On every (re)connect it backfills
since its newest entry from `server.gcode_store`, `server.history.list` and, after a shutdown, a
klippy.log tail. Backfilled entries carry `recovered` and are de-duplicated by time and text.
The screen cannot recover firmware restarts, the "command" pause cause, runout trips, a shutdown
with no wall-clock reference in the log window, or anything that scrolled out of those sources
(such as MCU disconnects during a long outage).

**Testing.** `cd moonraker-plugin && ../.venv/bin/python -m pytest tests/test_helix_journal.py`;
`make t F='[plugin_event_journal]'` (recorder cases also `[event_journal_recorder]`) and
`make t F='[notifications]'`. Mock: `HELIX_MOCK_HELIX_PLUGIN=1` serves five journal events;
without it the screen journal can be seeded through `$HELIX_CONFIG_DIR/event_journal.json`.

---

## Camera Photos (`photos` v1)

Two pictures from the printer's camera per job: one once the print finishes layer 2, one when
it ends completed, canceled or in error. The plugin takes them for every job; without it, or
while it cannot reach the camera, the screen takes them while it is connected. Either way the
bytes are stored unchanged in `gcodes/.helix_photos/` and listed in one record per history job.
Observer: on unless opted out (`plugin_features.photos`), with an app fallback, so it has a
switch: Settings > Devices > Print photos.

| File | Role |
|------|------|
| `moonraker-plugin/helix_photos.py` | Pure rules (`layer_two_done`, `pick_webcam`, `pick_snapshot_url`, `snapshot_candidates`, `redirect_allowed`, `photo_path`, `with_photo`, `newer_record`, `history_matches`, `prune_plan`, `orphan_files`) and `HelixPhotos` (history and status handlers, reachability, capture, record, aux link, prune, `delete_older_than`) |
| `include/print_photos.h`, `src/printer/print_photos.cpp` | Constants (`kPhotosDir`), `photo_taker`, `layer_two_done`, `ends_with_photo`, `is_photo_path`, record and aux parsers, `photo_record_with`, `photo_record_is_newer`, `merge_photo_lists`, `photo_records_to_prune`; firmware-compiled |
| `include/print_photos_controller.h`, `src/printer/print_photos_controller.cpp` | `PrintPhotosController`, the app fallback, owned by `SubjectInitializer` |
| `include/ui_history_photos.h`, `src/ui/print/ui_history_photos.cpp` | `HistoryPhotoViewer`: the detail card's photos, `default_photo_view`, `photo_for_view`, `delete_photos` |
| `include/webcam_selection.h#photo_pick`, `src/api/webcam_selection.cpp#view_config_for` | The camera both sides photograph; the flip and rotation a photo is shown with |
| `include/camera_stream.h#camera_view_transform` | That transform as the camera widget applies it |

**Files and record.** `gcodes/.helix_photos/<job id>_<int start_time>_<layer2|end>.jpg`
(the start time keeps a reused job id from overwriting an older job's photos). Namespace
`helix_photos`, key = the job id as history spells it:
`{"version": 1, "job_id", "filename", "start_time", "photos": [{"kind": "layer2"|"end", "path",
"size", "taken_at", "source": "plugin"|"app"}]}`, layer photo first. A record answers only for
the job whose `start_time` matches (`history_matches` / `photo_record_matches`, within 1 s). A
record of a newer version is never pruned, counted, overwritten or deleted with its files, on
either side (`newer_record`, `photo_record_is_newer`). No endpoint; the app reads the namespace
and downloads the files.

**History link.** Where history has `get_job` / `save_job`, the plugin writes the `photos` aux
entry (`value`: the paths; [History rows](#history-rows)), from history `finished` only (any
status, so a job that ended in a shutdown still links its layer photo), never on a row still
`in_progress`: history saves the running row over anything written to it. It links only a
record holding a photo with `source: "plugin"`; photos the screen took are linked by nobody,
since a link made from a partial record could miss the ones still on their way. The app's
history parser reads the entry into `PrintHistoryJob::photos`. The detail view always reads the
record too and shows `merge_photo_lists(linked, record)`: one photo per kind, the record's on
conflict, so a photo shows whichever side took it. It opens on the End photo when a job has
one (`default_photo_view`); Preview / Layer 2 / End tabs switch. Photos are decoded with the
camera's flip and rotation (`camera_view_transform`: the camera widget's, when it shows that
camera, else Moonraker's); the bytes on disk stay as the camera sent them.

**When.** Layer 2: `print_stats.info.current_layer` seen moving from 1 or 2 to above 2 in a job
history `added` (plugin: subscribed `print_stats.info`; app: the print-layer subject, then
`slicer_current_layer()` on the next tick). So a job first seen past layer 2 and a
`plr_resume` job get none, and so does a print whose slicer emits no layer info. End: history
`finished` with status `completed` / `cancelled` / `error`, after history saved the row.

**Camera.** `pick_snapshot_url` / `webcam::photo_pick`, one rule in two languages: of the
enabled webcams with a snapshot URL that is not an HTML page, the HelixScreen screen-server
entry excluded, the first MJPEG one, else the first. No webcam: nothing happens. The app
resolves the URL through `resolve_webcam_url`. A relative snapshot URL is served by the web
frontend, whose port the plugin cannot know, so it tries `snapshot_candidates`: the origin of
the camera's absolute stream URL, then `http://127.0.0.1` on ports 80, 4408 and 4409, and
remembers the one that answered. Fetches use no proxy and follow a redirect only to the same
origin (`redirect_allowed`).

**Reachability.** The plugin tries the camera at klippy ready and at every history `added`
(`check_reachable`), but only while the user has photos switched on: the switch alone decides,
not the feature's availability, so a camera that answers again is noticed while the feature
still reports itself unavailable. When no candidate answers a
relative URL, the feature reports `available: false`, reason `snapshot_unreachable: <url>`
(one warning), so `plugin_feature` reads Absent and the screen takes the photos; the Devices
row keeps its fallback switch. An absolute URL that fails stays available (the frontend port is
not the question there). Each flip, either way, sends `notify_helix_features_changed`, and the
app re-reads the map at every `added` anyway ([the app side](#the-app-side-plugin_feature-and-one-decision-function)),
so the hand-over happens at job start, before either photo is due.

**Bounds (plugin).** 8 MiB per photo, a JPEG SOI required; nothing taken under 256 MiB free.
Prune: dead records (history job gone or id reused) are found at startup and at most once a day
(`FULL_PRUNE_INTERVAL_S`); after every job end, the bounds: oldest until at most 500 jobs and
128 MiB, then unlisted files over an hour old (no sweep while `.helix_photos` is a link or a
newer-version record exists). History that cannot be asked keeps records (the bounds still
apply). The storage report finds this consumer at `gcodes/.helix_photos`; its cleanup goes
through `HelixPhotos.delete_older_than`, which removes the files, the record and this plugin's
aux entry together, and automatic cleanup never touches it.

**Decision.** `photo_taker(plugin_feature("photos", 1), opted_in, can_capture, role)`: Ready ->
Plugin; Disabled -> None; Absent (no plugin, or `snapshot_unreachable`) -> App only when opted
in (default on), this build can fetch a snapshot and the printer lists a camera, and
`fallback_sender_role` is Sender.

**Fallback.** `PrintPhotosController` (job events from `HistoryJobEvents`) fetches through
`live_camera_sources().fetch` (HttpExecutor, 4 MiB cap), checks the bytes are a JPEG
(`sniff_image_format`), uploads them with `upload_file_with_name` on `UploadLane::Bulk` (the slow executor, so a
photo never queues ahead of the UI's own requests), then reads, extends and writes the record, one write at a time
so two photos landing together both reach it, then trims to 500 jobs
(`photo_records_to_prune`; the screen cannot see sizes cheaply, so the bound is a count).
Deleting a job in the screen's history deletes its photos and record; a delete in another
client leaves them until the plugin prunes. Misses prints while the screen is off or away. The
K-Touch (ESP32) compiles all of it but has no fetcher (no camera stack, and upload is
unimplemented on its transport) and no JPEG decoder, so it neither takes nor shows photos; with
the plugin, the printer still takes them and other screens show them.

**Testing.** `pytest moonraker-plugin/tests/test_helix_photos.py` (3.8+);
`make t F='[plugin_photos]'`. Mock: `HELIX_MOCK_HELIX_PLUGIN=1` reports `photos`
(`HELIX_MOCK_PHOTOS=unavailable` reports it `snapshot_unreachable`, so the screen takes them);
`HELIX_MOCK_PRINT_PHOTOS=1` gives the newest mock history job both photos, served from
`assets/test_gcodes/mock_print_photo_{layer2,end}.jpg`.

---

## Storage Report and Guard (`storage` v1, `storage_auto_cleanup` v1)

What fills the printer's disks, by consumer; a cleanup per consumer; a free-space floor for the
plugin's own print starts; and an opt-in daily cleanup by age. `storage` only observes until
the user taps Clean up: `always_on`, no switch. `storage_auto_cleanup` acts on files:
`plugin_features.storage_auto_cleanup`, default off, plugin-only. Switch: Settings > System >
Storage > Automatic cleanup.

| File | Role |
|------|------|
| `moonraker-plugin/helix_storage.py` | `CONSUMERS` (the one table: name, root, dirs, cleanable, min age, auto age), pure `classify_gcodes_path`, `name_allowed`, `root_problem`, `cleanup_tops`, `scan_usage`, `cleanup_candidates`, `free_space_refusal`; `HelixStorage` (endpoints, `protected_paths`, `check_free_space`, `auto_cleanup`) |
| `moonraker-plugin/helix_print.py` | `_start_storage`, `check_free_space` (called by `start_linked_print`; `helix_edits.py` calls it before writing a copy), `staging_dirs`, `link_lock`, `recovery_protects` |
| `moonraker-plugin/helix_plr.py` | `HelixPlr.protected_paths()`, `HelixPlr.resume_unaccounted()`: what a waiting snapshot resumes from |
| `include/storage_report.h`, `src/printer/storage_report.cpp` | `kConsumers`, `report_source`, `parse_storage_result`, `classify_gcodes_path`, `parse_directory_result`, `fallback_cleanable`, `fallback_may_clean`, `timelapse_root_safe`, `timelapse_listing_is_own`, `fallback_removes`, `cleanup_message`, `upload_space`, `is_no_space_refusal`, `kFreeSpaceFloorBytes`; exception-free |
| `include/storage_report_service.h`, `src/printer/storage_report_service.cpp` | `StorageReportService` (plugin fetch or file-API scan, cleanup either way), `check_upload_space` |
| `src/ui/settings/ui_settings_storage.cpp`, `ui_xml/settings_storage_overlay.xml`, `ui_xml/components/storage_consumer_row.xml` | Settings > System > Storage |

**Consumers.** In fixed order: `helix_temp` (print copies), `3mf_outputs`, `analysis_sidecars`,
`photos`, `timelapse`, `logs`, `thumbnails`, `user_gcode`, `plr` (`helix-plr/`: the power-loss
snapshot and mesh file). Cleanable: `helix_temp` (copies at least 10 minutes old),
`analysis_sidecars`, `photos` (only while the photos module runs), `timelapse` and `logs` (each
at least 5 minutes old). Never cleanable: `3mf_outputs` (the printable form of a `.3mf`;
deleting it deletes the user's print, and the converter would not recreate it until the source
changes), `thumbnails`, `user_gcode`, and `plr` (deleting a waiting snapshot ends recovery). The
photos and staging directories are one constant each, shared across modules (`PHOTOS_DIR` /
`kPhotosDir`, `HelixPrint.staging_dirs()` / `gcode::staging_dirs()`).

**Cleanup trusts no configured directory.**
- `root_problem()`: each staging directory name must be one plain path segment and not another
  consumer's directory (`.helix_index`, `.helix_photos`, `.thumbs`); the timelapse and logs
  roots, resolved, must not be or hold the gcodes, config, data or database directory. While
  one fails, both features report `available: false` with the reason
  (`unsafe_staging_dir: '<name>'`, `unsafe_root: '<path>'`) and cleanup answers 409.
- `cleanup_tops()` resolves every directory a cleanup walks and requires it strictly inside its
  root; a staging directory that is a link is refused.
- Positive name rules (`name_allowed`): timelapse removes only render outputs (`.mp4 .mkv
  .webm .avi .mov .gif .jpg .png`), logs only rotated logs (`<name>.log.<suffix>`), analysis
  only complete layer indexes.
- Protected (`HelixStorage.protected_paths`): an active linked print's copy and link, the file
  `print_stats` reports printing or paused (named and resolved), every file a waiting power-loss
  snapshot resumes from (`HelixPlr.protected_paths()`: its print path, the job's file and what
  its link names, its layer indexes, QIDI's resume copy, the snapshot itself), and the layer
  index a running analysis uses. When `print_stats` cannot be read, the cleanup removes nothing
  and reports one error. While a snapshot waits with no readable job record
  (`resume_unaccounted()`), print copies and sidecars are not cleaned at all.
- Photos are removed through `HelixPhotos.delete_older_than`, so the record and the history
  link go with the files. Sidecar cleanup runs `prune_layer_indexes` first and leaves `.tmp`
  files to it. Removal of copies and links holds `HelixPrint.link_lock()`, which a linked print
  start also holds while it makes its link.

The plugin's own scheduled removal of a print copy asks the same question
(`HelixPrint.recovery_protects`): a copy or link a waiting resume needs is kept and checked again
later, and the startup cleanup runs only after PLR has loaded its job.

**Endpoints.** `server.helix.storage` -> `{volume: {total, used, free} | null, floor, consumers:
[{name, root, bytes, files, available, cleanable}], auto_cleanup: {enabled, last_run}}`.
`server.helix.storage_cleanup {consumer, older_than=0}` -> `{consumer, removed, freed, errors}`;
400 for an unknown or uncleanable consumer or a bad `older_than`; 409 for an unsafe root, or photos while the photos module is not running.
Sizes are file sizes, symlinked directories are not followed, and a root nested in gcodes counts
once. `storage` is in `kReadonlyMethods`; `storage_cleanup` is refused to Lua plugins. README
§ Storage Report and Cleanup has the table.

**Guard.** `print_modified` / `print_edited` answer 507 when the copy would leave less than
`FREE_SPACE_FLOOR_BYTES` (100 MiB) on the gcodes volume: `print_edited` before writing (counting
the original's size), `print_modified` before linking the uploaded copy, which it then deletes.
An unreadable volume never refuses. Without `helix_storage.py` there is no guard. In the app a
507 is final (`is_no_space_refusal`): no stock `printer.print.start` is tried in its place, the
screen-start note is withdrawn, and the user sees the space message
(`print_start_failure_tag`).

**Automatic cleanup.** First run 10 minutes after start, then every 24 h, only while the opt-in
is on and `print_stats` is idle; a run that finds the printer busy tries again an hour later.
Print copies older than 7 days, rotated logs older than 30 days. Photos, timelapse videos and
user files are never deleted automatically.

**Decisions.** `report_source(plugin_feature("storage", 1))`: Plugin only for Ready (the page
falls back to the file API if the plugin's reply fails). `cleanup_message(outcome)`: Freed,
FreedPartly ("some files could not be deleted"), NothingFreed ("Could not free space") or
NothingToFree, from the plugin's `errors` or the fallback's failed deletes. `upload_space(free,
size)`: TooFull below the floor, Unknown (go ahead) when unreadable. The auto-cleanup row is
`plugin_switch_mode` with `has_fallback=false`, so a plugin refusing an unsafe root hides it.

**Fallback.** The page lists `gcodes`, `timelapse` and `logs` with
`server.files.get_directory` (breadth first, at most 256 directories, `partial` shown past
that) and classifies files with the plugin's rules. It cannot see `.3mf` outputs (counted as
user files) or `helix-plr/`, so those rows hide. It cleans only timelapse
(`fallback_cleanable`): the app cannot ask which print copy, index or file a waiting snapshot
needs, photos belong to records only the plugin keeps, and Moonraker's logs root takes no
deletes. Even timelapse only when:
- no installed plugin reports `storage` unavailable (`fallback_may_clean`): a plugin refusing
  this printer's directories makes the page report-only;
- `server.files.roots` shows a timelapse root that is neither gcodes nor config nor above
  either (`timelapse_root_safe`), and its own listing holds no folder and only render files
  (`timelapse_listing_is_own`), which catches a second spelling of the same directory;
- the file passes the name rule and is at least 5 minutes old (`fallback_removes`), and the
  last scan listed it. Deletes go through `server.files.delete_file`.

Uploads the app makes (rewritten print copies, USB copies) check free space with
`check_upload_space` first, plugin or not, listing the target directory (the root only when the
directory does not exist yet): the plugin cannot see an upload before Moonraker takes it.

**Testing.** `pytest moonraker-plugin/tests/test_helix_storage.py` (`TestPlrProtectedPaths` and
`TestPlrQidiShapedJob` pin what recovery protects);
`make t F='[plugin_storage],[print][api]'`. Mock: `HELIX_MOCK_HELIX_PLUGIN=1` serves `storage`
(a fixed breakdown; cleaning `helix_temp` frees 412 MB) and `storage_auto_cleanup` from the mock
database.

---

## Config Read and Edit (`config_edit` v1)

The plugin reads the Klipper config the way Klipper does, with each option's file and line,
and writes edits atomically into the line that defines each option. `KlipperConfigEditor` uses
it for input shaper saves, PID/MPC migration and probe config rows when it is Ready. Transport:
own switch `always_on` (it writes only when the user saves on the screen); no `plugin_features`
key.

| File | Role |
|------|------|
| `moonraker-plugin/helix_config.py` | Reader (`read_config`), planner (`plan_edits`), writer (`write_plan`, `atomic_write`, backups), `HelixConfig` |
| `include/klipper_config_editor.h`, `src/system/klipper_config_editor.cpp` | `KlipperConfigEditor`, `config_edit_route`, `plan_plugin_config_edits`, `plugin_config_edit_backups`, `config_edit_target_file`, `config_backup_path` |
| `tests/fixtures/klipper_config_tree/` | One fixture tree and `expected.json` both suites read |

**Endpoints.** `server.helix.config {section?}`: the include-resolved config, per option
`{value, file, line, end_line, autosave}`, plus per-section `defined_in` and `errors`. It works
with Klipper down. Includes are read where their line stands; the root's `SAVE_CONFIG` block is
read last and supplies only options the regular config leaves undefined.
`server.helix.config_edit {edits: [{section, option, action, value, expect}], dry_run}` returns
`{dry_run, files, backups, diff}`; a refusal is 409 naming its rule first (`autosave`, `moved`,
`outside_root`, `invalid`). `invalid` includes a section or option name Klipper could not read
back (a newline, `[`, `]`, `#`, `;`; and `=`, `:` or whitespace in an option). Include globs
expand as Klipper's `glob.glob` does, without `recursive=True`, so `**` is one directory.
Files are written through `atomic_write_bytes` with their own mode. Backups go to `.helix_backups/<file>.helixbak-<stamp>` under the
config root, newest five per file. The plugin never restarts Klipper. Full shapes: README
§ Read and Edit the Klipper Config.

**Decisions.** `config_edit_route(plugin_feature("config_edit", 1))`: Ready goes to the plugin,
anything else to the file API. `plan_plugin_config_edits(section, edits, read)`: Edits, FileApi
(an option lives only in `SAVE_CONFIG`, or the list is empty) or Error.
`config_edit_target_file()`: an added key for a `SAVE_CONFIG`-only option goes to `printer.cfg`.

**Fallback.** The file API: download the include chain, edit in memory, back up to
`.helix_backups/<file>.helix_backup`, upload. Also used when the plugin read fails. Both paths
end in `restart_and_monitor()`: `FIRMWARE_RESTART`, wait for Klipper, and on timeout restore the
backups and restart again.

**Testing.** `.venv/bin/python3 -m pytest moonraker-plugin/tests/test_helix_config.py` (includes
a configparser oracle running Klipper's own read algorithm); `make t F='[plugin_config_edit]'`.
A change to either parser must keep `tests/fixtures/klipper_config_tree/expected.json` true for
both.

---

## Vendor State Push (`vendor_ad5x` v1, `vendor_ace` v1)

Two filament backends poll state Moonraker has no push for: AD5X IFS on native ZMOD downloads
`Adventurer5M.json` every 5 s idle / 30 s printing, and ACE on the REST bridge polls
`/server/ace/status` and `/slots` every 500 ms. Where the plugin runs, one module per vendor
(the vendor-abstraction rule) watches that state on the printer and broadcasts it only when it
changed; the backend stops its regular poll for as long as the feature is ready. One feature per
vendor, so the app never stops polling for a vendor the plugin cannot serve. Transports:
`acts=False`, `switch=always_on`, availability from a live probe, no switch.

| File | Role |
|------|------|
| `moonraker-plugin/helix_vendor_push.py` | Shared, vendor-free: `VendorPush.emit_if_changed`, the one `register_notification`, `ReadHealth` (whether the module's reads work), `Watcher` (an interval loop that survives a failing check), `websocket_clients` |
| `moonraker-plugin/helix_vendor_ad5x.py` | `vendor_ad5x`: stats `Adventurer5M.json` in the config root each second and on its `filelist_changed`, reads it only when the stat moved, and pushes it only when it parses as JSON |
| `moonraker-plugin/helix_vendor_ace.py` | `vendor_ace`: calls the bridge's own endpoint handlers in-process (`find_endpoint`, both the pre-0.9 and the 0.9+ endpoint registry); reads nothing until klippy ready has shown the bridge is the source, and nothing while Moonraker has no websocket client |
| `include/vendor_state_push.h`, `src/printer/vendor_state_push.cpp` | App side, vendor-free: `vendor_state_source()`, `vendor_state_payload()`, `VendorStateTracker`, `current_plugin_feature()`, `subscribe_vendor_state()`, `kVendorPushBackstop` |
| `src/printer/ams_backend_ad5x_ifs.cpp` | `#refresh_vendor_push`, `#on_vendor_state`, `#apply_json_content`, `#should_poll_json` |
| `src/printer/ams_backend_ace.cpp` | `#refresh_vendor_push`, `#on_vendor_state`, `#apply_rest_status`, `#apply_rest_slots`, `#note_rest_status_failure`, `#rest_data_poll_due` |

**Wire.** `notify_helix_vendor_state`, params `[{"vendor": ..., "payload": ...}]` (the
Moonraker event `helix_print:helix_vendor_state`, the journal's naming convention). The payload
is exactly what the poll reads, so the backend's own parse runs on it unchanged:

| Feature | vendor | payload | unavailable reasons |
|---|---|---|---|
| `vendor_ad5x` | `ad5x` | `{"file": "Adventurer5M.json", "content": <text>}` | `no_adventurer_json`, `adventurer_json_unreadable` |
| `vendor_ace` | `ace` | `{"status": <the /status result>, "slots": <the /slots result>}`; an endpoint that returned nothing reads `{"error": "unreadable"}`, and `failures`, the run of failed reads, is sent while it is not zero | `no_ace_bridge`, `ace_objects_carry_slots`, `bridge_unreadable` |

**Ready means the reads work.** `ReadHealth`: a feature is available only after one of its reads
has returned data, and unavailable again after `MAX_READ_FAILURES` (3) failed reads in a row
(`*_unreadable`). The app stops polling while a feature is ready, so a push that cannot read
must not look ready. A failed AD5X parse counts as a failed read and re-reads at the next tick.
Each pushed failure is a distinct payload, and the ACE backend routes it to the poll's own
failure handling (`note_rest_status_failure`).

`ace_objects_carry_slots`: after each klippy ready the module queries `filament_hub`, `ace` and
`ace_instance_0`; one with a non-empty `slots` list means the app reads ACE by subscription, so
there is nothing to replace. `no_adventurer_json`, `no_ace_bridge` and `ace_objects_carry_slots`
mean the hardware is absent, so they are the rows' `hidden_reasons` and the overview leaves the
feature out while one applies ([The overview](#the-overview)); an `*_unreadable` reason or a
module that fails to load still shows as Unavailable with its reason.

**Decision functions.** `vendor_state_source(plugin_feature(name, 1))`: Push only for Ready,
Poll for an absent plugin, a plugin without the map, disabled, unavailable, or below the
contract. `VendorStateTracker::update(state, probe_seq, connection)` folds it in and returns
true when the backend owes one read: on switching to Push, and on each later plugin probe while
pushing (a reconnect re-probes, and a restarted plugin may have missed a change). Ready counts
only from a probe first seen on the current `connection_generation()`, so a reconnect never
pushes on the last connection's answer, and both backends `reset()` it in `on_started` /
`on_stopping`. Plugin side: `objects_carry_slots()`, `bridge_answered()`, `ace_payload()`,
`names_adventurer_json()`, `payload_key()`.

**Backends.** Each subscribes at start (`subscribe_vendor_state`, payload deferred to the main
thread through the backend's lifetime token) and refreshes the tracker on its main-thread status
frames. A push is applied only while pushing, through the same function as a poll that saw a
change: AD5X `apply_json_content(content, "json_push")` (note, parse, the GET_ZCOLOR /
IFS_STATUS follow-up; nothing while the standalone module is live); ACE `apply_rest_status` /
`apply_rest_slots` (only on the REST path). While pushing, each backend still polls once every
`kVendorPushBackstop` (30 s; AD5X `should_poll_json`, ACE `rest_data_poll_due` in its REST
thread), so a push that stops arriving costs at most that long. The AD5X event-driven re-reads
(sensor edges, klippy ready) and the startup read stay.

**Fallback.** The regular polling, in every state but Ready, and at once when a probe stops
reporting the feature ready. An app that predates this feature keeps polling an ACE bridge
beside the plugin's reads (two cheap Klipper queries per 500 ms); nothing breaks.

**Testing.** `pytest moonraker-plugin/tests/test_helix_vendor_ad5x.py
moonraker-plugin/tests/test_helix_vendor_ace.py`; `make t F='[plugin_vendor]'` (backend cases
also carry `[ad5x_ifs]` / `[ace]`; the REST-thread case sleeps about a second of real time).
Backend details: [FILAMENT_BACKEND_AD5X_IFS.md](FILAMENT_BACKEND_AD5X_IFS.md),
[FILAMENT_BACKEND_ACE.md](FILAMENT_BACKEND_ACE.md).

---

## Adding a Feature

1. **Module.** `moonraker-plugin/helix_<feature>.py`, with its feature name and contract version
   as module constants. Keep the rules pure and the Moonraker-facing class thin. Python 3.8.
   A feature that keeps one record per gcodes file builds on `helix_file_queue.py`
   (`FileTracker`, `WorkQueue`, `RecordStore`, `scan_files`); one that reads whole files does
   so in a worker (`WorkerProcess`, `worker_timeout`); one that writes a file uses
   `atomic_write_bytes` / `atomic_publish`; one that records per job writes history rows only
   through `patch_history_job` / `set_history_aux`. A vendor's knowledge lives in its own
   module (`helix_vendor_<vendor>.py`). A sibling it imports by name loads before it in
   `helix_print.py`. G-code a backend already produces is rendered by the app and only
   sent by the plugin (print hooks); the plugin renders G-code only when it is built from a
   file scan the plugin runs, as power-loss recovery's resume is
   ([why](POWER_LOSS_RECOVERY.md#why-the-plugin-renders-the-resume-g-code)).
2. **Register.** A `_start_<feature>()` in `helix_print.py` that loads the sibling, registers the
   feature (`acts`, and `switch=always_on` if it has no setting), and registers it with
   `available=False` and its load error when the module is missing. Call it from
   `component_init` through `_start_isolated(name, acts, start)`, which handles a disabled
   plugin and a start that raises; a module checks `feature_enabled(name)` before it acts. A
   start-of-job feature acts from history `added` and checks `helix_initiated()`; a path that
   starts a job marks it.
3. **Install list.** Add the file to `PLUGIN_FILES` in `moonraker-plugin/remote-install.sh`;
   the release tarball is packed from the same list. `tests/shell/test_plugin_remote_install.bats`
   fails for any shipped `.py` or `.sh` the wrapper does not fetch.
4. **Allowlist.** A read endpoint joins `kReadonlyMethods` in
   `src/plugin/plugin_permissions.cpp` under its exact JSON-RPC name. A write endpoint stays out,
   and `reject_plugin_rpc` refuses it.
5. **App decision.** One pure function of `plugin_feature(name, min_version)` and the caller's
   inputs, tested over Absent / Disabled / Ready / below minimum / unavailable. Every caller asks
   it. A fallback that acts on the printer, or writes a record the plugin reads, asks
   `resolve_fallback_sender_role()`.
6. **Switch, only if the feature acts (or is an observer with a fallback).** One
   `kPluginSwitchFeatures` row (name, min version, `has_fallback`, `acts`, mode subject, on
   subject or nullptr for an own switch), one `kKnownPluginFeatures` row (label, icon,
   `PluginFeatureHome`), and on the page:
   ```xml
   <plugin_feature_toggle_row feature="x" label="..." description="..." icon="..."
                              subject="plugin_feature_on_x" callback="on_plugin_feature_switch_changed"/>
   ```
   A feature with no switch still gets a `kKnownPluginFeatures` row with
   `PluginFeatureHome::None`, so the overview shows its label instead of its key.
7. **Mock.** Report the feature from the mock's `server.helix.status` under
   `HELIX_MOCK_HELIX_PLUGIN=1` and serve its endpoint; document any new variable in
   `MOCK_ENVIRONMENT_VARIABLES.md`.
8. **Tests.** pytest for the module; a C++ tag for the decision and the consumer. Shared rules
   (a rule both sides implement) get one fixture under `tests/fixtures/` that both suites read.
9. **Docs.** The README's endpoint section, a section here, the user page
   (`docs/user/guide/helixprint-plugin.md`), and any plan doc for the feature deleted with the
   change that ships it.
