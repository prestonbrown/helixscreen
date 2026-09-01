# Forge-X 1.4.2 compatibility + HEADLESS install slot

Status: in progress (2026-08-27)
Branch: `feature/forgex-headless-install`

## Why

Forge-X 1.4.2 (beta-2 as of 2026-08-25) changes three things our AD5M installer
depends on. Separately, DrA1ex closed `DrA1ex/ff5m#74` and asked that HelixScreen
live on HEADLESS rather than GUPPY, because any other slot risks failed OTA
updates and repeated Moonraker recovery prompts.

Target: one code path that works on 1.4.0, 1.4.1 and 1.4.2. Verified that 1.4.0
and 1.4.1 are byte-identical across every file we touch, so "1.4.1 compatibility"
and "1.4.0 compatibility" are the same requirement.

## What changed upstream (1.4.0/1.4.1 -> 1.4.2-beta-2)

| Area | 1.4.0 / 1.4.1 | 1.4.2-beta-2 |
|------|---------------|--------------|
| `mod_params.json` display default | `STOCK` | `FEATHER` |
| `screen.sh` draw commands | `draw_loading`, `draw_splash`, `boot_message` | `draw_splash` only; adds `splash_start`/`splash_version`/`splash_subtitle`/`splash_stop` |
| `start.sh` tslib | GUPPY only | FEATHER **or** GUPPY |
| `config/headless.cfg` | base+headless+client, no backlight macro | adds `reset_screen` (dims) and `initial_boot` (`screen.sh draw_splash`) |
| `backlight)` case body | unchanged between all three | unchanged |
| `logged --send-to-screen` | present | present |
| `/etc/init.d` user-services loop in `start.sh` | present, byte-identical | present, byte-identical |

## Defects this fixes

1. **`configure_forgex_display()` has no FEATHER branch** (`forgex.sh:17`). It only
   rewrites `'STOCK'`/`'HEADLESS'` -> GUPPY. On a fresh 1.4.2 install (default
   FEATHER) it changes nothing and returns 1, leaving Feather drawing over us.
   Feather is not a killable process - it is Klipper macros in
   `./mod/config/feather.cfg` calling `screen.sh`. Our `COMPETING_UIS` list
   (`competing_uis.sh:18`) contains `featherscreen`, which matches nothing.

2. **`patch_forgex_screen_drawing()` silently half-applies** (`forgex.sh:~200`). It
   awk-matches `draw_loading|draw_splash|boot_message`; on 1.4.2 only `draw_splash`
   exists. Its success check is `grep -q 'helixscreen_active'` over the whole file,
   which passes off that one match, so it logs success while leaving the new
   `splash_start` path unguarded.

3. **GUPPY is the wrong slot.** The stated reason (`forgex.sh:14`, "ForgeX handles
   backlight properly in this mode") does not hold: the backlight path is
   `chroot "$MOD" /root/printer_data/py/backlight.py` from screen.sh's `backlight)`
   case, which is mode-independent. In 1.4.0/1.4.1 it is `guppy.cfg` - not
   `headless.cfg` as the comment claims - that adds the dimming `reset_screen`
   delayed_gcode. We selected GUPPY and then patched around a problem GUPPY
   introduced.

## Decisions

- **Target HEADLESS** for all three versions. Rewrite `STOCK`/`FEATHER`/`GUPPY` -> `HEADLESS`.
- **Do not move the init script.** `/etc/init.d/S90helixscreen` (`platform.sh:818`)
  is host-side: dropbear runs on the host, there is no chroot-on-login, so an SSH
  install writes the host's `/etc/init.d`. BusyBox init runs it between
  `S60dropbear` and `S99root`, before the chroot comes up. It is already
  display-mode independent, which is exactly what makes the HEADLESS switch safe.
  (The image branch's `.root/S90helixscreen` + explicit `start.sh` wiring is the
  approach PR #74 proposed and DrA1ex rejected; the future image should drop it
  and use the installer's slot.)
- **Capability-detect, do not version-detect.** Guard whichever of
  `draw_loading` / `boot_message` / `draw_splash` / `splash_start` exist, and
  verify each intended label individually instead of grepping the whole file.
- **Keep** the backlight patch and the `logged` wrapper. Both are version-agnostic
  and still needed (1.4.2's headless.cfg re-adds `reset_screen`).
- **Drop** the tslib / guppyscreen `chmod a-x` hack. Under HEADLESS `start.sh`
  invokes neither, in any of the three versions. Today they are de-execed and fail
  with permission-denied on every boot. Install now *restores* the execute bit, so
  a machine carrying an older HelixScreen install stops wearing our footprint.
- **Record the pre-install display mode** (`mod_data/helixscreen_prev_display`) and
  restore it on uninstall, falling back to GUPPY when absent. Without this the
  HEADLESS switch would be a regression: `uninstall_forgex` hardcoded a restore to
  GUPPY, which for a 1.4.2 printer means uninstalling leaves it on a mode it never
  had.
- **Leave `COMPETING_UIS` alone.** Its `featherscreen` entry matches no process on
  Forge-X, but Feather is handled by the display-mode switch, and the entry is
  inert rather than harmful.

## Notes / upstream bugs observed

- `stop.sh`'s user-services loop uses a **relative** `find ./etc/init.d/` where
  `start.sh` uses absolute `/etc/init.d/`. Present in all three versions. Our stop
  path must not depend on being called.
- `splash` supports `--duration` but `splash_start` does not pass it; it runs until
  `splash_stop` over the control FIFO. `NOT_FIRST_LAUNCH_F` is `/tmp/not_first_launch_f`,
  cleared each boot, so `splash_stop` does run every boot. No leaked process.

## Verification status

**Not hardware-verified.** The AD5M test device (192.168.1.67) was unreachable
throughout ("No route to host"), so everything here rests on static analysis of
the upstream 1.4.0 / 1.4.1 / 1.4.2-beta-2 tags plus the new bats coverage.

`tests/shell/test_forgex_display_modes.bats` (20 cases) runs the real functions
against a mock /opt tree rather than grepping them as text, covering both
screen.sh shapes, the mode matrix, idempotency, and the uninstall restore.
Mutation-checked: removing `splash_start` from `FORGEX_DRAW_COMMANDS` or `FEATHER`
from `FORGEX_DISPLAY_MODES` turns the matching cases red.

Still to do before the image ships:
- Flash a real AD5M on 1.4.2 final and confirm boot with no splash contention.
- Re-diff against 1.4.2 **final**; beta-2 is a moving target and DrA1ex said the
  boot process could still change.
