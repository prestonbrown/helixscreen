# GPU Acceleration Phase 2: Handoff

**Date:** 2026-09-11
**Phase 1 landed as:** `aa8d86153` on main
**Spec:** `docs/devel/plans/2026-09-10-gpu-acceleration-design.md` - read this first, it is the binding design
**Phase 1 plan:** `docs/devel/plans/2026-09-10-gpu-acceleration-phase1.md` - its "What Phase 2 inherits" section is the short version of this file

Phase 1 delivered no acceleration. It made the display's capability claims true so that
Phase 2 is a behaviour change rather than a bug reveal. Phase 2 is where the frames come
from.

## What the hardware actually offers

Measured 2026-09-11 against every owned DRM device. Do not re-derive this from source;
do re-measure on any board not listed.

| Device | Display node | Plane rotation | GPU | EGL on the scanout node |
|---|---|---|---|---|
| Pi 5, DSI panel (the one in use) | `drm-rp1-dsi` | `0x0` | V3D 7.1.7, GLES 3.1, Mesa 24.2.8 | works |
| Pi 5, HDMI | `vc4` | `0x35` (0, 180, reflects) | V3D | works |
| BTT CB1 | `sun4i-drm` | `0x0` | Mali-G31, GLES 3.1, Mesa 21.3.9 | **`gbm_create_device` fails** |
| Snapmaker U1 | `rockchip` | `0x21` (0, reflect-y) | none installed | impossible |

Three consequences that shape the whole phase:

1. **No plane in the fleet rotates 90 or 270.** The framebuffer path is the rotation
   mechanism, not a fallback from a better one.
2. **The CB1 fails EGL and runs the same `pi-both` binary as the Pi.** Its
   `/opt/panfrost` Mesa ships no `kms_swrast`/`swrast` driver, so allocating a scanout
   buffer on `sun4i-drm` returns NULL even though Mali renders fine. This is why the
   design calls for a runtime probe rather than a compile-time switch: a compile-time
   decision ships broken to one of the two boards.
3. **The U1 has no `libEGL`, `libGLESv2` or `libgbm` at all.** Its only hardware path is
   the plane, and its mask excludes every angle we would ask for.

**The fleet is not the product.** An `x86_64` desktop with `amdgpu` reports plane masks of
`0xf` - rotate-0, 90, 180 and 270, the full set. `x86` and `x86-both` are shipped targets,
so a board with a rotation-capable plane is a configuration HelixScreen ships to and nobody
here owns. Phase 1 nearly shipped a defect on exactly that basis: it reasoned "no owned
board advertises 90 or 270, so the case is unreachable", which is fleet scope stated as
product scope. Measure the box in front of you, then ask what else the target reaches.

## The two things Phase 2 can build

**EGL presentation** replaces a CPU memory copy with a GPU page flip. It accelerates how
a finished frame reaches the panel. Rasterization stays on the CPU. This is the smaller
win and the one the design's section 2 describes.

**The nanovg draw unit** moves rasterization itself to the GPU. It is fully vendored
already - `lib/lvgl/src/draw/nanovg/` and `lib/lvgl/src/libs/nanovg/` - and switched off.
It needs **both** `LV_USE_NANOVG` and `LV_USE_DRAW_NANOVG`; setting only the second links
with undefined `nvg*` symbols. This is the larger win, and it is also what makes
`LV_DRAW_TRANSFORM_USE_MATRIX` legitimate, since only `vg_lite` and `nanovg` read the
per-draw-task matrix.

`lv_draw_opengles` is the weaker draw unit and is not proposed: its `draw_to_texture()`
re-enters the software rasterizer per unique shape and caches the result, cutting
frame-to-frame redundancy rather than first-draw cost, and it declines skewed images.

Worth investigating: a matrix-capable draw unit may give DRM real 90/270 rotation through
LVGL's matrix rotation, which would remove the fbdev swap entirely. Unverified.

## Traps that will cost you time

**Resolve config macros with the preprocessor, never by reading `lv_conf.h`.** That file
says one thing and the build sees another. This is the entire substance of #1580.

```sh
printf '#include "lv_conf_internal.h"\nX = LV_LINUX_DRM_USE_EGL\n' > /tmp/p.c
gcc -E -I lib/lvgl/src -I . -DHELIX_DISPLAY_DRM -DHELIX_ENABLE_OPENGLES \
    -DLV_CONF_INCLUDE_SIMPLE /tmp/p.c | grep '^X'
```

