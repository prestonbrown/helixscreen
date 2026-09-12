# GPU Acceleration: Presentation, Drawing, and Rotation

**Date:** 2026-09-10
**Status:** Design, not yet implemented
**Issues:** prestonbrown/helixscreen#1580, prestonbrown/helixscreen#1581, prestonbrown/helixscreen#1582, prestonbrown/helixscreen#1275
**Goal:** hardware acceleration wherever a board can actually deliver it, software fallback everywhere else.

## What the fleet actually offers

Measured 2026-09-10 with a throwaway DRM/EGL probe against every owned DRM device. This
table is the design input; everything below follows from it.

| Device | Display node | Plane rotation mask | 90/270 | 180 | GPU | EGL on the scanout node |
|---|---|---|---|---|---|---|
| Pi 5, DSI panel (connected) | `drm-rp1-dsi` | `0x0` | no | no | V3D 7.1.7, GLES 3.1, Mesa 24.2.8 | works |
| Pi 5, HDMI (disconnected) | `vc4` | `0x35` | no | yes | V3D | works |
| Pi 3B, DSI panel (connected) | `vc4` | `0x35` | no | yes | VC4 V3D 2.1, GLES 2.0, Mesa 25.0.7 | works |
| CB1 / Voron, current image (Armbian 26.8.1 / Debian 13, stock Mesa) | `sun4i-drm` | `0x0` | no | no | Mali-G31 (Panfrost), GLES 3.1, Mesa 25.0.7 | works |
| CB1 / Voron, stale image (BTT/Siboor, Debian 11, `/opt/panfrost` Mesa) | `sun4i-drm` | `0x0` | no | no | Mali-G31, GLES 3.1, Mesa 21.3.9 | `gbm_create_device` **fails** |
| Snapmaker U1 | `rockchip` | `0x21` | no | no | none installed (card1 is an RKNPU, not a GPU) | impossible |

Four conclusions, three of which contradict assumptions in the issues:

1. **No plane in the fleet rotates 90 or 270.** The fbdev swap is not a compromise, it is the
   only mechanism that exists for a sideways panel.
2. **The Pi 5's live DSI panel has no plane rotation at all**; its `0x35` mask belongs to the
   disconnected HDMI connector. The Pi 3B's live DSI panel is the exception: it sits on `vc4`
   directly, carries that same `0x35` mask, and is the only board in the fleet with a
   rotation-capable plane, a connected panel, and working EGL all at once.
3. **The U1 has no `libEGL`, `libGLESv2`, or `libgbm`.** Its only acceleration is the plane,
   and its mask (`rotate-0 | reflect-y`) excludes every angle we would ask for.
4. **EGL capability is a property of the installed image, not the silicon.** The CB1's
   `/opt/panfrost` Mesa 21.3.9 has no `kms_swrast`/`swrast` driver, so allocating a scanout
   buffer on `sun4i-drm` fails; the identical board on a current Debian 13 image (stock Mesa
   25.0.7, no `/opt/panfrost`) succeeds on that same node, with `GL_RENDERER` reporting the
   real Mali, not a software fallback. Both run the same `pi-both` binary.

Point 4 is the load-bearing one, and it is stronger than a single board's hardware could ever
make it: **capability is a runtime property of the installed image, not a compile-time
property of the target, and not a fixed property of the silicon.** The same binary on the
same board answers differently depending on which SD card is in it. Compile time cannot know
that.

## Three layers, currently welded together

| Layer | Decides | Today | Available |
|---|---|---|---|
| Presentation | how a finished frame reaches the panel | DRM dumb buffers (CPU memcpy + page flip) | EGL/GBM on a real GPU |
| Drawing | what rasterizes widgets | `lv_draw_sw` everywhere | `lv_draw_nanovg` (vendored, switched off) |
| Rotation | who turns the picture | CPU, via the fbdev swap | DRM plane (180 only, one config) |

Drawing's `lv_draw_nanovg` is not a peer option to Presentation's EGL/GBM: it renders into
the layer's bound GL surface and has no readback path onto a dumb buffer or fbdev
framebuffer, so it can only run on top of the EGL rung, never instead of it.

All three currently hang off `HELIX_ENABLE_OPENGLES`, a macro that does not do what its
readers believe.

## The defect

`lv_conf.h#LV_LINUX_DRM_USE_EGL` is set from `HELIX_ENABLE_OPENGLES`, then
`lib/lvgl/src/lv_conf_internal.h` redefines it from `LV_USE_OPENGLES` with no `#ifndef`
guard. `lv_conf.h` pins `LV_USE_OPENGLES 0`, so the value resolves to 0 on every target and
the compiler says so on every build:

```
lib/lvgl/src/lv_conf_internal.h:4898: warning: "LV_LINUX_DRM_USE_EGL" redefined
```

`lv_linux_drm_egl.c` is therefore an empty translation unit, and
`src/api/display_backend_drm.cpp` reads the *request* (`HELIX_ENABLE_OPENGLES`) rather than
the *result*, so it claims GPU acceleration in the log and disables plane rotation to pay for
a path that was never compiled. `include/display_backend_drm.h#using_egl_` propagates the
same claim to `src/application/display_manager.cpp`, giving two false log lines.

