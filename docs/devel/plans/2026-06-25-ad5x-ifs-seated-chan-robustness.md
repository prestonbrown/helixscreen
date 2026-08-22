# AD5X native ZMOD IFS: `Chan` seated-authority robustness (v2)

> ⚠️ **Historical record (verified 2026-08-09) - not instructions. Status: PARTIALLY SHIPPED.**
> The "## Status" section at the bottom still says "Implementation not started"; that is stale.
>
> - **Fix 1 shipped, but by a different mechanism than prescribed.** There is no
>   `pending_cold_op_slot_`. What shipped is a head-gate on the *switch* sensor plus
>   `clear_seated_if_ejected_locked()`. **Do not implement the `head_filament_` gate this plan
>   prescribes** - see the correction under Fix 1.
> - **Fix 2 shipped.** `persisted_seated_slot_` + `persist_seated_slot_locked()` +
>   `seated_resolved_since_boot_` in `src/printer/ams_backend_ad5x_ifs.cpp`, persisted to the
>   Moonraker `lane_data` DB rather than `SettingsManager`.
> - **Fix 3 shipped.** `PURGING_TIMEOUT_SECONDS = 240` (`include/ams_backend_ad5x_ifs.h`),
>   selected in `check_action_timeout()`, and `action_start_time_` reset on
>   `ifs_motion_sensor` activity during `PURGING`.
> - **Fix 4 did NOT ship.** There is no single "Unload current filament" affordance. Fix 2
>   made the common case moot, but the residual still exists: head loaded, `Chan == 0`, and
>   *nothing persisted* leaves `slot_unloads_to_toolhead()` true for every slot.
>
> Verify predicates and line numbers against current code before acting on anything here.

Follow-up to the 2026-06-15 AD5X IFS status seated-slot plan, which introduced `IFS_STATUS`
`Chan` as the seated-channel authority (shipped as Bug-1 fix `4520e1e23`, in v0.99.84).
That plan's line 76–79 flagged "field confirmation REQUIRED": run `IFS_STATUS` loaded+idle and
after unload. **This is that confirmation.** It verifies Bug 1 is fixed and falsifies the
plan's core assumption about `Chan` lifetime.

Source data: GitHub #1065 follow-up — debug bundle **CGR6C7PA** (v0.99.84, firmware
`v0.13.0-698-g01ee2767-ZMOD-20260526`, `user_note=raza616`) + a hand-built state table
(`AD5X status.xlsx`) running `IFS_STATUS`/`COLOR`/menu actions across power cycles.
Reporters: mkleersn (posted) + raza616 (bundle). Related: #995, #981, #996.

## What the data proved about `IFS_STATUS Chan`

The 2026-06-15 plan assumed (line 77): *"Chan persists at the loaded port while idle and
goes 0 after unload."* Both halves are wrong on this firmware:

| Observation | xlsx / log evidence | Implication |
|---|---|---|
| **Chan = 0 after power cycle**, even with a lane physically at the head | xlsx "Power cycled … channel 2 loaded" → `Chan: 0`; startup log `IFS_STATUS trigger=startup Chan=0` with `head_filament_=true` | The seated lane is **unrecoverable from firmware** after a power cycle. Cold-boot floor is real. |
| **Chan does NOT zero after unload** — it holds the channel the removal routine engaged | log: user unloads → `IFS_STATUS trigger=toolhead_unload Chan=2` *after* head sensor already dropped; HelixScreen then sets `current_slot: -1 → 1` | `Chan` is **last-engaged, not currently-seated**. It is sticky and gets polluted by unload/eject of any lane. |
| **Chan polluted by cold-lane eject** | xlsx: load ch2 (menu correct) → eject ch1 → loaded ch2 now offers **Eject** | Eject of a cold lane rewrites `seated_chan_` to that cold lane → misroutes the truly-seated lane. (Bug 3) |
| **Chan correct for clean in-session load** | xlsx: "Loaded channel 2 with COLOR macro … Channel 2 shows loaded, can be unloaded, all others eject" | **Bug 1 fix verified.** `Chan` is trustworthy *only* immediately after a clean load that the head sensor corroborates. |
| `Ports[]` tracks presence reliably | xlsx: port 2 → false on unload, true on insert | Use `Ports[]` for presence/grey state, never `Chan`. |
| `GET_ZCOLOR SILENT=1` clean, no dialog | xlsx header | Secondary Q answered. |

## Root causes (grounded in code)

