# Release 1.0 Checklist

Everything that must happen before `v1.0.0` ships, and before the 1.1 devel track
opens alongside it. Delete this file once 1.0 is out and 1.1 is publishing.

Background on the two-track mechanism: `UPDATE_SYSTEM.md` § "How CI Determines
Upload Channels" and § "Switching Channels (and moving backward)".

---

## 1. The atomic branch cut

**These two edits must land in the same change.** They are the only ordering
constraint in this document that can strand a fleet.

- [ ] Cut `release/1.0` from the 1.0 commit. It keeps `RELEASE_CHANNEL=stable`.
- [ ] Flip `RELEASE_CHANNEL` on `main` to `beta` **in that same change**.

Why atomic: `main` currently says `stable` because it is still the only release
line. Flip it early and the stable fleet gets no further updates. Flip it late —
i.e. tag anything from `main` after 1.1 work starts — and that tag publishes to
`stable`, overwriting the 1.0 manifest for every user.

The pre-upload downgrade guard in `release.yml` catches the *second* half of that
mistake (it refuses to move a channel manifest backward), but not the first. It
also cannot help if 1.1.0 > 1.0.0, which is exactly the dangerous case: publishing
1.1.0 to `stable` is a *forward* move and sails straight through.

### Measured state, 2026-08-15

Checked because "tell XXLarge users to run the bleeding-edge channel" was proposed
as the answer to a layout gap (see below) and turned out not to work yet.

| Branch | `RELEASE_CHANNEL` | Position |
|--------|-------------------|----------|
| `main` | `stable` | the only live release line |
| `release/1.0` | `stable` | exists, but **99 commits behind `main`, 0 ahead** |
| `devel/1.1` | ~~`stable`~~ → `dev` | fixed 2026-08-15, see below |

Three things follow.

**Nothing feeds `beta` or `dev` yet.** dl.helixscreen.org/dev/manifest.json serves
v0.99.111 — an ordinary `main` build. Until step 2 above happens, telling a user to
switch to Dev gets them newer `main`, not the devel track. Any plan that routes
users to bleeding edge to pick up 1.1 work is blocked on the flip, not on the work.

**`release/1.0` is a placeholder, not a maintenance line.** Being 99 behind and 0
ahead means the cut in step 1 has not really happened; the branch name exists but
carries none of the 1.0 content. Do not read its presence as step 1 being done.

**`devel/1.1` declared `stable`, which was live-fire.** Tagging that branch would
have published the whole 1.1 line — including the grid rewrite — to every stable
install, and the downgrade guard would not have blocked it because 1.1.0 > 1.0.0 is
a forward move. Set to `dev` on 2026-08-15, ahead of the atomic cut, because it is
safe in isolation: it only removes a destination, and `devel/1.1` was never meant
to reach `stable`. It does **not** substitute for step 2 — `main` still says
`stable`, so the ordering constraint above is untouched.

### Deferred to 1.1: the XXLarge layout gap

Not a 1.0 blocker, recorded here so the deferral is a decision rather than an
oversight. `GRID_DIMS` on `main` stops at XLarge (`NUM_BREAKPOINTS = 6`) and
`clamp_bp()` folds tier 6 back to 5, while every anchor in `default_layout.json`
stops at `xlarge` — so a 1920x1080 display renders the XLarge 8x5 grid and XLarge
anchor positions at 2.25x the pixels. `devel/1.1` already fixes it end to end
(`NUM_BREAKPOINTS = 7`, a per-tier `GRID_CELL` ladder, content-derived
`get_dimensions()`, and `xxlarge` placements on all 10 anchors), so fixing it on
`main` would rewrite exactly the files 1.1 rewrote and be discarded at merge.

Scale, from telemetry over 1,779 distinct devices: XXLarge is 15 devices (0.8%),
and **every one is a Pi or x86 box** — 2.3% of Pi installs, 0% on every integrated
printer panel, which are all 800x480 or smaller. Pi XXLarge resolutions are
2560x1440 (8) and 1920x1080 (4), i.e. HDMI monitors. Note this population will
invert on Android: `min_dim > 1000` catches essentially every modern phone and
tablet, so XXLarge goes from rare to typical the moment the Play Store listing goes
live. See `ANDROID_PLAY_STORE.md`.

