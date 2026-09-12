# GPU Acceleration

What the GPU can and cannot do for HelixScreen, measured on real boards rather
than argued from source. Read this before proposing work on the rendering path.

**HelixScreen rasterizes on the CPU on every target.** What differs per board
is how those pixels reach the screen: the `pi` target ships a GPU presentation
binary alongside the software one and picks between them at boot, everything
else presents through DRM dumb buffers or fbdev. The nanovg draw unit, which
would move rasterization itself onto the GPU, is unusable upstream. The
sections below say how much each is worth and what blocks it.

---

## The four rungs

LVGL 9.5 offers four ways to put pixels on a DRM screen. They are not
alternatives — each rung builds on the one below.

| Rung | What it does | State here |
|------|--------------|-----------|
| `lv_draw_sw` into a dumb buffer | CPU rasterizes, kernel scans out | **ships** |
| `lv_draw_sw` into a GBM/EGL surface | CPU rasterizes, GPU composites and presents | **ships on `pi`**, probe-gated |

The EGL rung's CPU numbers below were first recorded against a build that was
rendering incorrectly - see "The alpha trap" - so treat any measurement of this
path as provisional until someone has looked at the panel.
| `lv_draw_nanovg` | GPU rasterizes widgets | **broken upstream**, see below |
| `lv_draw_opengles` | GPU rasterizes, different unit | not evaluated |

The config chain matters and is easy to get wrong: `LV_USE_OPENGLES` is the
macro that counts. `lv_conf_internal.h` derives `LV_LINUX_DRM_USE_EGL` from it
and `LV_USE_EGL` from that in turn, so setting `LV_LINUX_DRM_USE_EGL` directly
does nothing — the derive overwrites it. In this tree `LV_USE_OPENGLES` comes
from `HELIX_ENABLE_OPENGLES`, which `mk/cross.mk` defines for
`ENABLE_OPENGLES=yes`. Resolve these with the preprocessor, never by reading
`lv_conf.h`.

---

## Measured: what EGL is worth

Same binary, same panel, same scene; only `ENABLE_OPENGLES` differs. Measured
2026-09-11.

| | Pi 5 (V3D, 4 GB) | CB1 (Mali-G31, 969 MB) |
|---|---|---|
| CPU, software | 39.7% | 87% |
| CPU, EGL | 24.7% | 78.9% |
| CPU delta | **−38%** | **−9%** |
| Throughput | **+24%** | **+91%** |
| RSS cost | +16 MB | **+70 MB** |

Two things to carry forward:

**The CB1 gains the most throughput and the least CPU relief, at the highest
memory cost.** It is a 969 MB board. How much of that +70 MB is a cost the
board cannot get back is the next section, and the answer decides what can be
done about it.

**nanovg's remaining headroom is small.** Profiling the EGL build shows roughly
31% of its remaining 24.7% CPU is software rasterization — so a perfect GPU
draw unit could recover about **7.7 CPU points**, against the ~15 EGL already
delivers. nanovg was assumed to be the bigger prize; it is not, and it is not
close.

### What the RSS number is made of

Same method on both boards, measured 2026-09-12: force the DRM rung through a
systemd drop-in, restart, idle 100 s at the home panel, read
`/proc/<pid>/status` and `/proc/<pid>/smaps`; remove the drop-in and repeat on
the EGL rung. Figures are MiB.

| | Pi 5 (4 GB, V3D) | Pi 3B (856 MB, vc4) |
|---|---|---|
| VmRSS, DRM rung | 105.7 | 51.2 |
| VmRSS, EGL rung | 122.1 | 117.0 |
| **VmRSS delta** | **+16.4** | **+65.8** |
| RssAnon delta | +11.3 | +20.5 |
| RssFile delta | +5.1 | +45.3 |

The two boards differ by 4x on the headline and agree closely on the part that
cannot be reclaimed. The gap is Mesa's text, and which binary faults it in.

The EGL process on both boards holds `libLLVM` resident (39.8 MiB on the Pi 5,
38.5 on the Pi 3B) plus `libgallium` (9.7 / 10.8). That is roughly 50 MiB of
shared, file-backed library text.

**The base DRM binary links the same three GL libraries** - `ENABLE_GLES_3D=yes`
builds the 3D gcode viewer into it, so `libEGL`, `libGLESv2` and `libgbm` are on
both binaries' `ldd` output, and the two files differ by 56 bytes. Whether it
pays Mesa's residency therefore depends only on whether GL initialises. On the
Pi 5 it does, unprompted, within 100 s of boot: the DRM process already holds
`libLLVM` resident and maps `/dev/dri/card1`. On the Pi 3B it does not, with
zero `libLLVM` mappings.

