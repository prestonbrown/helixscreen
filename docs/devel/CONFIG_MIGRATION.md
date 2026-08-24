# Config Migration System

How the versioned config migration system works, how to add new migrations, and how migrations are tested.

**Key files**: `include/config.h`, `src/system/config.cpp`, `tests/unit/test_config.cpp`

---

## Overview

HelixScreen stores user configuration in a JSON file (`config/settings.json`). As the application evolves, the config schema changes: keys get renamed, defaults change, new sections appear. The migration system handles upgrading existing configs automatically so users never need to hand-edit JSON.

There are two migration layers:

1. **Structural migrations** (legacy) -- Key-path moves like `display_rotate` to `/display/rotate` or `/display/calibration` to `/input/calibration`. These run unconditionally based on key presence.

2. **Versioned migrations** -- Numbered `v0->v1`, `v1->v2`, etc. Each bumps the integer `config_version` field. These run in sequence and only on configs older than the target version.

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
static constexpr int CURRENT_CONFIG_VERSION = 19;
```

The two migrations enumerated below (v0->v1, v1->v2) are the earliest examples; the
full migration ladder up to the current head lives in `config.cpp`. The "how to add a
migration" mechanism shown later still applies to every step in that ladder.

### Fresh Install vs. Upgrade

| Scenario | What happens |
|----------|-------------|
| **No config file** | `get_default_config()` creates one with `config_version = CURRENT_CONFIG_VERSION`. No migrations run. |
| **Existing config, no `config_version`** | Treated as version 0. All migrations run in sequence (v0->v1, v1->v2, ...). |
| **Existing config, `config_version = 1`** | Only migrations after v1 run (v1->v2, ...). |
| **Existing config, `config_version = CURRENT`** | No migrations run. |

### Execution Order in `Config::init()`

```
1. Load config JSON from disk (or create default if missing)
2. Run structural migrations:
   a. migrate_display_config()  -- root-level display_* keys -> /display/
   b. migrate_config_keys()     -- /display/calibration -> /input/calibration
3. Run versioned migrations:
   a. Read config_version (default 0 if absent)
   b. if (version < 1) migrate_v0_to_v1()
   c. if (version < 2) migrate_v1_to_v2()
   d. Set config_version = CURRENT_CONFIG_VERSION
