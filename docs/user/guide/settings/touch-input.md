# Settings: Touch & Input

Reached from **Settings → System → Touch & Input**. Groups every setting that affects how the screen reads finger input — calibration, debug visualization, jitter filtering, scroll feel, and long-press behavior.

---

## Touch Calibration

Recalibrate if taps register in the wrong location:

1. Tap **Touch Calibration**
2. Tap each crosshair target as it appears on screen (3 points, 7 taps each)
3. Test that taps land correctly in the verify area
4. Tap **Accept** to save (or **Retry** to redo)

The row description shows "Calibrated" or "Not calibrated" status. Always available — you can recalibrate even on screens that auto-detect as already correct. For the full menu of force-calibration options (env var, config file, CLI) and per-platform paths, see the [Touch Calibration Guide](../touch-calibration.md).

---

## Show Touch Points

Toggles a debug overlay that draws a ripple at every touch point. Useful when taps feel offset or buttons aren't responding where you expect — turn it on, tap around, and see exactly where the system thinks your finger is. Turn off when done.

Takes effect immediately — no restart required.

> Persistent equivalent: `HELIX_DEBUG_TOUCH=1` in `helixscreen.env`. The Settings toggle and the env var read the same flag.

---

## Touch Jitter Filter

A dead zone in pixels that suppresses tiny coordinate noise from the touch controller. Range `0`–`30`, default `5` — which works for most panels.

| Symptom | Suggested value |
|---|---|
| Stationary taps register as swipes (common on Goodix GT9xx capacitive controllers) | **15–25** |
| Default — works on most panels | **5** |
| Disable the filter entirely (ultra-precise touch) | **0** |

Requires a restart to take effect. HelixScreen offers a restart prompt automatically after you change the slider.

---

## Scroll Engage Distance

Pixels of finger travel before a press becomes a scroll instead of a click. Range `1`–`20`, default `10`.

| Symptom | Suggested value |
|---|---|
| Scrolls fire a click on whatever was under your finger when you meant to scroll | **5** |
| Default — sweet spot for most panels | **10** |
| Taps feel twitchy, micro-wobbles start scrolls | **15** |

Requires a restart to take effect.

---

## Long Press Time

How long you need to hold your finger down before a press counts as a **long-press**. Range `300`–`1500` ms, default `500` (about half a second).

A long-press is the gesture behind several actions — entering home-screen Edit Mode, deleting a file card, opening macro edit mode, and others. If those trigger when you're just resting your finger on the glass (common on a tablet lying flat), raise this value. A setting around `800`–`1000` makes accidental long-presses much rarer without making deliberate ones feel sluggish.

Takes effect immediately — no restart required.

---

## Allow Home Screen Editing

Toggles whether a long-press on the home grid enters **Edit Mode** (the drag-and-drop layout editor). **On by default.**

If Edit Mode triggers by accident — typically a finger resting on a tablet lying flat — turn this off and long-pressing the home grid will do nothing. You can turn it back on when you want to rearrange, resize, add, or remove widgets.

Takes effect immediately — no restart required.

> Want to fine-tune the hold time instead of disabling Edit Mode entirely? Raise the **Long Press Time** slider above — a longer threshold makes accidental entry harder while keeping the feature available.

---

## Scroll Guard

Some capacitive controllers fire a phantom "clicked" event when you lift your finger after scrolling. Enable Scroll Guard to ignore taps for ~80 ms after a scroll ends.

FlashForge AD5M and AD5X enable this automatically via their hardware presets — leave it on. Most Raspberry Pi setups don't need it.

Requires a restart to take effect.

> Still seeing phantom clicks with the guard enabled? Some controllers need a longer cooldown. Tune `scroll_guard_cooldown_ms` in `settings.json` — see the [TROUBLESHOOTING guide § Accidental Button Presses After Scrolling](../../TROUBLESHOOTING.md#accidental-button-presses-after-scrolling).

---

[Back to Settings](../settings.md) | [System Settings](system.md) | [Touch Calibration Guide](../touch-calibration.md)