So the Pi 5's +16 MB is not a cheaper GPU path. It is a board that had already
paid for Mesa on the rung below. **The durable cost of the EGL rung is the
anonymous delta, +11 to +21 MiB.** The rest is shared library text, which the
kernel evicts under pressure and which the DRM rung pays too the moment anything
initialises GL.

Reproduce with `tools/drm_gpu_probe.c` (build instructions in its header) for
plane masks and renderer strings, then an A/B of `ENABLE_OPENGLES=no|yes`.

---

## How a board gets the EGL rung

Three binaries, selected by `scripts/helix-launcher.sh#select_binary` at boot:

```
helix-screen-egl     GPU presentation      taken only if --probe-egl exits 0
helix-screen         DRM dumb buffers      the default
helix-screen-fbdev   /dev/fb0              when the DRM binary's libs are missing
```

Which of LVGL's two DRM drivers a binary carries is a compile-time choice, so
this is a choice of binary and not a runtime mode. `mk/egl-link.mk` builds the
third one from the DRM build's objects, recompiling only the translation units
that read the EGL config macros — `src/api/display_backend_drm.cpp` and LVGL's
own DRM and OpenGL ES drivers.

That sharing is measured, not assumed. Building the `pi` target twice, once per
`ENABLE_OPENGLES` value, and comparing all 1465 objects: **811 differ as raw
bytes, but only 14 differ once debug info is stripped**, and one of those is
`libhv`'s `htime.o`, which embeds `__DATE__`/`__TIME__` and so differs between
any two builds. The remaining 13:

| Object | Belongs to |
|---|---|
| `api/display_backend_drm.o` | the app — the only file under `src/` or `include/` that reads an EGL config macro |
| `lvgl/.../drm/lv_linux_drm.o`, `lv_linux_drm_egl.o` | which DRM back end compiles at all |
| `lvgl/.../opengles/` ×9 | empty translation units without the config |
| `display/display_backend_drm.o` | `libhelix-display.a`, which only the splash and watchdog link — not part of this rung |

The raw-byte number is the trap here: comparing objects without stripping says
811 files changed and implies the EGL binary needs a full second build. It
does not. Strip the debug info and the answer is 12 objects in the main binary.

**Selection is a probe, not a crash.** A failed EGL init inside one binary
would fall through to fbdev in-process and skip the middle rung entirely,
silently demoting a board from dumb buffers to `/dev/fb0`. So the probe is a
separate process: `helix-screen-egl --probe-egl` does the bring-up
(`gbm_create_device`, `eglInitialize`, `eglChooseConfig`, `eglCreateContext`),
prints what answered, and exits 0 or non-zero. A bring-up that aborts inside
the driver takes down only the probe. A board whose probe declines runs the DRM
binary; it never drops two rungs at once.

**The probe refuses a software renderer.** The CB1's original failure was a
stale Mesa in `/opt/panfrost` missing `kms_swrast`/`swrast`; the inverse —
succeeding into llvmpipe — is worse than failing, because every pixel is still
rasterized by the CPU and the handoff to a GPU that is not there costs extra on
top. `gl_renderer_is_software()` in `include/gcode_gl_fallback.h` is the
predicate. It is deliberately a different question from
`gl_renderer_is_denylisted()` beside it: that one names hardware whose driver
faults during 3D draws, and Panfrost is on it while still presenting through
EGL correctly.

**Verifying the binary is the one you think it is.** A build that links the base
objects into the EGL binary produces something that runs fine and presents
through dumb buffers, while the launcher believes it is on the GPU. There is no
other symptom, so `make verify-egl` checks for `lv_opengles_init` — defined only
inside `#if LV_USE_OPENGLES` — in the EGL binary and asserts its absence from
the base one. The trap it guards is real: `lv_opengles_shader.c` holds its GLSL
in C++11 raw string literals, so it compiles as C++ when the config is on and as
plain C when it is off, and the stock rules put both at the same object path.

**Overrides.** `HELIX_DISPLAY_BACKEND=egl` skips the probe and forces the rung;
`=drm` and `=fbdev` force the rungs below it. `HELIX_DRM_DEVICE` pins the node
the probe opens, exactly as it pins the one the display backend opens.

---

## Board capability

Measured per board, not inferred from the SoC.

