# Multi-filament unit art and live trays (#2068)

## Intent

HelixScreen identifies filament systems and dryers with flat logos and colour bars. Those do not read as hardware. We want accurate, consistent drawings of each real unit wherever the app talks about one, and a repeatable way of producing them so unit number 15 matches unit number 1.

Issue #2068 scopes this to the dryer page. The real scope is every place a multi-filament unit or dryer appears.

The visual target is the two Bambu "Drying and Humidity" screens Preston supplied: small pictograms in a selector row, and a rich shaded drawing of the unit beside the controls. These are a target for quality and information layout, not something to copy.

## Goals (Preston's)

- Pictographically accurate units on the AMS overview (replacing or joining the per-lane colour bars), on configuration screens, and on the drying screens.
- As many units as we can draw.
- Live filament colour and overlays on the art.
- Decide logo replacement versus logo plus art by going through the screens systematically, not by blanket rule.
- A repeatable skill, built first, so everything is consistent.
- A live tray that adapts to N lanes and uses the real live spools, accepting it is not a perfect representation because it spreads the unit out horizontally and changes the angle for information room.
- Simpler rendering on boards that cannot afford the full version.
- Toolheads redrawn to be consistent and accurate, using the same approach.

## Non-goals and anti-goals

- Not invented hardware detail. A unit is drawn from real photos and proportions. Looking plausible but wrong is a failure.
- Not per-build variation for DIY systems. Each DIY system gets one representative drawing (my call).
- Toolheads are not in the first milestone; they follow once the kit is proven (my call, Preston agreed in discussion of the playback).
- No manufacturer wordmarks in the art (my call).
- No regression on the 800x480 and smallest layouts: width is the scarce resource, and caps and lanes must be compact.

## Approved direction

Agreed from the mockups in `docs/superpowers/iso-art-spike/` (throwaway; `sheet.png` and `tray5.png` are the reference images, `ace.py` and `tray.py` drew them). Preston: "you've pretty much nailed it."

**One unit description, two outputs.**

| | Art view | Live tray view |
|---|---|---|
| Used for | Dryer page, config screens, selector thumbnails | AMS detail, overview where it fits |
| Camera | Three-quarter: yaw about 27 degrees, pitch about 20 degrees, fixed for all units | The existing tray camera, `include/ams_tray_projection.h` |
| Output | Generated SVG baked to images at build time | Drawn at runtime for N lanes |
| Spools | Baked with a tintable filament region | The real `spool_canvas` widgets |

True isometric was tried and rejected: near flanges hide most of the filament, which carries the live colour.