`SlotStatus` enum: `UNKNOWN=0, EMPTY=1, AVAILABLE=2, LOADED=3` (`include/ams_types.h:155`).
The cold-boot status path is **fine** — with `Chan=0`, `current_slot=-1`, so
`update_slot_from_state` (`src/printer/ams_backend_ad5x_ifs.cpp:601-648`) marks populated
ports `AVAILABLE`, none `LOADED`. The bugs are in seated derivation and eject/unload routing.

### RC1 — `seated_chan_` set unconditionally from every `IFS_STATUS`
`src/printer/ams_backend_ad5x_ifs.cpp:3013-3023`: `seated_chan_ = chan;` on every parse.
Because firmware `Chan` = last-engaged channel, an **eject/unload of a cold lane** (whose
removal routine engages that lane) overwrites `seated_chan_` with the wrong lane.
`recompute_current_slot_locked` (:3822-3843) then sets `current_slot = seated_chan_-1` on the
native path. → **Bug 3.**

### RC2 — `slot_unloads_to_toolhead` falls through to Unload when seated lane unknown
`src/printer/ams_backend_ad5x_ifs.cpp:1173-1197`. After power cycle: `current_slot=-1`,
`seated_slot=-1`, `head_loaded=true`. Guard 1 needs `current_slot>=0` (no), guard 2 needs
`seated_slot>=0` (no), guard 3 is `!head_loaded` (no) → `return true` for **every** slot →
every lane offers Unload. → **"all lanes offer Unload after power cycle."**

### RC3 — PURGING shares the 90 s generic budget
`include/ams_backend_ad5x_ifs.h:650-651`: `ACTION_TIMEOUT_SECONDS=90`,
`HEATING_TIMEOUT_SECONDS=300`. `check_action_timeout` (`...ifs.cpp:3851-3885`) gives only
HEATING the long budget; PURGING gets 90 s. A real purge is ~180 s and status polls starve
behind serial gcode → false ERROR. xlsx "stuck at 225/230 while really 230" is the same
starvation. → **Bug 2.**

## Fix plan

### Fix 1 — Stop cold-lane eject/unload from polluting `seated_chan_` (RC1, Bug 3) — highest value
Only accept a `Chan` update as new seated truth when it is **corroborated** or **not
self-induced**:
- Track the slot of any in-flight eject/unload (`pending_cold_op_slot_`). While that op is
  active and for a short settle window after, ignore an `IFS_STATUS Chan` that resolves to
  that slot (the removal routine engaged it; it is not seated).
- Prefer to update `seated_chan_` only when `head_filament_` is true (something genuinely at
  the head). When `head_filament_` is false, a non-zero `Chan` is stale — clear seated to
  `-1` (or leave persisted value, see Fix 2) rather than trust it.
- Keep the clean-load path intact (load completes + head true → `Chan` is the seated lane).

> ⚠️ **Correction (2026-08-09) - the second bullet is wrong and would clear a genuinely
> seated lane.** `head_filament_` is written by **two** branches of
> `handle_status_update()` (`src/printer/ams_backend_ad5x_ifs.cpp`): the motion sensor
> (`filament_motion_sensor` / `zmod_ifs_motion_sensor ifs_motion_sensor`) and the switch
> sensor (`filament_switch_sensor` / `zmod_ifs_switch_sensor head_switch_sensor`), both via
> `parse_head_sensor()`. The **motion** sensor reports false whenever filament is not
> *moving*, which is the normal state of a lane that is loaded and idle. So
> `head_filament_ == false` is not evidence of an empty head, and a rule that clears
> `seated_chan_` on it drops the seated lane during ordinary idle polling.
>
> What shipped instead is a latch of the switch sensor's own authority, kept separate from the
> conflated `head_filament_`:
>
> ```cpp
> const bool head_empty_authoritative = head_switch_seen_ && !head_switch_present_;
> ```
>
> Only that rejects a sticky `FFMInfo.channel` / `Chan`. On motion-only firmware that
> publishes no switch, `head_switch_seen_` stays false and the gate never fires, so those
> printers keep the old adopt-the-channel behaviour rather than losing the seated lane. See
> the `head_filament_` / `head_switch_seen_` / `head_switch_present_` member comments in
> `include/ams_backend_ad5x_ifs.h`, and the uses in
> `AmsBackendAd5xIfs::apply_zcolor_result()` and `log_seated_state_locked()`.
>
> The self-pollution half of Fix 1 shipped as `clear_seated_if_ejected_locked()` (zero the
> seated pointer and `FFMInfo.channel` when they name the just-ejected lane), not as an
> in-flight `pending_cold_op_slot_` window.

