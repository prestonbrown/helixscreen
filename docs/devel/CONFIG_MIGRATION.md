# Config Migration System

How the versioned config migration system works, how to add new migrations, and how migrations are tested.

**Key files**: `include/config.h`, `src/system/config_migrations.cpp` (the ladder), `src/system/config.cpp` (`Config::init()`), `tests/unit/test_config.cpp`

---

## Overview

HelixScreen stores user configuration in a JSON file (`config/settings.json`). As the application evolves, the config schema changes: keys get renamed, defaults change, new sections appear. The migration system handles upgrading existing configs automatically so users never need to hand-edit JSON.

There are two migration layers:

1. **Structural migrations** (legacy) -- Key-path moves like `display_rotate` to `/display/rotate` or `/display/calibration` to `/input/calibration`. These run unconditionally based on key presence.

2. **Versioned migrations** -- Numbered `v9->v10`, `v10->v11`, etc. Each bumps the integer `config_version` field. These run in sequence and only on configs older than the target version.

---

## How It Works

### The `config_version` Field

Every config file has an integer `config_version` at the root level:

```json
{
    "config_version": 19,
    "printer": { ... },
    "display": { ... }
}
```

The current version is defined in `config.h`:

```cpp
static constexpr int CURRENT_CONFIG_VERSION = 28;
static constexpr int MIN_MIGRATABLE_CONFIG_VERSION = 9;
```

The full migration ladder lives in `config_migrations.cpp`: one function per step and one ordered table, `kMigrations`, that `run_versioned_migrations()` walks.

Two properties of the ladder worth knowing before you add to it:

- **Every step must be idempotent under replay.** A rollback to an older build stamps
  `config_version` back down, and the next upgrade re-runs the chain over a document
  already in the new shape. Each migration guards against re-doing its own work.
- **A config written by a newer build is left alone.** `run_versioned_migrations()`
  detects a `config_version` above `CURRENT_CONFIG_VERSION` and returns without
  stamping, so an older binary never rewrites data it does not understand.

The current head of the ladder:

- **v23 -> v24** tags every saved home layout with `layout_units: "cells_v21"` and a
  `legacy_rows` floor harvested from the old `/ui/cached_grid` node, then lifts legacy
  flat widget arrays into the page shape. The tag is all this migration writes: it runs
  at config load, before the screen size is known, so the actual coordinate conversion
  happens later, at the first square-cell grid build, in
  `port_legacy_layout()` (`include/layout_port.h` + `src/ui/home/layout_port.cpp`), which
  drops the tag when done. Three guards keep a replay from converting twice: a panel
  that already carries `grid`/`parked_grids` (per-grid storage postdates the
  migration), one already tagged, and one with no coordinates at all.
- **v24 -> v25** re-keys the pre-print prediction history's per-phase timing from
  phase ordinals to phase names (`"4"` -> `"QGL"`), against a frozen name table,
  because inserting a phase renumbers every ordinal after it.
- **v25 -> v26** converts `/completion_alert` from a JSON boolean to the
  Off/Notification/Alert int `AudioSettingsManager` has always read and written.
  A pre-migration fresh install wrote the boolean `true`, and `Config::get<int>()`
  converts that via nlohmann's bool-to-arithmetic rule (`true` -> 1, `false` -> 0)
  rather than the intended `CompletionAlertMode::ALERT` (2) — so every install
  that never touched Print Completion Alert silently fell back to Notification.
  `true` -> Alert, `false` -> Off; an explicit stored int is a real user choice
  and is left untouched.

- **v27 -> v28** rewrites an integer `/display/screensaver_type` as the stable saver
  name it stood for (`7` -> `"maze"`), through the frozen
  `kLegacyScreensaverTypeNames` table in `include/screensaver_registry.h`, so
  reordering, adding or compiling out a saver never changes a stored choice. An
  out-of-range number becomes the default saver's name; a string is left as it is.
  The reader (`include/screensaver_setting.h#screensaver_from_setting`) stays
  lenient on its own, so a hand edit or an old backup restored at the current
  version still reads correctly.

- **v29 -> v30** moves each printer's `ams/qidi_eject_distance` and `ams/qidi_eject_velocity`
  to the backend-neutral `ams/eject_distance` and `ams/eject_velocity`. The new keys default to
  0, which the ejecting backend reads as "use my own default", so a printer that never chose a
  value keeps the QIDI Box's 878 mm / 100 mm/s.

- **v30 -> v31** moves `/remote_screen/enabled` and `/remote_screen/control` into the
  active printer, where screen sharing is now kept per printer. Every other printer starts
  with sharing off, so turning it on for one printer never lists the screen among another
  printer's webcams. The pairing code, paired browsers and port stay global: they belong to
  the screen, not to a printer.