**Generated vector art, from a shared kit in code.** A unit is a short definition using shared projection, lighting, materials and parts. Not AI image generation (invents detail, inconsistent) and not one C++ renderer per unit (today's toolheads: about 2,900 lines for 8). The `MrBongoC/ai-iso-skill` look was rejected; its face-projection idea is the same one the kit uses.

**Spools always follow the app's style.** Flange colours, gradient, six stadium vents, vent lip, air-hole lattice, and hub come from the current `spool_canvas` and `spool_vents` values, which are the same on main and `release/1.1`. Air holes on full-size drawings only; thumbnails stay plain. The kit reads or mirrors those values so art cannot drift from the live widget.

**Tray layout: left cap, N lane segments, right cap.**
- A lane segment holds that lane's rail section, tick, tab and badge. The spool is a separate live layer.
- The lid spans the row and is stretched, never tiled.
- Features centred on the row, such as the ACE's latch, are placed after layout, with a rule to avoid colliding with a lane's tick on odd lane counts.
- Caps only clear the outer flange: half a spool width plus wall and gap. No half-pitch of dead space at the ends.
- The caps carry the unit's real side profile, because the flat tray camera barely shows depth. For the ACE: low front, cheek climbing to a taller back wall (`TrayBox::back_extra`), dome following the slope. Between the caps, the taller back wall is visible behind the spools.
- Layering follows what `src/ui/filament/ui_ams_detail.cpp` already does: inside faces, then spool widgets, then front, side and glass.

**Performance tiers.** The shell is static between state changes, so it is drawn once and kept; only spools repaint on data changes. The cost risk is the translucent lid blended over spools. Measure on MIPS first. Boards that cannot afford it get the same segments with no glass blending and simplified spools. Which boards land in which tier comes from that measurement.

**Thumbnails are their own drawing tier**, bolder than the full-size art. The current logo slots are 20 to 40px (`ams_logo_size_*`); the spike's 48px thumbnail was already mostly dark mass, so readable colour at the real sizes is a thing to prove, not assume.

**The skill**, lives in the repo at `.claude/skills/` (my call), written alongside the first two units rather than before them. It covers: the kit and how to define a unit; style rules (camera, light, dark-UI palette, tiers); gathering reference photos and proportions before drawing, and never inventing; the layered outputs and overlay anchors; a contact sheet of every unit at real sizes including the smallest screen; and an asset size check, since assets count against small-device installs.

**Overlay anchors.** The kit knows each part's 3D position, so it exports 2D points. This gives #2068's per-unit airflow path, plus lane positions and status overlays.

## First milestone

The ACE Pro proves both outputs: baked art and live tray. ACE Pro and Pro 2 share one drawing. Then a second unit chosen to break the kit, such as a curved AMS shell or a DIY Box Turtle. Only after both hold up is the skill frozen and the remaining units drawn.

Unit list beyond those, in rough priority, is left to the builder, starting from what the backends support: Bambu AMS HT and AMS 2 Pro via OpenAMS, Anycubic ACE, Creality CFS, QIDI Box, Happy Hare and AFC units, then DIY systems in `assets/images/ams/`.

## Preston's decisions on scope

- **ERCF and Tradrack:** draw the unit in front of a row of spools on a generic roller tray. The spools there are live, so these use the live tray model with a different shell, not the box.
- **Themes:** one art set for light and dark. Art carries its own contained shading. Live tray shell colours come from theme tokens.
- **Overview cards when tight:** a live-tinted thumbnail identifies the unit, with today's per-lane bars kept so no status is lost. This is a third, small live rendering alongside the two outputs.
- **Dryer page, first milestone:** lay the page out around a large drawing, with a selector row when there are several units. Airflow animation is a separate later design that uses the anchors.

- **Spool rotation (Preston's idea):** a spool slowly rotates while it is being fed, in the live tray. Driven by the backend's `AmsAction::LOADING` and `UNLOADING` on the active lane, reversing direction for unload. It honours the animations setting and is off on the simpler performance tier. The vents repeat every 60 degrees, so the live spool can loop a small cached set of frames for the one moving spool instead of repainting pixels continuously. The active spool also turns slowly during a print (Preston), as an indication of the printing lane rather than real extrusion speed. It stops when the print pauses (my call). It only runs while the tray is on screen, and it must meet the same performance bar, because a print is its longest-running case. Comes after the first milestone.

Author's reading of what those answers leave open: 3MS and similar selector units follow the ERCF treatment; tool changers get art only and keep today's tray; a system whose physical hardware cannot be identified, such as Happy Hare reporting only firmware, shows its logo rather than a guessed drawing.

## Implementation calls made by the author (check these)

- **Where the unit description lives.** A host-only Python kit is the single source. It generates committed baked images and a generated C++ geometry table for caps and lanes. Firmware and cross-compile builds never run Python; C++ only places, stretches and tints. Per-unit hand-written C++ is not allowed.
- **What baked art shows live.** Full-size art: per-lane colour and empty or absent spool. Fill level and active lane belong to the live tray. Thumbnails: per-lane colour only.
- **ESP32.** Out of scope; it stays on logos and compiles none of the new assets.
- **Asset budget.** Baked art ships only to targets with room for it. The first milestone records actual sizes, and the budget gets set from those numbers.
- **Performance bar.** Full tier needs no visible frame drop while a spool repaints and no more than 2 points of added idle CPU on MIPS (2026-11-04 baseline: 6 to 7 percent, animations off). Tier uses the existing helix::ui::reduced_effects() board decision, with no user override at first.
- **Landing.** Main only, as 1.2 work. Art lands after the second unit validates the kit. Units without a definition keep today's behaviour. The `kSpoolWidth` decision comes before the art baseline is frozen.

## Constraints

- Runs on MIPS, ARM, aarch64 and Pi. Main-thread LVGL only.
- Project declarative-UI, vendor-abstraction and threading rules apply: art selection is a capability lookup in one module, not vendor names in generic code.
- Must work at 800x480 and the smallest breakpoint; `ctl geom` the surfaces before fixing sizes.
- Existing mock backends must be able to show every unit, so screens can be verified without hardware.

## Known open points, for the builder unless noted

- `kSpoolWidth` is 42% of flange diameter against about 35% on a real 1kg spool. It is shared by every tray, so narrowing it changes them all. **Preston suggested spools may be too wide; a real change needs his eye on the result.**
- Dome reads as a straight band in the tray view; needs a stronger curved highlight or more height.
- Light filament shows little through vents and air holes. Any fix should apply to the live spool as well.
- Baked tinting mechanism (masks, layers or recolour) is unproven in LVGL.
- #2068 names `src/ui/filament/ui_dryer_airflow.cpp` and the `dryer_airflow` widget; neither is present on main under that name, so find the current dryer visual first.
