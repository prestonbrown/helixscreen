# GPU Acceleration

What the GPU can and cannot do for HelixScreen, measured on real boards rather
than argued from source. Read this before proposing work on the rendering path.

**Today HelixScreen ships software rendering on every target.** That is a
deliberate position, not an unfinished one: the EGL presentation path is real
and measurably faster, and the nanovg draw unit is unusable upstream. The
sections below say how much each is worth and what blocks it.

---

## The four rungs

LVGL 9.5 offers four ways to put pixels on a DRM screen. They are not
alternatives — each rung builds on the one below.

| Rung | What it does | State here |
|------|--------------|-----------|
| `lv_draw_sw` into a dumb buffer | CPU rasterizes, kernel scans out | **ships** |
| `lv_draw_sw` into a GBM/EGL surface | CPU rasterizes, GPU composites and presents | **measured, works** |
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
memory cost.** It is a 969 MB board; +70 MB is not free there. Any ladder that
turns EGL on everywhere has to answer for that specific square.

**nanovg's remaining headroom is small.** Profiling the EGL build shows roughly
31% of its remaining 24.7% CPU is software rasterization — so a perfect GPU
draw unit could recover about **7.7 CPU points**, against the ~15 EGL already
delivers. nanovg was assumed to be the bigger prize; it is not, and it is not
close.

Reproduce with `tools/drm_gpu_probe.c` (build instructions in its header) for
plane masks and renderer strings, then an A/B of `ENABLE_OPENGLES=no|yes`.

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