---

## 2. Before tagging `v1.0.0`

- [x] Close the open 1.0-milestone issues. **All closed; the 1.0 milestone is
      0 open / 18 closed as of 2026-08-15.** (#1272 print stats, #1262 sensor
      registry, #1260 orphaned printer presets.)
- [x] Green CI on `main`. **Green as of 2026-08-15.** `Build`, `esp32-build`,
      `XML Lint`, and `Code Quality` all passed on `ee9abcb95` (2026-08-14
      21:46), and the Nightly Full Test Suite passed 2026-08-15 04:28 -
      including the `test_recovery_dialog_threading.cpp` SIGSEGV that had been
      unreproduced. Watch whether it recurs rather than treating it as fixed.
- [ ] `VERSION.txt`: `0.99.113` → `1.0.0`. Every existing install is on `0.99.x`,
      so this is an ordinary forward step for the updater — no special handling.
- [x] Confirm the `ALLOW_CHANNEL_DOWNGRADE` repository variable is **unset**. It
      is the escape hatch for the downgrade guard and must be off by default.
      *Verified 2026-08-14 (`gh variable list`): not set.*
- [x] **tar.gz Phase 2 — DEFERRED, not in 1.0.** Decided 2026-08-14 on fresh
      telemetry (549 actives, 30d).

      The pre-v0.99.31 count this item was written around is still tiny (3–5 of
      549, ~0.5–0.9%, none meaningfully self-updating) — but it was never the
      real gate. `scripts/generate-manifest.sh:36` sets
      `ZIP_EXCLUDE_PLATFORMS="ad5m ad5x cc1 k1 k2 snapmaker-u1"`, six platforms
      deliberately served tar.gz as their **only** manifest asset because
      pre-v0.99.102 updaters verify with `unzip -tqq` and BusyBox lacks `unzip -t`
      before 1.32 (K1 ships 1.31.1, AD5M 1.29.3, K2's OpenWrt has none — #993).
      **Those six platforms are 344 of 549 actives, 62.7% of the fleet.** Dropping
      tar.gz production today strands the majority, not the stragglers.

      v0.99.102 (which fixes the verifier) shipped 2026-07-26; the 7-day view has
      those fleets at 80–100% on 102+, but the 30-day view is 40–50%, and the
      long-tail device that boots monthly is exactly the one that would brick.

      **Decision rule for later: Phase 2 unblocks when `ZIP_EXCLUDE_PLATFORMS` is
      empty**, not when the pre-v0.99.31 count reaches zero. Retire platforms from
      that list one at a time as each fleet clears v0.99.102. Realistically 1.1+.

      Caveat on all of the above: telemetry is opt-in and default OFF, so 549 is a
      self-selected floor. The bias runs the wrong way — a user who disables
      telemetry is plausibly the same user who does not update.

---

## 3. First release on each track

The two-track routing has never run end-to-end against real R2. Verify both.

- [ ] First `stable` tag from `release/1.0`: confirm stable/manifest.json serves
      `1.0.0`, and that `notify-website` fired (docs deploy is gated on
      `channel == 'stable'` now, not on the tag lacking a hyphen).
- [ ] First `beta` tag from `main`: confirm beta/manifest.json **and**
      dev/manifest.json both move, that the GitHub release is marked
      prerelease, and that `notify-website` did **not** fire.

      **beta/manifest.json does not exist yet and is actively 404ing.** Zone
      analytics for 2026-08-08..15 show **150 failed polls, ~19/day**, against
      /beta/manifest.json - consistent with the 30 Beta-channel installs in
      the telemetry split below. Those devices have been failing every update
      check silently for as long as the object has been missing. The tag from
      `main` creates it and fixes them; nothing else is needed. If the cut
      slips, publishing a beta manifest pointing at the current stable release
      would un-strand them in the meantime.

      Verify after the cut that the error count for `beta` in the dashboard's
      CDN fleet metric drops to zero (`TELEMETRY_ADMIN.md` § "Fleet size").
- [ ] Confirm an app on the Beta channel is offered the devel build, and an app on
      Stable is not.
- [ ] Sanity-check the GitHub API fallback paths once R2 has both channels
      populated: `stable` uses `/releases/latest` (excludes prereleases —
      correct), `beta` scans for the first prerelease.

**Known behaviour change — checked, nobody is affected.** `stable` no longer
publishes to the `dev` channel, so anyone pinned to Dev while tracking the stable
line would stop receiving updates. Telemetry 2026-08-14 (`auto_update_channel`
from raw `settings_snapshot` events, 483 of 484 actives reporting):
**Stable 453 (93.8%), Beta 30 (6.2%), Dev 0.** Re-confirmed on a 3-day August
sample: Stable 34, Beta 2, Dev 0. Nobody is on Dev — consistent with the dropdown
being a 7-tap easter egg. No action needed.

---

## 4. Before unhiding the channel dropdown

Tracked as #1236 ("Beta: Update Channel dropdown — finish or drop", 1.1 milestone).
Do this *after* both tracks are confirmed publishing.

- [ ] Remove the `show_beta_features` gate on `container_update_channel` in
      `about_settings_overlay.xml` (currently a 7-tap easter egg on the version row).
      Edit `ui_xml/` only — `android/app/src/main/assets/ui_xml/` is a Gradle build
      output that `copyAssets` wipes and re-copies on every build, so an edit there
      is erased. See `docs/devel/ANDROID_ASSETS.md`.
- [ ] Rename the options for a two-track UX: **Stable / Devel**, keeping **Dev**
      behind the beta gate. Dev is still rejected outright without
      `/update/dev_url`, which is fine for a hidden developer option and wrong for
      a user-facing one.
- [ ] Translate the three downgrade strings — currently English placeholders in all
      8 non-English locales: `"Switch to v%s"`, `"Install Older Version?"`, and the
      confirmation body `"This channel offers v{}, older than the installed v{}…"`.
      Consult `translations/GLOSSARY.md` per locale.

---

## 5. Verified, and not

**Verified end-to-end** (headless mock against a local dev manifest serving 0.5.0
while running 0.99.111):

- `Channel is behind: 0.99.111 -> 0.5.0 (downgrade offered)` — the `Older` branch fires
- `Auto-check: 0.5.0 is a downgrade, not notifying` — no unprompted notification
- About row subject reads `Switch to v0.5.0`, not "available"
- Confirmation modal: *"Install Older Version?"* / *"This channel offers v0.5.0,
  older than the installed v0.99.111…"* → confirming reaches the download modal

**Not verified:**

- [ ] The downgrade path on a **real device**, not desktop mock. In particular the
      install actually completing and the older binary coming up on its own config.
- [x] **A full devel → stable → devel config round trip — verified 2026-08-14, safe
      for the reachable range.** `tests/unit/test_config_migration_future.cpp` now
      carries 10 round-trip cases (tag `[config][migration][roundtrip]`) driving a
      populated config — two printers, macros, LED auto-state maps, filament slot
      overrides, widget layout, material presets, a captured touch affine. Rollback
      to config_version 18/19/20 and back is **byte-identical on the whole
      document**, and a sweep of 43 untargeted settings survives every rollback
      depth. Mutation-verified.

      **Five migrations are NOT idempotent**, and are pinned as current behavior
      rather than fixed: `config.cpp:343` (jitter 15→5, fires below v3), `:446` and
      `:488` (brightness 50→80, below v7/v9), `:457` (toolhead_style 2→5/3→2, a
      rotation — below v8), `:812` (writes `recheck_pending` unconditionally, below
      v18; the flag can invalidate a captured touch calibration at boot via
      `should_invalidate_legacy_calibration`).

      **Why this is accepted, not a blocker:** every one of them requires rolling
      the stamp below config_version 18, i.e. below v0.99.80 (2026-06-18). The
      in-app updater only ever offers what a channel's manifest serves — after the
      cut that is 1.0.0 on stable and 1.1.x on beta — so reaching that range means
      hand-installing a 2026-06 build. Not a path the product exposes. The
      forward-compat guard (v0.99.112, `7e3d6f05d`) additionally stops a newer
      config being stamped down at all, and both 1.0 and 1.1 carry it.

      If a migration below v18 ever becomes reachable again, `:812` and `:457` are
      the two to fix first — `:457` is a rotation and cannot be made idempotent
      without a marker.