### Fix 2 — Persist last-known seated slot across restart (RC2 + cold-boot floor)
- On a confirmed clean load (head true + `Chan>0`), persist `{seated_slot, material, color}`
  via `SettingsManager` (per-printer).
- On cold boot when `head_filament_==true && Chan==0`, restore the persisted slot as a
  **provisional** seated slot so `slot_unloads_to_toolhead` routes correctly (Unload on the
  one seated lane, Eject on the rest). Mark it provisional; a later confirmed `Chan` (or a
  user load/unload) supersedes it. If nothing is persisted, fall back to today's behavior but
  prefer a single "Unload current" affordance over Unload-on-every-lane (see Fix 4).

### Fix 3 — Dedicated PURGING budget + activity-based reset (RC3, Bug 2)
- Add `PURGING_TIMEOUT_SECONDS` (~240 s) and select it in `check_action_timeout` for
  `AmsAction::PURGING`.
- Reset `action_start_time_` on `ifs_motion_sensor` activity during PURGING (filament still
  moving = not stuck), so a long-but-healthy purge never trips the timeout.
- Treat a starved/timed-out status poll during PURGING as "keep waiting," not ERROR.

### Fix 4 — Unknown-seated UX (depends on Fix 2 outcome)
When head is loaded but the lane is genuinely unknown (no persisted slot, `Chan=0`), present a
single "Unload current filament" action rather than offering Unload on every lane. Avoids the
"unload ch1 actually unloads ch2" footgun seen in the xlsx.

## Tests (`tests/unit/test_ams_backend_ad5x_ifs.cpp`)
- **Bug 3**: load ch2 (`Chan=2`, head true) → `slot_unloads_to_toolhead(1)==true`,
  others false. Then eject ch1 emitting `IFS_STATUS Chan=1` while head still true →
  `slot_unloads_to_toolhead(1)` MUST stay `true` (ch2 still seated), eject of ch1 must not
  flip ch2 to Eject.
- **Power-cycle floor**: `Chan=0 + head_filament_=true` with a persisted seated slot=1 →
  `current_slot==1`, Unload only on slot 1, Eject on the rest.
- **Power-cycle, no persisted slot**: `Chan=0 + head true` → does NOT offer Unload on every
  lane (Fix 4 single-affordance or all-eject, per chosen UX).
- **Cold-lane self-pollution**: an in-flight eject on slot 0 emitting `Chan=1` must not set
  `seated_chan_=1` while head is loaded with another lane.
- **PURGING budget**: action stays non-ERROR up to the new budget; `ifs_motion_sensor`
  activity resets the clock.

## Field timing data for Fix 3 (#1065 follow-up)
- **raza616 (2026-06-25):** from a cold start (~24 °C ambient, worst case) the *whole*
  cut → unload loaded channel → load new channel → purge took **almost exactly 3 minutes**.
  This is the full multi-phase op (includes the cold HEATING phase, already budgeted 300 s);
  it does NOT isolate the PURGING phase.
- **Vger1700 (earlier):** hit the 90 s PURGING ERROR twice before a purge finished → the
  purge phase alone may approach ~180 s, OR poll starvation hides completion.
- **Implication:** reports agree the op is long but disagree on whether PURGING *alone* is.
  Since each phase's clock resets on transition (except HEATING), the robust fix is both
  (a) a dedicated PURGING budget (size ≥180 s) AND (b) reset the clock on `ifs_motion_sensor`
  activity, so the exact phase duration matters less; treat a starved poll as "keep waiting."

## Field confirmation still needed
- Power-cycle Unload→Eject after Fix 1/2: re-run the xlsx sequence (load X, power cycle,
  eject Y, re-check X) → X should still offer Unload.
- PURGING-phase duration in isolation (motion-sensor-active window) to confirm the dedicated
  budget — raza616's 3 min is whole-op, not purge-only.

## Status
Confirmed/diagnosed from CGR6C7PA + xlsx. MAJOR / critical-path (AMS) - worktree + test-first.

**Updated 2026-08-09:** "Implementation not started" was true when written and is no longer.
Fixes 1 (by a different mechanism), 2 and 3 are in `main`; Fix 4 is not. See the banner at the
top of this file for the per-fix evidence.