| Board | Renderer | Rotation-capable plane | Notes |
|-------|----------|------------------------|-------|
| Pi 5 | V3D | no (DSI panel reports mask `0x0`) | a `0x0` mask passes any rotation test vacuously — never verify rotation here |
| Pi 3B | vc4 | **yes** (mask `0x35`) | the only board with a rotation-capable plane, a connected panel, and working EGL at once |
| CB1 | Mali-G31 (Panfrost) | not measured | needs Mesa 25.x; the vendor Mesa 21.3.9 in `/opt/panfrost` fails `gbm_create_device` for want of `kms_swrast`/`swrast` |

The CB1's `gbm_create_device` failure was a stale userspace Mesa, not a hardware
limit. On current Armbian it reports `GL_RENDERER = Mali-G31 (Panfrost)`.

---

## What EGL costs you

Selecting the EGL driver compiles the dumb-buffer driver to nothing, so symbols
that live in it disappear. Three of the six `lv_linux_drm_*` entry points this
app calls vanish; the one that matters in practice is
`lv_linux_drm_set_preferred_mode()`.

**Forced mode selection is unavailable on the EGL path.** The connector's own
preferred mode is what you get. `src/api/display_backend_drm.cpp` guards the
call and warns rather than failing to link.

No shipped per-board config sets a mode override, so this reaches only someone
who picked a resolution by hand. On the EGL rung that choice is logged and
ignored, and the panel comes up at its preferred mode instead. Whether a
configured resolution should make the launcher decline the rung is a per-board
policy question, not a probe question — the probe cannot see the config.

---

## nanovg: why it is unusable

Three independent defects, all upstream in LVGL 9.5. The first alone is
disqualifying.

### 1. It corrupts rendering whenever more than one layer is alive

nanovg is a deferred renderer: work between `nvgBeginFrame` and `nvgEndFrame`
accumulates in a batch submitted against whatever framebuffer is bound **at
flush time**. `lib/lvgl/src/draw/nanovg/lv_draw_nanovg.c` rebinds
and clears the framebuffer on a layer change without ending the frame first,
and `glViewport`/`nvgBeginFrame` are gated behind `is_started`, so an open batch
keeps the previous layer's projection and viewport. `lv_draw_dispatch`
round-robins layers while nanovg executes one task per dispatch, so with two
layers it hops between them constantly, re-clearing each time.

The trigger in this app is `src/ui/setting_group.cpp#setting_group_xml_create`
setting `clip_corner`; with a non-zero theme radius, `lv_refr.c` allocates two
ARGB8888 band layers per card per refresh. The home panel has no `clip_corner`
container and renders perfectly; the settings panel loses every background fill
and the nav rail.

Confirmed by setting the theme radius to 0, which makes LVGL skip the band
layers (`if(radius == 0) clip_corner = false;`) — the settings panel then
renders correctly under nanovg.

### 2. The FBO cache collides sibling layers

`lv_nanovg_fbo_cache.c` keys on `(width, height, flags, format)` with a capacity
of 4. A card's top and bottom clip bands have the same width and the same
height, so they hash to one entry and share a framebuffer; the second band's
`glClear` wipes the first.

### 3. The glyph path assumes a tight stride

`lv_nanovg_reshape_global_image()` allocates a deliberately tight stride —
correct for its image-cache caller, which does a stride-aware copy, and for
`nvgCreateImage()`, which takes no stride and reads `w * h` contiguous bytes.
But the label caller hands that buffer to `lv_font_get_glyph_bitmap()`, which
ignores the buffer's declared stride and writes rows at
`lv_draw_buf_width_to_stride(box_w, A8)`.

With `LV_DRAW_BUF_STRIDE_ALIGN 1` those are the same number and nothing happens.
Above 1 it is a heap overflow on every glyph — ASAN reports a write past a
`w * h` region allocated by `lv_nanovg_reshape_global_image`, and glibc aborts
with `malloc(): invalid size` about 1.5 s in.

Note the trap: padding the *height* silences ASAN without fixing anything,
because the buffer is a shared high-water-mark allocation that later glyphs hide
inside. The text still renders sheared, because the two consumers disagree about
pitch. A real fix has to allocate at the font engine's pitch and repack to tight
before upload.

### Also worth knowing

- **Gradient fills silently draw nothing** unless `LV_USE_VECTOR_GRAPHIC` is on,
  which it is not. One `LV_LOG_WARN` and an empty rect.
- **Anything that falls back to the software unit is invisible** on this path:
  `lv_linux_drm_egl.c`'s flush callback ignores `px_map` entirely, so SW output
  is rasterized into a buffer nobody presents.

---

## The alpha trap, and why nothing automated caught it

**LVGL's 32bpp native format is `XRGB8888`, and it leaves the X byte at
`0x00`.** That byte is don't-care by contract, so LVGL writes `0xFF` there on
full-word draw paths and leaves it alone otherwise.