**Resolve config macros with the preprocessor, never by reading the header.**

```sh
printf '#include "lv_conf_internal.h"\nX = LV_LINUX_DRM_USE_EGL\n' > /tmp/p.c
gcc -E -I lib/lvgl/src -I . -DHELIX_DISPLAY_DRM -DHELIX_ENABLE_OPENGLES \
    -DLV_CONF_INCLUDE_SIMPLE /tmp/p.c | grep '^X'
```

## Design

### 1. Make the request real, and make drift a build failure

`lv_conf.h` sets `LV_USE_OPENGLES` from `HELIX_ENABLE_OPENGLES`, which is the only value
`lv_conf_internal.h` derives `LV_LINUX_DRM_USE_EGL` from. Patching `LV_LINUX_DRM_USE_EGL`
directly cannot work; the same override eats it.

Then make the class of bug unrepresentable rather than merely fixing its symptom. In
`display_backend_drm.cpp`:

```c
#if defined(HELIX_ENABLE_OPENGLES) && !LV_LINUX_DRM_USE_EGL
#error "HELIX_ENABLE_OPENGLES set but LVGL resolved LV_LINUX_DRM_USE_EGL to 0"
#endif
```

A log line that can drift is what hid this for six months. A build that cannot start is not a
log line. Every remaining `#ifdef HELIX_ENABLE_OPENGLES` in that file becomes
`#if LV_LINUX_DRM_USE_EGL`, and `using_egl_` is set from the resolved macro.

### 2. A fallback ladder, chosen by probe rather than by crashing

LVGL makes `lv_linux_drm.c` and `lv_linux_drm_egl.c` mutual exclusives
(`#if LV_USE_LINUX_DRM && !LV_LINUX_DRM_USE_EGL`), so one binary cannot hold both drivers.
The original design accepted that and fell from EGL straight to fbdev. The measurements say
that demotes any CB1 whose EGL probe fails — which a stale `/opt/panfrost` image still does,
even though a current Debian 13 image on the identical board does not — from DRM dumb
buffers to fbdev, so we add a rung instead:

```
helix-screen-egl    EGL/GBM presentation on a real GPU
        |  --probe-egl fails
helix-screen        DRM dumb buffers, atomic modesetting, vsync   (today's behaviour)
        |  DRM unavailable, or crash, or HELIX_DISPLAY_BACKEND=fbdev
helix-screen-fbdev  /dev/fb0, software rotation at any angle
```

`scripts/helix-launcher.sh` already selects between the lower two rungs and already retries on
a crash exit. The new rung reuses that machinery.

**Selection is a probe, not a crash.** A failed EGL init inside the app would fall through to
fbdev in-process, skipping the middle rung entirely. So `helix-screen-egl --probe-egl` does
only the EGL bring-up (`gbm_create_device`, `eglInitialize`, `eglChooseConfig`,
`eglCreateContext`) and exits 0 or non-zero. The launcher picks on that, deterministically,
with no partial UI and no crash loop. It is also the one piece of this that is testable off
hardware.

Build: `mk/pi-dual-link.mk` gains a third link. A filter is not enough here, because the EGL
rung needs LVGL's DRM sources *recompiled* with `LV_USE_OPENGLES=1` rather than merely a
different subset linked. That is the `FBDEV_GLES_VARIANT_OBJS` pattern in the same file, which
already recompiles a named set of sources under different flags into a separate object
directory, applied to `LVGL_DRM_DRIVER_OBJS`.

### 3. Rotation

`src/api/drm_rotation_strategy.cpp#choose_drm_rotation_strategy` is correct and stays. What
changes is who is allowed to act on its answer.

Fixing §1 exposes `DrmRotationStrategy::HARDWARE` for the first time, on exactly one
configuration in the fleet: a board on the dumb-buffer rung driving a `vc4` HDMI panel at
`rotate: 180`. That configuration is broken today. `src/application/display_manager.cpp` calls `lv_display_set_rotation()` before asking
the backend for a strategy, so the HARDWARE path rotates the plane *and* leaves LVGL rotated,
and `patches/lvgl-drm-flush-rotation.patch` does its CPU 180 reversal on top. Two 180s cancel:
the user sees an unrotated screen having paid for a 1.5MB shadow buffer and a full-frame CPU
reversal every frame.

So **#1275 and #1580 land together**. Ownership moves to the backend: DisplayManager hands the
angle down and each backend decides whether LVGL is also told, which is already how fbdev and
SDL behave.

The EGL rung declares no rotation support, and needs no launcher involvement to handle a
rotated unit: `display_manager.cpp#try_drm_to_fbdev_fallback` already deletes the DRM display
and rebuilds on the fbdev backend in-process, rebuilding the input devices with it. That is
how rotation reaches a Pi today, and it keeps working on the EGL rung unchanged. Nothing is
lost by declaring no rotation, because no plane in the fleet offers 90/270, the Pi 5's live
panel offers nothing at all, and the Pi 3B's live panel only offers 180 — which the same
fbdev fallback already reaches, just without the plane doing the work.

