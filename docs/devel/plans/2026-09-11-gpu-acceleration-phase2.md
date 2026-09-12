# GPU Acceleration Phase 2 — ship the EGL rung, move rotation ownership

**Status:** not started. Phase 1 shipped as `aa8d86153`.
**Measured facts and the nanovg verdict:** `docs/devel/GPU_ACCELERATION.md` — read it
first; this plan does not repeat its numbers.

Phase 2 is two pieces of work that share a gate. The first ships the presentation
win that is already measured and currently enabled nowhere. The second is the
rotation and touch ownership question, which was always Phase 2's other half and
is the only place a GPU plane can pay off beyond presentation.

**nanovg is out of scope, permanently.** It is unusable upstream for reasons
recorded in `GPU_ACCELERATION.md`, and its ceiling is about half of what EGL has
already delivered. Do not reopen it without new upstream code.

---

## What bounds this work

Facts that constrain the design and are not obvious from the code:

- **`ENABLE_OPENGLES := no` at all nine target sites in `mk/cross.mk`.** Nothing
  ships the measured win today.
- **The U1 is permanently excluded.** No `libEGL`, `libGLESv2` or `libgbm` on the
  device; its `card1` is an RKNPU. Its plane mask `0x21` is rotate-0 plus
  reflect-y, which is not rotation.
- **The fleet is not the product.** `x86_64` + `amdgpu` reports plane mask `0xf`
  — full 90/180/270 — and `x86`/`x86-both` are shipped targets. Nobody here owns
  such a board, so "no owned board advertises 90 or 270" is not a safety argument.
- **The Pi 5 splits render and scanout** (`v3d` render node, `drm-rp1-dsi`
  scanout) and relies on Mesa kmsro to pair them.
- **Sysroot Mesa is 20.3 (Bullseye) against a 24.2/25.0 runtime.** This is why
  `LV_USE_LINUX_DRM_GBM_BUFFERS` is off; leave it off unless that changes.
- **The Pi 4 is unowned.** Any claim about it is untested.

---

## Task 1 — the EGL rung

A third binary and a launcher rung, not an in-process fallback.

```
helix-screen-egl    EGL/GBM presentation on a real GPU
        |  --probe-egl fails
helix-screen        DRM dumb buffers, atomic modesetting, vsync   (today)
        |
helix-screen-fbdev  (existing bottom rung)
```

**Selection is a probe, not a crash.** A failed EGL init inside one binary would
fall through to fbdev in-process and skip the middle rung entirely — silently
demoting a board from DRM dumb buffers to fbdev. So `helix-screen-egl
--probe-egl` does only the EGL bring-up (`gbm_create_device`, `eglInitialize`,
`eglChooseConfig`, `eglCreateContext`) and exits 0 or non-zero.

The probe must also reject a **software** renderer. The CB1's original failure
was a stale Mesa in `/opt/panfrost` missing `kms_swrast`/`swrast`; the inverse —
succeeding into llvmpipe — is worse than failing, because it costs CPU to save
CPU. Check the renderer string, not just that a context was created.

**Launcher:** `scripts/helix-launcher.sh` has two rungs today
(`FALLBACK_BIN="${BIN_DIR}/helix-screen-fbdev"`). Add the EGL rung, and add
`egl` as a forcing value so the probe can be overridden for testing.

**Build:** `mk/pi-dual-link.mk` gains a third link. A filter is not enough — the
`FBDEV_GLES_VARIANT_OBJS` pattern in that file is the shape to follow.

### The build trap that will cost you a day

**Flipping `ENABLE_OPENGLES` moves a source file between compilers.** `Makefile`
compiles `lib/lvgl/src/drivers/opengles/assets/lv_opengles_shader.c` as **C++**
with `-fpermissive` when the flag is yes (it uses C++11 raw string literals), and
as ordinary C when it is no. **The object path is identical**, so make will
happily link a stale object built by the other compiler. Any variant scheme has
to give these distinct object paths.

---

## Task 2 — per-board policy

EGL is not a global yes. The numbers in `GPU_ACCELERATION.md` make the CB1 the
hard case: it gains the most throughput and the least CPU relief, and pays
+70 MB on a 969 MB board that already has a GLES crash in its history.

