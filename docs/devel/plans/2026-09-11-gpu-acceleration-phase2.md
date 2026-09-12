# GPU Acceleration Phase 2 — ship the EGL rung, move rotation ownership

**Status:** Tasks 1 and 4 done on `feature/gpu-egl-rung` (unpushed). Tasks 2 and
3 remain. Phase 1 shipped as `aa8d86153`.

Task 1 is verified on hardware, visually included - which took a second pass.
The launcher probes, logs `EGL probe: /dev/dri/card1: V3D 7.1.7.0`, selects
`helix-screen-egl`, and the app reports `GPU-accelerated display active
(EGL/OpenGL ES)` on the Pi 5.

The first pass called that "verified" on those logs plus a CPU number, and the
panel was in fact rendering every icon, border and antialiased edge black. The
cause was LVGL's XRGB8888 padding byte arriving as alpha on the GL upload; see
`GPU_ACCELERATION.md` § "The alpha trap". Nothing automated caught it, and one
of the signals - `ctl screenshot` - actively misleads, because `lv_snapshot_take`
re-renders the widget tree instead of reading the buffer that gets presented. The probe
picks the scanout node on both owned boards even though their numbering is
opposite - Pi 5 `card1` (`drm-rp1-dsi`, V3D) and CB1 `card0` (`sun4i-drm`,
Mali-G31 (Panfrost)) - because it requires a connected connector rather than a
node index. Probing a render node would answer "the GPU works" without proving
the presentation path.
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

## Task 2 — per-board policy  ✅ decided: no memory gate (2026-09-12)

The task asked for a per-target decision plus a memory headroom check. Neither
lever exists in the shape assumed.

**There is one aarch64 SBC artifact, not two.** `install.sh#detect_platform`
resolves every 64-bit Debian-family ARM SBC to the platform key `pi` - Raspberry
Pi, BTT CB1, MKS, QIDI, Armbian alike - and `get_download_platform` maps that to
`helixscreen-pi.zip`. A CB1 installs the same package as a Pi 5 and receives the
same `helix-screen-egl`. `ENABLE_OPENGLES` and `ENABLE_EGL_RUNG` in `mk/cross.mk`
cannot separate the two boards because they are the same target.

**A MemTotal gate cannot separate them either.** The Pi 3B is 856 MB, smaller
than the CB1's 969 MB. It pays the larger RSS delta of the two measured boards
and has the least CPU in the fleet to spare. Any threshold that denies the CB1
denies the Pi 3B first, and the Pi 3B is the board where the rung earns most.

**The cost such a gate would defend against is a fifth of the headline.** The
+70 MB is RSS. Roughly 46 MiB of it is shared `libLLVM` and `libgallium` text
that the DRM rung pays too as soon as GL initialises, which on the Pi 5 it
already has, unprompted, at boot. The non-reclaimable anonymous delta is
+11 MiB on the Pi 5 and +21 MiB on the Pi 3B. Numbers and method:
`GPU_ACCELERATION.md` § "What the RSS number is made of".

**Decision: the probe stays the only gate.** `HELIX_DISPLAY_BACKEND` is the
per-install override, and `config/helixscreen.env` and `config/helixscreen.service`
now name `drm` as the way to decline the rung. A board that cannot afford GPU
presentation is one whose owner says so, not one this code guesses at from
MemTotal.

**What reopens this:** the CB1's anonymous/file split is unmeasured, and the CB1
is the only owned board on a different GPU family. Panfrost may hold GPU buffers
as anonymous or shmem pages where vc4 and V3D hold file-backed text. If the
CB1's anonymous delta lands near its +70 MB RSS rather than near +21 MiB, the
reasoning above loses its basis and a gate is back on the table. Measuring it
means deploying a dev build to the AFC rig.

---

## Task 3 — rotation and touch ownership

Two coupled problems. Step one of the second is done; the rest has not moved
since Phase 1.

### Step 1 - an applied-rotation source that is not LVGL's  ✅ done (2026-09-12)

`DisplayBackend::applied_rotation_degrees()` is now the answer to "what angle is
the picture presented at", and `display_rotation_degrees()` asks the live backend
instead of reading `lv_display_get_rotation()` itself. The default implementation
returns exactly what LVGL returns, so every board behaves as it did. What changed
is that a backend handing the angle to a scanout plane has one place to say so,
rather than five readers inferring it from LVGL.

`DisplayBackend` tracks the live instance in its own constructor and destructor,
so the DRM→fbdev swap and `create_auto()`'s discarded candidates stay correct
with nothing to remember at the seven `m_backend` assignment sites.

`DisplayManager::is_software_rotated()` was a fifth reader calling
`lv_display_get_rotation()` directly, outside what
`scripts/check_touch_rotation_source.py` guards. It asks its own backend now.

`tests/unit/test_display_rotation_source.cpp` pins the seam: a backend reporting
an angle LVGL is not reporting wins; the answer disappears with the backend
rather than lingering as a cached value; and a backend overriding nothing answers
exactly as LVGL does. Deleting the delegation turns two of the three red.

