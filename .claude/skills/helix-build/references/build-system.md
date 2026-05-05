# Build System Internals

## Dependency Management

### Dependency Check
```bash
make check-deps              # Full check
SKIP_OPTIONAL_DEPS=1 make check-deps  # Minimal (for cross-compilation)
```

Checks: compiler, cmake, make, python3, npm, clang-format, SDL2, spdlog, libhv, LVGL submodules, canvas libs (cairo, pango), pkg-config.

Platform-aware install commands for macOS (brew), Debian/Ubuntu (apt), Fedora/RHEL (dnf).

### Auto-Install
```bash
make install-deps    # Interactive: detect → list → confirm → install
```

### Library Build Chain

Libraries build automatically when missing, no manual intervention:

1. **libhv** — `./configure --with-http-client && make` → `build/lib/libhv.a` (architecture-isolated)
2. **wpa_supplicant** (Linux only) — `make libwpa_client.a` → `build/lib/libwpa_client.a`
3. **SDL2** — System `sdl2-config` preferred, falls back to CMake submodule build
4. **spdlog** — Header-only, system or submodule
5. **LVGL** — Always submodule (project patches required)

### Library Clean Targets
```bash
make libhv-clean     # Clean libhv
make sdl2-clean      # Clean SDL2 CMake build
make lvgl-clean      # Clean LVGL objects
make libs-clean      # Clean all library artifacts
```

Cross-compilation auto-cleans libhv before each build (prevents architecture mixing).

## Git Submodules

| Submodule | Purpose | Notes |
|-----------|---------|-------|
| `lvgl` | LVGL 9.5 graphics | Always submodule, project patches required |
| `libhv` | HTTP/WebSocket | Auto-built, uses system if available |
| `spdlog` | Logging | Header-only, uses system if available |
| `wpa_supplicant` | WiFi control | Linux only, auto-built |

`lib/helix-xml/` is NOT a submodule — it lives directly in the repo with XML patches baked in.

**Never commit to submodules directly.** Always create patches in `patches/`.

## Patch System

### How It Works
1. Patches stored in `patches/` (repo root)
2. `mk/patches.mk` auto-checks before each build
3. Idempotent — safe to run multiple times
4. `make apply-patches` for manual application
5. `make reset-patches` to revert

### Current Patches
- `lvgl_sdl_window_position.patch` — Multi-display support via env vars (`HELIX_SDL_DISPLAY`, `HELIX_SDL_XPOS`, `HELIX_SDL_YPOS`)
- Multiple LVGL patches for fbdev, DRM, theme, blend, string, slider fixes

### Adding Patches
```bash
cd lib/lvgl
git diff > ../../patches/my-patch.patch
# Update mk/patches.mk to apply in the apply-patches target
# Document in patches/README.md
```

## Font & Icon Generation

### MDI Icon Fonts
- Source: `scripts/regen_mdi_fonts.sh`
- Font: `assets/fonts/materialdesignicons-webfont.ttf`
- Output: `mdi_icons_{16,24,32,48,64}.c`
- Codepoints: `include/ui_icon_codepoints.h`

```bash
make regen-fonts       # Regenerate from regen script
make generate-fonts    # Explicit regeneration
make validate-fonts    # Verify all codepoint icons in fonts
```

### Text Fonts (Noto Sans)
- Source: `package.json` npm scripts
- Output: `noto_sans_*.c`, `noto_sans_bold_*.c`

### Adding New Icons
1. Find icon at https://pictogrammers.com/library/mdi/
2. Add codepoint to `scripts/regen_mdi_fonts.sh`
3. Add to `include/ui_icon_codepoints.h`
4. `make regen-fonts && make -j`

## IDE/LSP Support

### compile_commands.json
Incremental generation — each `.o` generates a `.ccj` fragment, auto-merged after builds:
```bash
make compile_commands       # Merge fragments (~1-2s)
make compile_commands_full  # Full regeneration (slow, only if corrupted)
```

Fragments stored as `.ccj` next to `.o` files in `build/obj/`.

## Build Performance

| Method | Full rebuild (32-core) |
|--------|----------------------|
| `make -j` cold, no ccache | ~4.5 min |
| `make dev` cold | ~2.5 min |
| `make -j` ccache populated | ~38 sec |
| `make dev` ccache | ~20 sec |
| Touch one `.cpp`, rebuild | ~7 sec |

### Performance Tips (impact order)
1. **Install ccache** — biggest win, auto-detected
2. **Use `make dev`** — `-O0`, ~2x faster compilation
3. **Parallel builds** — `make -j` auto-detects cores
4. **Incremental builds** — avoid `make clean`

### High-fanout Headers (changing triggers most recompilation)
| Header | Files affected |
|--------|---------------|
| `theme_manager.h` | ~144 |
| `ui_update_queue.h` | ~144 |
| `moonraker_api.h` | ~122 |
| `app_globals.h` | ~121 |
| `printer_state.h` | ~104 |

## Install Target

For external packaging (buildroot/yocto/debian):
```bash
make install DESTDIR=/tmp/staging PLATFORM_TARGET=ad5m-br
```

Layout: `$(DESTDIR)/opt/helixscreen/{bin/, ui_xml/, assets/{fonts,images,sounds,config}, certs/}`

Runtime state (config, cache, logs) is NOT installed — init scripts create `/data/helixscreen/` on first boot.

## SVG to PNG Conversion

Always use `rsvg-convert` (from `librsvg`), never ImageMagick — ImageMagick's SVG renderer produces corrupted output.

```bash
rsvg-convert input.svg -w 64 -h 64 -o output.png
```