Decide and record, per target: does the probe get to say yes, and what is the
memory headroom check? A board that passes the probe and then OOMs mid-print is
a worse outcome than one that never used the GPU.

---

## Task 3 — rotation and touch ownership

Two coupled problems. Neither has moved since Phase 1.

**Ownership only half-moved.** `DisplayManager` still calls
`lv_display_set_rotation()` itself *and* calls the backend, which may clear it —
so the value is written twice and the winner depends on call order.
`DisplayBackendFbdev`'s override is a no-op that relies on DisplayManager having
set it. `#1275` and `#1580` land together: ownership moves to the backend,
DisplayManager hands the angle down, and each backend decides whether LVGL is
also told. That is already how fbdev and SDL behave.

**`plane_may_own_rotation()` returns a constant `false` and is not dead code.**
It is false because LVGL transforms pointer input solely from its own display
rotation: `lv_display_rotate_point()` early-returns at `ROTATION_0` and has
exactly one caller, the pointer path in `lv_indev.c`. Rotating the plane while
clearing LVGL's rotation gives you a rotated picture and an **unrotated touch
frame**.

**Flipping it to true requires making touch follow the plane first** —
transforming pointer coordinates independently of `lv_display_get_rotation()`.
Do that before touching the flag, not after.

Note also that no `DrmRotationStrategy` branch is acted on in the shipped
configuration: SOFTWARE is unreachable because
`DisplayManager::try_drm_to_fbdev_fallback` swaps in fbdev first, and HARDWARE is
unreachable because of the gate above.

### Where to verify it

**Not on the Pi 5's DSI panel.** Its plane mask is `0x0`, so it takes the fbdev
path regardless of what the code does — **it passes whether or not the fix is
present.** Reaching the plane path there needs the HDMI connector.

**Use the Pi 3B** (`192.168.1.163` wired). Its DSI panel sits on `vc4`, plane
mask `0x35`, and it is the only board in the fleet with a rotation-capable plane,
a connected panel, and working EGL at once. It has no HelixScreen install yet —
that is a prerequisite, not a task.

The reproduction that has never been run: **`rotate: 180` and confirm the picture
is actually inverted**, then confirm touch lands where the picture says it should.

---

## Task 4 — close the drift gate's blind side

`#if defined(HELIX_ENABLE_OPENGLES) && !LV_LINUX_DRM_USE_EGL` catches a request
without a result. It does **not** catch the inverse: setting `LV_USE_OPENGLES 1`
without the request is silent, and the compiler's "redefined" warning that used
to cover that direction no longer fires now that both headers agree on the token.

---

## Open decisions this phase owns

1. **Does `#1582`'s EGL context getters get wired, or dropped?**
   `patches/lvgl-drm-egl-getters.patch` stops being dead the moment the rung
   exists. It is a memory optimisation, not a correctness fix. If the ladder is
   abandoned, delete the patch instead.
2. **Per-board default** — which targets get `ENABLE_OPENGLES=yes` in
   `mk/cross.mk`, versus probe-only opt-in.
3. **Does a failed probe on a board we said yes to log loudly, or silently take
   the next rung?** Silent is friendlier and hides a regression.

---

## Verification

| Claim | How |
|---|---|
| EGL rung selected on a capable board | Pi 5: launcher picks `helix-screen-egl`, log says GPU-accelerated |
| Probe rejects a software renderer | force llvmpipe; probe must exit non-zero |
| Probe rejects a broken GL userspace | CB1 with the old `/opt/panfrost` Mesa on the second SD card |
| No demotion past the middle rung | probe fails → DRM dumb buffers, **not** fbdev |
| Mode override regression understood | which boards set a `drm_mode` override? EGL cannot honour it |
| Rotation no longer double-applies | Pi 3B, `rotate: 180`: picture actually inverted |
| Touch follows the plane | Pi 3B, rotated: taps land where the picture shows them |
| U1 unaffected | unchanged behaviour, no probe attempted |

`tools/drm_gpu_probe.c` answers plane masks and renderer strings per board in one
pass. Build it with `-DNO_EGL` for a board with no GL userspace, such as the U1.