**Not done, and the order still matters:** nothing rotates touch yet.
`plane_may_own_rotation()` still returns false and must stay false until a
backend that overrides `applied_rotation_degrees()` also transforms pointer
coordinates. The seam makes that change land in one place; it does not make it.

**An alternative worth knowing about:** LVGL has
`lv_display_set_matrix_rotation()`, which rotates through the draw matrix instead
of a post-render software pass, and would keep `lv_display_get_rotation()` honest
without a plane at all. It is gated on `LV_DRAW_TRANSFORM_USE_MATRIX`, which the
preprocessor resolves to **0** in this tree, so it is unavailable today. On the
EGL rung that matrix would cost close to nothing. Enabling it is its own change,
with its own risk, and it has not been measured.

**Ownership only half-moved.** `DisplayManager` still calls
`lv_display_set_rotation()` itself *and* calls the backend, which may clear it —
so the value is written twice. The backend call comes second in all four
DisplayManager sites (`init`, `apply_rotation`, and `run_rotation_probe` twice),
so the backend wins on ordering. `DisplayBackendFbdev`'s override is a no-op
that relies on DisplayManager having set it. `#1275` and `#1580` land together:
ownership moves to the backend, DisplayManager hands the angle down, and each
backend decides whether LVGL is also told. That is already how fbdev and SDL
behave.

`scripts/check_rotation_cache_order.py` and
`tests/shell/test_rotation_cache_order_gate.bats` pin the order of
`set_display_rotation()` against the `m_width`/`m_height` cache read in those
four functions, and fail on purpose when the pair is extracted into a helper.
Collapsing the two calls into one changes the shape that gate matches, so it
moves in the same commit or the commit hook refuses.

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
unreachable because of the gate above. It is broader than the strategy switch —
`DisplayBackendDRM::set_display_rotation` is unreachable *in its entirety*.
DisplayManager calls it only for non-zero angles, and the one caller that would
pass `ROTATION_0`, the rotation probe, returns early on DRM builds. So the
double-write hazard above is currently masked rather than absent: on DRM the
second write is always the fbdev no-op.

**The coupling that will bite, and it is not in the rendering path.**
`display_backend.h#display_rotation_degrees` answers "is the display rotated
right now?" by reading `lv_display_get_rotation()`, and
`scripts/check_touch_rotation_source.py` forces the backends to use it rather
than the config key. Give the plane the rotation and clear LVGL's, and that
helper returns 0 on a physically rotated display — flipping four decisions that
have nothing to do with rendering:

- both `create_input_pointer` implementations stop ignoring a stored evdev touch
  range and start programming one solved through a rotation, which `#1394`
  established double-applies
- `touch_calibration_wrapper.cpp` stops discarding an unstamped affine, so a
  matrix solved at an unknown rotation is accepted as if solved at 0
- `touch_calibration_panel.cpp` re-enables the range-fit stage on a rotated
  panel, the exact thing `#1394` turned off
- `DisplayManager::is_software_rotated` returns false, turning animations back
  on for a board deliberately opted out (`#986`)

Two of those write to `settings.json`. The failure shows up one boot later as a
stored range or affine in the wrong basis, and it **survives a revert of the
code that caused it**. So the touch pipeline needs a rotation source
independent of `lv_display_get_rotation()` *before* anything else moves — that
is step one, not a cleanup afterwards.

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

## Task 4 — close the drift gate's blind side  ✅ done (`cc5993366`)

`#if defined(HELIX_ENABLE_OPENGLES) && !LV_LINUX_DRM_USE_EGL` catches a request
without a result. It does **not** catch the inverse: setting `LV_USE_OPENGLES 1`
without the request is silent, and the compiler's "redefined" warning that used
to cover that direction no longer fires now that both headers agree on the token.

---

## Open decisions this phase owns

1. **`#1582` cannot be closed by deleting the patch.** Its premise is that
   `patches/lvgl-drm-egl-getters.patch` is dead. The three EGL *context* getters
   are indeed uncalled, but the same patch also carries
   `lv_linux_drm_get_fd`, which `display_backend_drm.cpp` calls twice for
   connector DPMS (`#1049`) and which the EGL variant of that object has a real
   undefined reference to. Deleting the patch breaks the EGL link and takes
   panel power with it.

   Removing only the three dead getters means surgery on two patches:
   `lvgl-drm-flush-rotation.patch` declares them in `lv_linux_drm.h` and
   provides the non-EGL `get_fd`, while this patch provides the EGL bodies. That
   is ~24 lines of dead code inside otherwise load-bearing patches. Retitle the
   issue or accept the getters as the cost of the fd.
2. ~~**Per-board default** - which targets get `ENABLE_OPENGLES=yes` in
   `mk/cross.mk`, versus probe-only opt-in.~~ **Decided: probe-only, no memory
   gate.** Per-target is not a lever here; one aarch64 artifact serves the whole
   SBC fleet. See Task 2.
3. ~~**Does a failed probe log loudly, or silently take the next rung?**~~
   **Decided: loudly.** `select_binary` logs the probe's own verdict line and,
   on a refusal, `EGL unavailable here - using DRM dumb buffers`. Both go to
   stderr, because the function's stdout is the chosen binary path.

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
