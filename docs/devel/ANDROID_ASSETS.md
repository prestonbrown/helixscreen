# Android Asset Staging

**`android/app/src/main/assets/` is a build output. Never edit it, never commit
into it, never write to it from a Makefile or script.**

The single source of truth is `ui_xml/`, `assets/` and `config/` at the repo
root. Editing those is sufficient and authoritative for every platform,
Android included.

## How assets reach the APK

`android/app/app/build.gradle` defines a `copyAssets` task that stages the three
source trees into the APK's asset directory:

```groovy
task copyAssets(type: Copy) {
    doFirst {
        stagedAssetDirs.each { it.deleteDir() }   // ui_xml, assets, config
    }
    from('../../ui_xml') { into 'ui_xml' }
    from('../../assets')  { into 'assets' }
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

## Why the tree still causes trouble

`android/.gitignore:5` ignores `app/src/main/assets/` wholesale, so the output of
your last local Android build sits in the working directory indefinitely — and to
anything walking the repo it looks exactly like source.

That has cost real time:

- A snapshot went four months stale: **248 of 287** common XML files differed, 65
  source files were missing from it, and 14 files it still held had been deleted
  from source. Whole directories had diverged (`portrait/` missing, `ultrawide/`
  long gone, `translations/` still the pre-split single file).
- Four separate lint gates each grew a hand-written exclusion for the path —
  `check_overlay_width.py`, `check_hardcoded_pixels.py`,
  `check_modal_chrome_budget.py`, `check_responsive_token_scope.py`. Four authors
  independently debugged the same false positives.
- `mk/filaments.mk` copied `assets/filaments.json` into the staged tree after
  every `make regen-filaments`, refreshing exactly one file out of 592 and making
  a dead directory look maintained. Removed.
- `RELEASE_1_0_CHECKLIST.md` told developers the Android tree "carries its own
  copy", i.e. mirror your edit there. It was followed at least once; the edit was
  erased by the next build. Fixed.

## The gate

`scripts/check_android_asset_staging.py` runs from `scripts/quality-checks.sh`
(so both the pre-commit hook and CI's `quality.yml`). It **fails** on:

- any tracked file under `android/app/src/main/assets/`;
- any `Makefile` / `mk/*.mk` / `scripts/*` line that *writes* into that path —
  `cp`, `mv`, `install`, `rsync`, `ln`, `mkdir`, `tee`, or a `>`/`>>` redirect.

Naming the path in order to *exclude* it (what the four lint gates do) and
reading from it are both fine — only writes are flagged. A genuinely necessary
write can be annotated `# ANDROID_STAGING_OK: <reason>`; there are none today.

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