**`plane_may_own_rotation()` returns a constant `false`, and it is not dead code.** It
exists because LVGL transforms pointer input solely from its own display rotation
(`lv_display_rotate_point`, which early-returns at `ROTATION_0` and has exactly one
caller, `lv_indev.c`'s pointer path). Rotating the plane while clearing LVGL's rotation
gives a rotated picture and an unrotated touch frame. **Flipping it to true requires
making touch follow the plane first** - transforming pointer coordinates independently of
`lv_display_get_rotation()`. Do not delete it because it looks inert.

**No `DrmRotationStrategy` branch is acted on in the shipped configuration.** SOFTWARE is
unreachable because `DisplayManager::try_drm_to_fbdev_fallback` replaces the backend with
fbdev before it could be used; HARDWARE is unreachable because of the gate above. The
decision function and its tests are kept intact because Phase 2 needs them, not because
anything calls the result.

**Rotation ownership only half-moved.** The design says DisplayManager hands the angle
down and each backend decides whether LVGL is also told. In the code DisplayManager still
calls `lv_display_set_rotation()` itself and then calls the backend, which may clear it,
so the value is written twice and the winner depends on call order. `DisplayBackendFbdev`'s
override is a no-op that relies on DisplayManager having set it. Do not assume that design
section landed.

**The `#error` drift gate is one-directional.** It catches `HELIX_ENABLE_OPENGLES` set
without the macro resolving. Setting `LV_USE_OPENGLES 1` without the request is silent,
and the compiler's "redefined" warning that used to cover that direction does not fire now
that both headers agree on the token.

**Flipping `ENABLE_OPENGLES` moves a source file between compilers.**
`Makefile` compiles `lib/lvgl/src/drivers/opengles/assets/lv_opengles_shader.c` as **C++**
with `-fpermissive` when the flag is yes (it uses C++11 raw string literals), and as
ordinary C when it is no. The object path is identical, so make will happily keep a stale
object built by the other compiler. A clean build is the first thing that tries the new
rule.

**`make pi-docker` exits 2 after a successful build** whenever nothing recompiled - the
post-build `compile_commands.json` merge errors on an empty fragment set. Check the
artifact, not the exit code. Filed as #1588.

**Do not verify the rotation/touch path on the Pi's DSI panel.** Its plane mask is `0x0`,
so it takes the fbdev path regardless of what the code does, and it will pass whether or
not the fix is present. Reaching the plane path needs the HDMI connector, whose mask is
the only one in the fleet containing 180.

## Verification this phase requires

Both GPU stacks, because Panfrost is not V3D and #966 was a GLES crash on the CB1:

- **Pi** - confirm `GL_RENDERER` is `V3D ...` and never `llvmpipe`.
- **CB1** - confirm the probe *declines* and the app lands on the rung below rather than
  failing. This board is the reason the design uses a runtime probe.
- **U1** - confirm nothing changed. It has no GL userspace and must keep the dumb-buffer
  driver.

`tools/drm_gpu_probe.c` answers all of this in one pass per board: driver name, atomic
capability, every plane's rotation mask, and whether GBM plus a real GLES2 context come up
and which renderer answers. Build instructions are in its header comment; it cross-compiles
in the existing toolchain images and takes one `scp` to run. Build it with `-DNO_EGL` for a
board with no GL userspace, such as the U1.

Run it on any board before reasoning about what that board can do. It was written because
Phase 1 spent its first hour arguing from source about capabilities that took ten minutes
to measure.

## Open decisions Phase 2 owns

1. **How EGL and the dumb-buffer driver coexist.** LVGL makes `lv_linux_drm.c` and
   `lv_linux_drm_egl.c` mutually exclusive (`#if LV_USE_LINUX_DRM && !LV_LINUX_DRM_USE_EGL`),
   so one binary cannot hold both. The design proposes a third binary plus a launcher rung
   selected by `--probe-egl`; the original 2026-02 design accepted falling straight from
   EGL to fbdev in one binary. The measurements favour the rung, because the one-binary
   route silently demotes every CB1 from DRM to fbdev. `mk/pi-dual-link.mk` already has the
   recompile-under-different-flags pattern (`FBDEV_GLES_VARIANT_OBJS`) this needs.
2. **Whether #1582's EGL context getters get wired or dropped.** They stop being dead the
   moment `lv_linux_drm_egl.c` compiles. Note the gcode renderer already saves and restores
   the current context, so sharing is a memory optimisation rather than a correctness fix -
   which matters on a CB1 that hit #966 at 492MB RSS of 918MB.
3. **Whether nanovg ships in the same phase as EGL or after it.** It changes how every
   widget rasterizes and deserves its own verification pass.

## Related issues

- #1580 EGL never enabled (Phase 1 closed the honesty half; the enablement is Phase 2)
- #1581 Pi/DRM rotation (closed as a documentation correction)
- #1582 EGL context getters - blocked on the decision above
- #1275 rotation double-apply (fixed; the gate is what keeps it from recurring)
- #1586 namespace gate exempts `Display*`/`drm*` - will silently exempt new display code
- #1587 `apply_rotation` has the ordering `init` had - goes live if HARDWARE ever does
- #1588 `make pi-docker` exit code
- #966 GLES crash on CB1 - the reason that board gets its own verification