The EGL path gives it meaning. `lv_linux_drm_egl.c` uploads the buffer as
`GL_RGBA`, so X arrives as alpha, and the fragment shader in
`lv_opengles_shader.c` computes `texColor.rgb * combinedAlpha`. Every pixel LVGL
did not leave fully opaque is multiplied to black. Measured on a Pi 3B: 4.9% of
pixels carried `X=0x00`, and the light-blue nav icon read
`R=58 G=124 B=200 X=0`. On screen that is icons, borders, shading and
antialiased text rendering black while flat fills look perfect.

`DisplayBackendDRM::create_display` forces `ARGB8888` on the EGL path so LVGL
maintains the byte. `DisplayBackendFbdev::init` already did the same thing for
the AD5M, whose LCD controller read that byte as alpha and produced a magenta
ghost. Same defect, two different consumers. fbdev, DRM dumb buffers and SDL are
all immune because they ignore the fourth byte of XRGB.

**This is the part worth remembering.** Every automated signal said the broken
build was healthy:

| Check | What it said |
|---|---|
| Process liveness | running, stable |
| CPU | down 38%, the measured win |
| `ctl ping` / `ctl current` | responsive |
| GL errors (`LV_USE_OPENGLES_DEBUG` is 1) | none |
| `ctl screenshot` | **clean** - `lv_snapshot_take()` re-renders the widget tree and never reads the presented buffer |

The screenshot is the trap: it looks like proof and is not, because the fault is
downstream of where it samples. The only detector was a person looking at the
panel. **A visual check is therefore mandatory when changing this rung, not
advisory** - and to inspect the presented buffer rather than a re-render, dump
`ctx->texture.fb1` from the flush callback and read the bytes.

---

## The stride invariant

**`LV_DRAW_BUF_STRIDE_ALIGN` must stay 1.** This is load-bearing and easy to
"fix" wrongly.

LVGL's software blenders, NEON paths included, address pixels with byte-element
loads and stores (`vst3q_u8`/`vld3q_u8` for 3-byte pixels), whose alignment
requirement is one byte. The 4-byte paths use `vld1q_u32` on buffers whose
stride is `w * 4` and therefore already aligned. What SIMD and DMA actually care
about is the **start address**, and `LV_DRAW_BUF_ALIGN` covers that — it is 16
here, and upstream's own NXP porting guide pairs a 64-byte `LV_DRAW_BUF_ALIGN`
with a stride align of **1**.

Raising it breaks every consumer that reads a buffer as tightly packed. Known
casualties: SDL texture uploads (`SDL_UpdateTexture` given `w * 4`), the
backdrop blur pipeline, the fbdev buffer path, the nanovg glyph upload, and
`esp_lcd_panel_draw_bitmap` on the ESP32 port. Each surfaced as a different
symptom — bus error, diagonal shear, smearing, trapezoid artifacts, heap
overflow — which is what made the shared cause hard to see.

If you are here because of a NEON crash: check the signal first. An unaligned
access raises `SIGBUS` with `BUS_ADRALN`. A `SIGSEGV`/`SEGV_ACCERR` in a blender
is an out-of-bounds write, and the fix is bounds clipping —
`patches/lvgl_blend_buf_bounds_clip.patch` clips the blend area to the layer's
buffer, which is what `lv_draw_sw_blend` was missing.

**Code that consumes a draw buffer must read `header.stride`**, never recompute
a pitch from width. `src/application/color_transform.cpp#apply` takes the stride
as a parameter for exactly this reason.

---

## Verification recipe

Anything claimed here is reproducible. The pattern that works:

1. Build variants into separate object trees with `BUILD_SUBDIR`, one
   `ENABLE_OPENGLES`/`ENABLE_NANOVG` combination each. Changing `lv_conf.h`
   invalidates every object, so expect a cold ccache per config.
2. Deploy and drive with `helix-screen ctl` over a pinned socket. `ctl` works on
   dev cross-builds; a packaged release has no server (`HELIX_PACKAGING=1`
   forces `ENABLE_REMOTE_CONTROL=no`).
3. **Take a screenshot and look at it.** A process that stays alive and answers
   `ping` can still be rendering garbage — that is exactly how the nanovg glyph
   bug presents once its crash is papered over.
4. **Run a control.** A config change that produces a byte-identical screenshot
   did not take effect. Two of the experiments behind this document were no-ops
   on the first attempt — one edited a theme file shadowed by a user copy — and
   both would have read as evidence without a control run.
