# Clog / flow meter consolidation

**Status:** planned, not started. Written 2026-08-13 at the end of the #1017 / #1126
session, for a fresh session to pick up.

**Branch it follows:** `feature/flowguard-2x1` — the FlowGuard bar, the grid snap-step
fixes, the `UiClogMeter` fill-mode removal and the mock scenarios below are all already
committed there. That work has since reached the trunk.

> Plans are point-in-time. Verify each predicate against the code before acting on it;
> the line numbers below predate the trunk swap and will have moved.

---

## Where things stand

Three visualizations exist. Two draw the **same** quantity, one draws a different one.

| Widget | File | Reads | Shows |
|--------|------|-------|-------|
| `UiClogBar` | `src/ui/ui_clog_bar.cpp` | `clog_meter_{mode,value,warning,danger_pct,peak_pct}` + 5 text subjects | reading, fault direction (end labels), danger threshold (shaded band), worst-so-far (peak tick), mode, status, headline number |
| `UiClogMeter` | `src/ui/ui_clog_meter.cpp` | `clog_meter_{mode,value,warning}` | reading as arc sweep + colour ramp, `clog_value_text`, `clog_mode_text` |
| `UiBufferMeter` | `src/ui/ui_buffer_meter.cpp` | `sync_feedback_bias` only | buffer plunger position — filament tension vs compression |

Placement today: bar on the home widget (carousel page 1), arc in the AMS sidebar and
the loaded-spool card, plunger meter in the Buffer Status modal and on the home
widget's carousel page 2.

The derivation that feeds all the `clog_meter_*` subjects is
`AmsState::sync_clog_meter_from_info()` (`src/printer/ams_state.cpp:2103`), called from
`sync_from_backend()` and from the two override setters. Source precedence is
**flowguard > encoder > AFC buffer**.

### Mock support (done)

`helix-screen ctl scenario <name>` drives the mock **backend**, not the subjects, so the
whole derivation runs. Subject-level scenarios would be overwritten on the next AMS
refresh — that is why driving the meter by hand needs `ctl freeze` first.

`clog_healthy`, `clog_warning`, `clog_blocked`, `flowguard_neutral`, `flowguard_tangle`,
`flowguard_clog`, `buffer_safe`, `buffer_fault`, `clog_off`.

Measured output of each, for regression comparison:

```
clog_healthy       mode=1 val=5   warn=0 peak=9  danger=59 [12mm|0]        center=11.8mm
clog_warning       mode=1 val=40  warn=1 peak=66 danger=59 [12mm|0]        center=7.5mm
clog_blocked       mode=1 val=94  warn=1 peak=95 danger=59 [12mm|0]        center=0.8mm
flowguard_neutral  mode=2 val=2   warn=0 peak=18 danger=80 [TANGLE|CLOG]   center=+2%
flowguard_tangle   mode=2 val=-55 warn=0 peak=62 danger=80 [TANGLE|CLOG]   center=-55%
flowguard_clog     mode=2 val=82  warn=1 peak=86 danger=80 [TANGLE|CLOG]   center=+82%
buffer_safe        mode=3 val=0   warn=0 peak=0  danger=75 [SAFE|FAULT]    center=
buffer_fault       mode=3 val=97  warn=1 peak=97 danger=75 [SAFE|FAULT]    center=2mm
clog_off           mode=0 val=0   warn=0 peak=0  danger=75 [|]             center=
```

---

## 1. Safe-state drift between arc and bar

**Bug, introduced by the bar. Smallest item, do it first.**

`UiClogMeter::update_safe_state()` treats `mode == 3 && value == 0` as "nothing to
report": it hides the arc and shows a check-circle icon. `UiClogBar` has no such
concept — it draws a zero-width fill, so the same state renders as an **empty track with
no number** (`ctl scenario buffer_safe` reproduces it; `center_text` is empty because
`ams_state.cpp` deliberately writes `""` for the safe case).

Fix: hoist the predicate into the shared header (see item 3 — it belongs on the model),
and give the bar the same affordance. A check-circle centred on the track, or the ends
dimmed with a check where the number goes; whichever, both renderers must read one
predicate.

**Watch for:** `mode == 3 && value == 0` is *not* the same as `mode == 0`. Mode 0 means
no detection hardware at all, and the whole widget hides itself via
`bind_flag_if_eq subject="clog_meter_mode" ref_value="0"` in the XML. Safe state means
hardware is present and armed with nothing to say.

## 2. The BUF modal shows no clog measurement

**IA bug. Independent of the others.**

Tapping the FlowGuard widget opens `BufferStatusModal`, which shows *less* clog
information than the tile that opened it. Its "Clog detection" row
(`buffer_status_modal.cpp:125-131`) prints the **mode string** — `"Automatic"` /
`"Manual"` / `"Off"`, or `"Active"` / `"Inactive"` on AFC — not a reading. The value,
the threshold and the peak are all absent; the only meter in the modal is the plunger,
which is a different sensor.

