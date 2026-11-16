# Android Asset Staging

**`android/app/src/main/assets/` is a build output. Never edit it, never commit
into it, never write to it from a Makefile or script.**

The single source of truth is `ui_xml/`, `assets/` and `config/` at the repo
root. Editing those is sufficient and authoritative for every platform,
Android included.

## How assets reach the APK

`android/app/build.gradle` defines a `copyAssets` task that stages the three
source trees into the APK's asset directory:

```groovy
task copyAssets(type: Copy) {
    doFirst {
        stagedAssetDirs.each { it.deleteDir() }
    }
    from('../../ui_xml') { into 'ui_xml' }
    from('../../assets') {
        into 'assets'
        exclude mockOnlyAssets + mockOnlyAssets.collect { "${it}/**" }
        exclude compiledFontSources
    }
    from('../../config') {
        into 'config'
        exclude nonShippableConfig
        exclude hostOnlyConfig
    }
    into 'src/main/assets'
}
```

The `deleteDir()` matters: Gradle's `Copy` is purely additive and never removes a
file an earlier build left behind, so without the wipe an exclusion added later
would have no effect on a destination that already holds the file. Because the
trees are deleted and rebuilt every time, **the APK can never ship stale
content**, no matter what state the on-disk copy is in.

Three companion tasks round it out:

| Task | What it does |
|------|--------------|
| `purgeStagedLocalState` | Sweeps leaked local state (`settings.json`, `.crash_restart_count`, …) out of staged `config/`. Separate from `copyAssets` because overlapping outputs make Gradle report a spurious `UP-TO-DATE`. |
| `genPackagingManifest` | Writes `MANIFEST.txt` at the assets root. |
| `genBuildStamp` | Writes `BUILD_STAMP` every build (`upToDateWhen { false }`) so `android_asset_extractor.cpp` re-extracts on device. |

### What stays out of the APK

Each exclusion list is defined once near the top of `build.gradle`:

| List | Leaves out | Why |
|------|-----------|-----|
| `nonShippableConfig` | Local state that a dev machine leaves in `config/` (`settings.json`, crash files, telemetry files, locks, `*.corrupt`) | It is one developer's state, not package content |
| `hostOnlyConfig` | systemd, polkit, udev and K2 procd plumbing | Android runs none of it |
| `mockOnlyAssets` | `assets/test_gcodes`, `assets/test_timelapse` | Only the mock printer reads them, and a release APK compiles no mock. The Linux packages leave out the same two through `RELEASE_ASSETS_EXCLUDE` in `mk/cross.mk` |
| `compiledFontSources` | `assets/fonts/*.c` | They are linked into `libmain.so` through `FONT_SOURCES` in `android/app/jni/CMakeLists.txt`; at runtime the app reads only the binary faces under `fonts/cjk/` |

`genPackagingManifest` drops the `mockOnlyAssets` directories from `MANIFEST.txt` as well,
because the extractor counts a manifest directory it cannot open as a failed extraction.
`tests/shell/test_android_feature_gates.bats` keeps `mockOnlyAssets` equal to
`RELEASE_ASSETS_EXCLUDE` and pins `compiledFontSources` to the CMake glob.

## On the device

`android_extract_assets_if_needed()` (`src/application/android_asset_extractor.cpp`) copies
the APK's assets into app storage when `BUILD_STAMP` changes, then sets `HELIX_DATA_DIR` to
the result. Everything lives under the app's files dir (`Context.getFilesDir()`, which an
app update leaves in place), laid out by `android_storage_layout()`
(`include/android_asset_extractor.h#AndroidStorageLayout`):

| Path under `files/` | Holds |
|---------------------|-------|
| `data/` | The extracted package content and the live `config/`. This is `HELIX_DATA_DIR` |
| `state/` | The primary rolling settings backups, and the app's state dir (where the update URL override file is looked for) |
| `.helixscreen/` | The fallback settings backups |

