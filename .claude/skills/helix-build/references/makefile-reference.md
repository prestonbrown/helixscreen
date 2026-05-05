# Makefile Reference

## Key Variables

### Compiler & Flags
| Variable | Default | Notes |
|----------|---------|-------|
| `CC` | clang > gcc | Auto-detect, with stdlib test for clang |
| `CXX` | clang++ > g++ | Auto-fallback on broken libstdc++ |
| `OPT` | 2 | Optimization: 0 (dev), 1, 2 (release) |
| `CXXFLAGS` | `-std=c++17 -Wall -Wextra -O{OPT} -g` | Extended per-platform |
| `SUBMODULE_CFLAGS` | `-std=c11 -O2 -g -w` | Third-party code: no warnings |
| `DEPFLAGS` | `-MMD -MP` | Header dependency tracking |

### Version
Read from `VERSION.txt` (MAJOR.MINOR.PATCH):
- `HELIX_VERSION`, `HELIX_VERSION_MAJOR/MINOR/PATCH`
- `HELIX_GIT_HASH` — short git hash
- Injected via `-DHELIX_VERSION="..."` etc.

### Build Directories
| Variable | Default | Override |
|----------|---------|----------|
| `BUILD_DIR` | `build` | `mk/cross.mk` sets per-platform |
| `BIN_DIR` | `build/bin` | Per-platform: `build/pi/bin` |
| `OBJ_DIR` | `build/obj` | Per-platform: `build/pi/obj` |
| `BUILD_SUBDIR` | (none) | Platform name: `pi`, `ad5m`, `k1` |

### Feature Gates
| Variable | Default | Purpose |
|----------|---------|---------|
| `ENABLE_SDL` | yes (native) | SDL2 desktop display |
| `ENABLE_OPENGLES` | per-target | EGL/GLES GPU rendering |
| `ENABLE_GLES_3D` | yes (Linux) | 3D gcode rendering |
| `ENABLE_SCREENSAVER` | yes (desktop/Pi) | Flying toasters |
| `ENABLE_MOCKS` | yes | Mock backends for dev |
| `ENABLE_SSL` | per-target | OpenSSL for HTTPS/WSS |
| `HELIX_HAS_LABEL_PRINTER` | 1 | Label printer feature |
| `HELIX_HAS_CFS` | 1 | CFS feature |
| `HELIX_HAS_IFS` | 1 | IFS feature |

## Linker Flags by Platform

### macOS
```
-lSDL2 -lhv -lz -lm -lpthread -liconv
-framework Foundation -framework CoreWLAN -framework CoreLocation ...
```

### Linux Native
```
-lSDL2 -lhv -lwpa_client -lusb-1.0 -lssl -lcrypto -ldl -lstdc++fs -lGLESv2
```

### Pi (aarch64, DRM)
```
-L/usr/lib/aarch64-linux-gnu -lhv -lwpa_client -lnl-genl-3 -lnl-3
-ldrm -linput -lEGL -lGLESv2 -lgbm -lusb-1.0 -lssl -lcrypto -lasound ...
```

### K1 Static (MIPS, musl)
```
-lhv -lwpa_client -lnl-genl-3 -lnl-3 -latomic -ldl -lz -lm -lpthread
# Fully static binary
```

### K1 Dynamic (MIPS, glibc)
```
-Wl,-Bstatic -lhv -lwpa_client -lnl-genl-3 -lnl-3 -lstdc++fs
-Wl,-Bdynamic -lstdc++ -lz -lm -lpthread -lrt -ldl -latomic -lgcc_s
```

### K2 (ARM, musl)
```
-lhv -lwpa_client -lnl-genl-3 -lnl-3 -ldl -lz -lm -lpthread
```

### Yocto
```
# Preserves bitbake LDFLAGS, appends project libs
-lhv -lwpa_client -lnl-genl-3 -lnl-3 -ldl -lz -lm -lpthread
```

## Source File Organization

### Application Sources
- `src/*.cpp`, `src/*/*.cpp`, `src/*/*/*.cpp` — main app
- `src/*.mm` — macOS Objective-C++ (WiFi)
- `src/tools/*.cpp` — standalone tools (separate build rules)
- `src/bluetooth/*.cpp` — shared library (separate)

### Excluded from main binary
- `test_*.cpp` — test files
- `helix_splash.cpp` / `helix_watchdog.cpp` — separate binaries
- `lvgl-demo/main.cpp` — LVGL demo

### Libraries
- `lib/lvgl/` — LVGL 9 (exclude XML/expat sources → `lib/helix-xml/`)
- `lib/helix-xml/` — Extracted XML engine
- `lib/lv_markdown/` — Markdown widget
- `lib/quirc/` — QR code decoder
- `lib/cpp-terminal/` — TUI library
- LVGL thorvg `.cpp` files compiled separately

## Key Targets

### Build
| Target | Description |
|--------|-------------|
| `all` | Default: build main binary with dep checks |
| `build` | Clean parallel build with timing |
| `dev` | `OPT=0 -j` fast dev build |
| `strict` | Build with `-Werror` |
| `clean` | Remove all build artifacts |
| `install` | Stage to `DESTDIR/opt/helixscreen/` |

### Code Quality
| Target | Description |
|--------|-------------|
| `format` | clang-format + xmllint all files |
| `format-staged` | Format only staged files |
| `quality` | Run `scripts/quality-checks.sh` |
| `check-deps` | Verify build dependencies |
| `install-deps` | Interactive auto-install |

### Assets
| Target | Description |
|--------|-------------|
| `regen-fonts` | Regenerate MDI icon fonts |
| `generate-fonts` | Explicit font generation |
| `validate-fonts` | Verify icons in compiled fonts |
| `icon` | Generate app icon from logo |
| `apply-patches` | Apply submodule patches |
| `reset-patches` | Revert submodule patches |

### Tools
| Target | Description |
|--------|-------------|
| `compile_commands` | Merge .ccj fragments (~1-2s) |
| `compile_commands_full` | Full regeneration (slow) |
| `moonraker-inspector` | Build Moonraker query tool |
| `tools` | Build diagnostic tools |

### Symbols & Strip (cross-compile only)
| Target | Description |
|--------|-------------|
| `symbols` | Extract `.sym` + `.debug` files |
| `strip` | Strip binaries for size |

### Debug Helpers
| Target | Description |
|--------|-------------|
| `print-ldflags` | Print computed LDFLAGS |
| `print-target-cflags` | Print TARGET_CFLAGS |
| `print-cxxflags` | Print CXXFLAGS |
| `print-strip` | Print STRIP_BINARY setting |