Fix: put a `UiClogBar` in the modal. `ui_xml/components/buffer_status_modal.xml` already
has a `meter_col` (100x150, gated by `buf_show_meter`) holding the plunger; the bar wants
width rather than height, so it likely belongs full-width above the two columns rather
than inside `meter_col`.

**Watch for:** the modal is reachable when `clog_meter_mode == 0` (it is the buffer
modal, not the clog modal). Gate the bar on mode, not on the modal being open.

## 3. Extract a shared model; arc and bar become renderers

**The consolidation proper. Do after 1 and 2, since both shrink under it.**

The duplication that matters is semantic, not textual:

- the safe-state predicate (item 1) — currently in one renderer only;
- symmetrical-vs-linear mode handling — the arc maps `-100..+100` onto `0..200` with
  `LV_ARC_MODE_SYMMETRICAL` (`ui_clog_meter.cpp` `on_mode_changed`/`on_value_changed`),
  the bar computes centre-out geometry in `clog_bar_geometry()`. One rule, two encodings;
- five observers on the same subjects, wired twice.

Shape: a `ClogMeterModel` owning the observers once and exposing a snapshot
(`mode, value, warning, danger_pct, peak_pct`) plus the derived state both renderers must
agree on — `is_safe()`, `is_symmetrical()`, normalized position — with a change callback.
`UiClogArc` and `UiClogBar` become thin renderers over it.
`include/clog_meter_geometry.h` is already the pure half of this (tint rule + bar
geometry, both unit-tested in `tests/unit/test_clog_meter_geometry.cpp`); it needs the
model half beside it.

Expect ~40 net lines saved. That is not the reason to do it — one place deciding what the
numbers mean is.

**Keep both presentations.** The sidebar column is ~100px wide and 150 tall, where an arc
genuinely fits better; the loaded card and the home tile are wide and short, where the bar
does. Neither is redundant, and this is not a step toward deleting the arc.

**Watch for:** `UiClogMeter` is constructed against an XML subtree it finds by name
(`clog_meter`, `clog_arc_container`, `clog_arc`) in two different parents. Whatever the
model looks like, the renderers keep that lookup contract.

## 4. AFC FPS_PSF proportional buffer

**New capability, not cleanup. Needs hardware to confirm.**

Verified in AFC-Klipper-Add-On v1.2.0 (`a06f14d`, local checkout at
`../AFC-Klipper-Add-On`): `extras/AFC_buffer.py` `load_config_prefix` accepts
`type = switched` (TurtleNeck two-switch) **or `type = FPS_PSF`** → `AFCFPSBuffer`, which
reads an analog filament pressure sensor over ADC and proportionally trims the lane
stepper's rotation distance.

`AFCFPSBuffer.get_status()` (`AFC_buffer.py:1565`) adds to the base buffer status:

- `fps_value` — raw reading, 3dp
- `smoothed_fps` — smoothed reading
- `set_point` — configured neutral, default 0.5

Tuning points: `low_point` (default 0.1, max tension / stretched), `set_point` (0.5,
neutral), `high_point` (0.9, max compression), `deadband` (0.30). Same quantity as Happy
Hare's `sync_feedback_bias`, expressed as 0..1 around a configurable neutral instead of
−1..+1.

Consequences for us:

- `UiBufferMeter`'s docstring ("Happy Hare only — AFC has no proportional sensor data")
  is **stale and wrong**.
- `IAmsBackend::supports_sync_feedback_visualization()` is under-inclusive: an AFC user
  with an FPS_PSF buffer has real proportional data we ignore, so the home widget never
  builds its page 2 and the modal hides its meter.

Mapping: normalize `(smoothed_fps - set_point)` against `(high_point - set_point)` on the
compression side and `(set_point - low_point)` on the tension side, to land on the
existing −1..+1 bias.

**Do not ship this on the strength of the source read alone.** Our comments about AFC —
and AFC's own — have been wrong before. Capture a live `get_status` dump from the
BoxTurtle rig (CB1/Voron+HDMI5, the only AFC hardware here) and confirm the field names,
the value range and whether `smoothed_fps` or `fps_value` is the one to display, before
trusting the normalization.

## 5. Danger band legibility when warning is set (minor)

When `warning == 1` the fill is drawn in `danger` and the danger band is also `danger` at
30% opacity, so "how far into the danger zone" is hardest to read exactly when it matters
most. Visible in `ctl scenario flowguard_clog`. Options: outline the band instead of
filling it, or shift the fill to a lighter tint when it overlaps. Cosmetic — bundle it
with item 3 rather than doing it alone.

---

## Verification recipe

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
    --remote-socket "$HELIX_SOCK" --printer happy_hare > /tmp/helix-$TREE.log 2>&1 &

# clog_detection is default_enabled=false — place it before driving states.
./build/bin/helix-screen ctl -s "$HELIX_SOCK" scenario flowguard_tangle
./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot /tmp/x.png --target clog_detection
```

`ctl geom clog_bar_fill` / `clog_bar_peak` / `clog_bar_danger_{lo,hi}` read the exact
placement — prefer them over eyeballing a screenshot.