`android_configure_backup_dirs()` points the two backup tiers there before `Config::init()`
runs, because Android has neither `/var/lib/helixscreen` nor a `$HOME`. Both sit outside
`data/`, so nothing an extraction writes or removes can reach them.

**`data/config/` is seed-only.** It is the writable layer `find_readable()` consults ahead of
the shipped seeds under `assets/config/`, so a file there belongs to the user: a theme saved
from the editor, a `printer_database.d` override. The extractor creates a `config/` file that
is missing and never rewrites one that exists
(`src/application/android_asset_extractor.cpp#asset_dir_is_user_layer`). An install keeps
the first copy of a `config/` file it ever received, so package content that has to follow
releases belongs under `assets/config/`, which every extraction rewrites.

A re-extraction also deletes what an older APK left behind and the current one no longer
ships: the shipped seeds that once lived under `config/` and now live under `assets/config/`
(they would shadow the new ones), the `mockOnlyAssets` trees and `fonts/*.c`. Nothing else ever removes an extracted file.

### Auto Backup

`AndroidManifest.xml` sets `allowBackup` with two rule files that must stay identical:
`res/xml/backup_rules.xml` (`fullBackupContent`, Android 11 and lower) and
`res/xml/data_extraction_rules.xml` (`dataExtractionRules`, Android 12 and higher, with the
same lines under both `cloud-backup` and `device-transfer`). They include only
`data/config/` and `state/`, the user's settings layer and its backups. Everything else
under `files/` is package content the APK puts back on its own, and would blow the 25 MB
cloud quota several times over.

Inside `data/config/` the rules exclude what belongs to the device rather than the user:
the telemetry identity, queue and snapshot, crash reports and the crash-restart counter, the
instance lock, `safe_mode.flag`, the GPU guards, the filament slot override cache,
the update success flag, and the `settings.json.corrupt` / `settings-test.json.corrupt` copies
`FileConfigStorage::preserve_corrupt()` sets aside. A new device-local file written into the
config dir needs an exclude line in both files. `tests/unit/test_android_asset_extractor.cpp`
(`[android][asset][backup]`) checks both files against `android_storage_layout()` and
against each other.

## Why the tree needs a gate

`android/.gitignore` ignores `app/src/main/assets/` wholesale, so the output of your last local Android build sits in the working directory indefinitely, and to anything walking the repo it looks exactly like source. A stale copy misleads greps, trips lint gates with false positives, and invites edits that the next build erases. Lint gates that walk the repo exclude the path; nothing may write into it.

## The gate

`scripts/check_android_asset_staging.py` runs from `scripts/quality-checks.sh`
(`scripts/qc/design_pixels.sh`, so both the pre-commit hook and CI's `quality.yml`). It **fails** on:

- any tracked file under `android/app/src/main/assets/`;
- any `Makefile` / `mk/*.mk` / `scripts/*` line that *writes* into that path —
  `cp`, `mv`, `install`, `rsync`, `ln`, `mkdir`, `tee`, or a `>`/`>>` redirect.

Naming the path in order to *exclude* it (what the four lint gates do) and
reading from it are both fine — only writes are flagged. A genuinely necessary
write can be annotated `# ANDROID_STAGING_OK: <reason>`.

It **warns without failing** when the staged tree is older than the newest file
in `ui_xml/`, `assets/` or `config/`. That is the normal state right after any
source edit and means only "your last Gradle build predates your last edit" — it
exists so a stale tree is attributable when someone greps it by accident, not so
anyone has to act on it.

Meta-tests: `tests/shell/test_android_asset_staging_gate.bats`, run by the
`test-shell` job in `.github/workflows/build.yml`.

## If you find a stale tree

Delete it. `rm -rf android/app/src/main/assets` — the next Gradle build
reconstructs it from source. Nothing under it is unique; it is all copies of
files that still live in `ui_xml/`, `assets/` and `config/`.