4. Ensure required sections exist with defaults (printer, display, input, etc.)
5. Save to disk if anything changed
```

Versioned migrations only run on **existing** configs. A fresh install skips straight to step 4 because `get_default_config()` already sets `config_version = CURRENT_CONFIG_VERSION`.

---

## Existing Migrations

### v0 -> v1: Disable sounds for existing users

**Problem**: Before the sound system worked, configs had `sounds_enabled: true` as a harmless default. When sound support shipped, upgrading users would get surprise beeps.

**Fix**: Force `sounds_enabled` to `false` for any pre-v1 config that has the key.

```cpp
static void migrate_v0_to_v1(json& config) {
    if (config.contains("sounds_enabled")) {
        config["sounds_enabled"] = false;
        spdlog::info("[Config] Migration v1: disabled sounds_enabled for existing config");
    }
}
```

**Note:** Dedicated tests for this specific v1->v2 migration are not yet implemented. Later migrations in the ladder are covered (e.g. `tests/unit/test_config_migration_v18.cpp`).

### v1 -> v2: Single LED string to multi-LED array

**Problem**: LED config was a single string at `/printer/leds/strip`. Multi-LED support needs an array at `/printer/leds/selected`.

**Fix**: Convert the old string value into a single-element array, or create an empty array if no LED was configured.

```cpp
static void migrate_v1_to_v2(json& config) {
    json::json_pointer strip_ptr("/printer/leds/strip");
    json::json_pointer selected_ptr("/printer/leds/selected");

    if (config.contains(selected_ptr)) {
        return;  // Already has new format
    }

    if (config.contains(strip_ptr)) {
        auto& strip_val = config[strip_ptr];
        if (strip_val.is_string()) {
            std::string led = strip_val.get<std::string>();
            if (!led.empty()) {
                config[selected_ptr] = json::array({led});
            } else {
                config[selected_ptr] = json::array();
            }
        }
    } else {
        config[selected_ptr] = json::array();
    }
}
```

---

## Adding a New Migration

### Step 1: Bump the version constant

In `include/config.h`:

```cpp
static constexpr int CURRENT_CONFIG_VERSION = 3;  // was 2
```

### Step 2: Write the migration function

In `src/system/config.cpp`, add a new static function in the anonymous namespace alongside the existing migrations:

```cpp
/// Migration v2->v3: <description of what and why>
static void migrate_v2_to_v3(json& config) {
    // Your migration logic here.
    // The config JSON is passed by reference -- modify it in place.
    // Use spdlog::info() to log what changed.
}
```

### Step 3: Register it in `run_versioned_migrations()`

Add one line to the chain:

```cpp
static void run_versioned_migrations(json& config) {
    int version = 0;
    if (config.contains("config_version")) {
        version = config["config_version"].get<int>();
    }

    if (version < 1) migrate_v0_to_v1(config);
    if (version < 2) migrate_v1_to_v2(config);
    if (version < 3) migrate_v2_to_v3(config);  // <-- ADD THIS

    config["config_version"] = CURRENT_CONFIG_VERSION;
}
```

### Step 4: Update `get_default_config()` if needed

If your migration changes a default value or adds a new key, make sure `get_default_config()` reflects the final state. Fresh installs use this function directly and skip migrations entirely.

### Step 5: Write tests

See the testing section below.

---

## Migration Rules

1. **Migrations are permanent and append-only.** Never remove or modify an existing migration function. Old configs in the wild may still need them.

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

```cpp
TEST_CASE_METHOD(ConfigTestFixture,
                 "Config: v0 config with sounds_enabled=true gets migrated to false",
                 "[core][config][migration][versioning]") {
    // Set up pre-migration config data
    set_data_for_plural_test(
        {{"sounds_enabled", true},
         {"brightness", 50},
         {"printer", {{"moonraker_host", "192.168.1.100"}, {"moonraker_port", 7125}}}});

    // No config_version means v0
    REQUIRE_FALSE(data_contains("config_version"));

    // Write to temp file
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_migration_test_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::string temp_path = temp_dir + "/test_config.json";

    {
        std::ofstream o(temp_path);
        o << get_data().dump(2);
    }

    // Run init which triggers migrations
    Config test_config;
    test_config.init(temp_path);

    // Verify migration ran
    REQUIRE(test_config.get<bool>("/sounds_enabled") == false);
    REQUIRE(test_config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    std::filesystem::remove_all(temp_dir);
}
```

### What to test for each migration

| Test case | What it verifies |
|-----------|-----------------|
| v0 config triggers migration | Old configs without `config_version` get migrated |
| Already-migrated config is left alone | Config at version N does not re-run migration N |
| Fresh config skips migrations | New install gets `CURRENT_CONFIG_VERSION` without running migration logic |
| Edge case: key absent | Migration handles configs that never had the key being migrated |
| Version stamp is set | After all migrations, `config_version == CURRENT_CONFIG_VERSION` |

### Running migration tests

```bash
# Build and run all migration tests
make test-run

# Run only versioning/migration tests
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
[Config] Migration v1: disabled sounds_enabled for existing config
[Config] Migration v2: converted LED 'neopixel chamber_light' from /printer/leds/strip to /printer/leds/selected array
```

If a config file is corrupt (unparseable JSON), `init()` backs it up as `settings.json.corrupt` and creates a fresh default config.

---

## Config File Rename (v0.X.X)

The config file was renamed from helixconfig.json to `settings.json`. Migration
is automatic:
- C++ `Config::init()` renames the file on disk if the old name is found
- The installer checks for old names during backup/restore
- Rolling backups fall back to old names if new-named backups don't exist
- Template renamed from `helixconfig.json.template` to `settings.json.template`