Each has a dedicated test: `tests/unit/test_config_migration_v24.cpp`,
`tests/unit/test_config_migration_v25.cpp`,
`tests/unit/test_config_migration_v26.cpp`,
`tests/unit/test_config_migration_v28.cpp`,
`tests/unit/test_config_migration_v30.cpp` and
`tests/unit/test_config_migration_v31.cpp`.

### Fresh Install vs. Upgrade

| Scenario | What happens |
|----------|-------------|
| **No config file** | `get_default_config()` creates one with `config_version = CURRENT_CONFIG_VERSION`. No migrations run. |
| **Existing config, no `config_version` and no printer** | Installer-seeded keys only: no `/printer`, and no printer object under `/printers` (e.g. `{"update":{"channel":1}}`, or the per-printer seed's `input`/`display` blocks). Treated as a fresh install: `get_default_config()` with the seeded keys laid over it by `merge_patch`, stamped CURRENT, so no migration runs over the seeded values. |
| **Existing config, no `config_version`** | Treated as version 0: a shipped preset (`assets/config/presets/*.json`), a tarball default, an installer seed holding a printer, or a user config from before v0.9.11 (the first release to stamp `config_version`). A rolling backup with a real version replaces it if one exists; otherwise `normalize_versionless_document()` moves the single `/printer` into the `/printers` map and the chain runs from v9. |
| **Existing config, `config_version` 1-8** | Below the floor. Copied to `settings.json.pre-migration`, one warning logged, and replaced by `get_default_config()`, the same defaults a missing config gets. |
| **Existing config, `config_version = 9`** | Only migrations after v9 run (v9->v10, ...). |
| **Existing config, `config_version = CURRENT`** | No migrations run. |

### Execution Order in `Config::init()`

```
1. Load config JSON from disk (or create default if missing). A versionless
   document holding no printer becomes get_default_config() with its keys
   laid over it, stamped CURRENT
2. Run structural migrations:
   a. migrate_display_config()  -- root-level display_* keys -> /display/
   b. migrate_config_keys()     -- /display/calibration -> /input/calibration
3. Run versioned migrations:
   a. Read config_version (default 0 if absent)
      If 0 < version < CURRENT and the document came from disk, copy
      settings.json to settings.json.pre-migration first
   b. If 0 < version < MIN_MIGRATABLE_CONFIG_VERSION: replace with defaults, stop
   c. If version == 0: normalize_versionless_document() (/printer -> /printers)
   d. Run each kMigrations row whose to_version > version, in order
   e. Set config_version = CURRENT_CONFIG_VERSION
4. Ensure required sections exist with defaults (printer, display, input, etc.).
   A /printers map with no printer object gets the default printer, except in
   a config from a newer build, which is left as written
5. Save to disk if anything changed
```

The `.pre-migration` copy is the only record of the pre-upgrade document: the save in step 5 also refreshes the rolling backup (`src/system/config_backup.cpp#write_rolling_backup`) with the migrated one. It holds one generation, overwritten by the next migrating boot from a different version. A copy already at the starting version is kept, since a migration that threw can leave settings.json partly migrated under its old stamp. No restore path reads it; recovering from it is a manual copy. Small-footprint storage (the K-Touch firmware, `ConfigFootprint::Small` in `include/config_storage.h`) keeps no such copy, since its 128 KB partition cannot spare one, and deletes any it finds at boot; a below-floor document is the exception, because it has no other copy.

Versioned migrations only run on **existing** configs. A fresh install skips straight to step 4 because `get_default_config()` already sets `config_version = CURRENT_CONFIG_VERSION`.

---

## The Migration Floor

`MIN_MIGRATABLE_CONFIG_VERSION` (9, first shipped in v0.99.4) is the oldest stamp the chain still migrates. A config stamped 1-8 is not migrated: `init()` keeps it as `settings.json.pre-migration`, logs `config_version N is older than this build migrates`, and starts from defaults. Raising the floor means deleting the steps below it, except any a version-0 preset still needs.

Version 0 is not below the floor. The shipped presets and pre-v0.9.11 user configs carry no `config_version` and use the single `/printer` shape, so `normalize_versionless_document()` moves that into the `/printers` map, gives a printer with no `leds` block an empty selection, and hides the printer switcher on a single-printer install, before the numbered chain runs. `tests/unit/test_config.cpp` loads every shipped preset through `Config::init()` to keep that path honest.

v17->v18 skips a version-0 document whose active printer has not completed the setup wizard: that is a preset or installer seed, and its touch calibration is a known-good matrix, so it gets no `recheck_pending`. A pre-v0.9.11 user config finished the wizard and is rechecked like any other.

---

## Adding a New Migration

### Step 1: Bump the version constant

In `include/config.h`:

```cpp
static constexpr int CURRENT_CONFIG_VERSION = 29;  // was 28
```

### Step 2: Write the migration function

In `src/system/config_migrations.cpp`, add a static function in the anonymous namespace alongside the existing migrations:

```cpp
/// Migration v28->v29: <description of what and why>
static void migrate_v28_to_v29(json& config, const std::string& /*config_path*/) {
    // Modify config in place; spdlog::info() what changed.
}
```

`config_path` is the settings file's path, for a migration that folds in a sidecar file (`migrate_v13_to_v14`).

### Step 3: Add a row to `kMigrations`

```cpp
    {28, migrate_v27_to_v28},
    {29, migrate_v28_to_v29},  // <-- ADD THIS
```

A `static_assert` fails the build if the last row does not reach `CURRENT_CONFIG_VERSION`.

### Step 4: Update `get_default_config()` if needed

If your migration changes a default value or adds a new key, make sure `get_default_config()` reflects the final state. Fresh installs use this function directly and skip migrations entirely.

### Step 5: Write tests

See the testing section below.

---

## Migration Rules

1. **Migrations are append-only above the floor.** Never modify an existing migration function; old configs in the wild may still need it. Steps below `MIN_MIGRATABLE_CONFIG_VERSION` are removed when the floor rises.

2. **Migrations must be idempotent.** Check if the target state already exists before making changes. Use `config.contains()` guards.

3. **Never overwrite user data.** If a target key already exists, skip the migration for that key. The user's explicit value wins.

4. **Keep migrations simple.** Each migration should do one thing. If you need to both rename a key and change its type, that is still one logical migration.

5. **Log what you do.** Use `spdlog::info("[Config] Migration vN: <what happened>")` so upgrade issues are diagnosable.

6. **Fresh installs skip everything.** `get_default_config()` returns the current schema directly. Migrations only run on existing configs loaded from disk.

---

## Testing Migrations

Migration tests are in `tests/unit/test_config.cpp` under the `[core][config][migration][versioning]` tags.

### Pattern: Write config to temp file, run init(), verify results

The standard pattern creates a temp config file with pre-migration data, runs `Config::init()` on it, and verifies the post-migration state:

`tests/unit/test_config.cpp` "a config at the migration floor is migrated" is the shape: write a stamped document to a temp file, wrap `init()` in a `BackupGuard` so no real rolling backup is found, then assert on the loaded result.

### What to test for each migration

| Test case | What it verifies |
|-----------|-----------------|
| Oldest stamp triggers migration | A config at the version before yours gets migrated |
| Already-migrated config is left alone | Config at version N does not re-run migration N |
| Fresh config skips migrations | New install gets `CURRENT_CONFIG_VERSION` without running migration logic |
| Edge case: key absent | Migration handles configs that never had the key being migrated |
| Version stamp is set | After all migrations, `config_version == CURRENT_CONFIG_VERSION` |

### Running migration tests

```bash
# Build and run the migration tests
make t F='[migration]'

# Run only versioning/migration tests, without a rebuild
./build/bin/helix-tests "[migration][versioning]"

# Run all config tests
./build/bin/helix-tests "[config]"
```

---

## Structural Migrations (Legacy)

Before the versioned system existed, two key-path migration helpers were used:

### `migrate_display_config()`

Moves root-level `display_rotate`, `display_sleep_sec`, `display_dim_sec`, `display_dim_brightness`, `touch_calibrated`, and `touch_calibration` into the `/display/` section. Triggered by the presence of `display_rotate` at the root level.

### `migrate_config_keys()`

A generic helper that takes a vector of `{from_path, to_path}` pairs and moves values between JSON pointer paths. Used to move `/display/calibration` to `/input/calibration` and `/display/touch_device` to `/input/touch_device`.

These still run on every `init()` call for backward compatibility with very old configs. New migrations should use the versioned system instead.

---

## Debugging

Run HelixScreen with `-vv` (DEBUG) to see migration log output:

```
[Config] Loading config from config/settings.json
[Config] Versionless config: restructured /printer to /printers/default
```

If a config file is corrupt (unparseable JSON), `init()` backs it up as `settings.json.corrupt` and creates a fresh default config.

---

## Config File Rename

The config file was renamed from helixconfig.json to `settings.json`:
- The installer renames it, in the install dir and in printer_data
- `Config::init()` follows a helixconfig.json symlink into printer_data when no `settings.json` exists
- Rolling backups fall back to old names if new-named backups don't exist
- Template renamed from `helixconfig.json.template` to `settings.json.template`