### 4. Context sharing (#1582)

`patches/lvgl-drm-egl-getters.patch` stops being dead the moment the EGL rung exists, because
`src/rendering/gcode_gles_renderer.cpp` can then adopt the display's `EGLDisplay`/`EGLContext`
instead of creating a second one.

This is an optimization, not a correctness fix: the renderer already saves and restores the
current context around its own work. It earns its place on memory, which is not academic on a
CB1 that hit prestonbrown/helixscreen#966 at 492MB RSS of 918MB. Scope it after the ladder
works; if the ladder is abandoned, delete the patch instead.

## Scope

**1.1** — §1 (config + `#error` gate + honest logs), §3 (rotation ownership, #1275+#1580 in one
change), and #1581 closed as a documentation correction. §2 if the third link lands cleanly.

**Later** — §4, and the nanovg draw unit. nanovg is vendored in full
(`lib/lvgl/src/draw/nanovg/` and `lib/lvgl/src/libs/nanovg/`) and needs both `LV_USE_NANOVG`
and `LV_USE_DRAW_NANOVG`; the second alone links with undefined `nvg*` symbols. Enabling it
also turns on `LV_DRAW_TRANSFORM_USE_MATRIX`, which adds an `lv_matrix_t` member to
`lv_layer_t` and `lv_draw_task_t` — an ABI change every translation unit including
`lv_draw.h` sees, so a nanovg binary is a full separate build, never a link variant off a
shared object set. Plain EGL (`LV_USE_OPENGLES`) carries no such cost: it only touches
`lv_types.h` under `LV_USE_3DTEXTURE`, which resolves to 0. nanovg's justification is that it
is the only way `LV_DRAW_TRANSFORM_USE_MATRIX` becomes legitimate — widget transforms
currently crop on scale-up and no-op on scale-down — not raw speed: it renders on top of the
EGL rung, not instead of it, and its reachable rasterization gain is bounded well under what
EGL presentation alone already delivers. It also changes how every widget rasterizes, so it
wants its own verification pass rather than riding along with this one.

`lv_draw_opengles` is the weaker draw unit and is not proposed: its `draw_to_texture()`
re-enters the software rasterizer per unique shape and caches the result, cutting
frame-to-frame redundancy rather than first-draw cost, and it declines skewed images.

## Verification

Software gates cannot see any of this, so name the hardware explicitly.

| Claim | How |
|---|---|
| `LV_LINUX_DRM_USE_EGL` resolves to 1 on a `pi` build | preprocessor one-liner above; the `#error` gate then enforces it |
| EGL rung runs on real V3D | `GL_RENDERER` on the Pi is `V3D 7.1.7.0`, never `llvmpipe` |
| Probe declines on a CB1 with the stale image | `helix-screen-egl --probe-egl` exits non-zero on the `/opt/panfrost` 21.3.9 image; launcher lands on `helix-screen` |
| Probe succeeds on a CB1 with a current image | `helix-screen-egl --probe-egl` exits 0 on Debian 13 / Mesa 25.0.7; `GL_RENDERER` reports the real Mali, never `llvmpipe` |
| CB1 keeps DRM dumb buffers when the probe declines | log says DRM, not fbdev, after the ladder change |
| Rotation no longer double-applies | Pi + HDMI + `rotate: 180`: picture is actually inverted |
| 90/270 still works | Pi + `rotate: 90` reaches the fbdev rung and renders rotated |
| U1 unaffected | mask `0x21` still yields SOFTWARE; no behaviour change |

Both GPU stacks must be exercised. Panfrost is not V3D, and prestonbrown/helixscreen#966 was a
GLES crash on the CB1.

## Risks

- **Third link** grows the Pi tarball by roughly one binary and adds build time.
- **Mesa skew**: built against a Bullseye sysroot (Mesa 20.3, which is why
  `LV_USE_LINUX_DRM_GBM_BUFFERS` is off), run against Mesa 24.2 on the Pi 5, Mesa 25.0 on the
  Pi 3B, and on the CB1 either Mesa 21.3 (a stale `/opt/panfrost` image) or Mesa 25.0 (current
  Debian 13). EGL and GLES are ABI-stable, but `gbm` is where this already bites.
- **Pi 5 split scanout**: rendering is on `v3d` and scanout on `drm-rp1-dsi`. The probe
  confirms Mesa's kmsro path handles it, but that is one board and one kernel.
- **A Pi 3B is now owned** (VideoCore IV / `vc4`, GLES 2.0, Mesa 25.0.7) alongside the Pi 5.
  Pi 4 is still in the original design's target list and not owned; the ladder degrades
  safely on it, but untested.
- **The EGL rung does not link yet.** `src/api/display_backend_drm.cpp` calls
  `lv_linux_drm_set_preferred_mode()`, which exists only in the dumb-buffer driver
  (`lv_linux_drm.c`); under `LV_LINUX_DRM_USE_EGL=1` that file compiles to nothing, so the
  call needs a guard before the third link can succeed. Guarding it costs forced-mode
  selection on that rung: a board whose requested mode differs from the connector's
  preferred mode gets the connector's preferred mode instead.
