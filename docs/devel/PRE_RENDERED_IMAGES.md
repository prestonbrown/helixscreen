# Pre-Rendered Image System

HelixScreen pre-renders splash screen images to LVGL binary format (`.lvbin`) at build time for instant display on embedded devices. This eliminates runtime PNG decoding, dramatically improving startup performance.

## Performance Impact

| Mode | FPS on AD5M | Time to Display |
|------|-------------|-----------------|
| PNG decoding (runtime) | ~2 FPS | ~500ms |
| Pre-rendered `.lvbin` | ~116 FPS | ~8ms |

## How It Works

1. **Build time**: The `scripts/regen_images.sh` script converts `helixscreen-logo.png` to LVGL binary format at exact pixel sizes matching each supported screen resolution
2. **Runtime**: `splash_screen.cpp` checks for pre-rendered images and uses them directly if available, falling back to PNG with runtime scaling otherwise

### Pre-rendered Image Sizes

Images are pre-rendered at **exact pixel sizes** matching the splash screen calculations in `splash_screen.cpp`:

Size classes are the `UiBreakpoint` tiers from `include/ui_breakpoint.h`, selected
from the **narrow axis** by `helix::get_splash_3d_size_name()`. The same word means
the same resolution in an asset filename and in a layout override.

The 3D splash composites a full-screen canvas at every class:

| Class | Resolution | Logo Size | Calculation | Devices |
|-------|------------|-----------|-------------|---------|
| micro | 480x272 | 240x240 | 480 × 50% (height < 500) | CC1 |
| tiny | 480x320 | 240x240 | 480 × 50% | Snapmaker U1 |
| small | 480x400 | 240x240 | 480 × 50% | |
| medium | 800x480 | 400x400 | 800 × 50% | K1, K2, AD5M, AD5X |
| large | 1024x600 | 614x614 | 1024 × 60% | |
| xlarge | 1280x720 | 768x768 | 1280 × 60% | |
| ultrawide | 1920x440 | 384x384 | wide and short; its own class, not a tier | |

The 2D logo is centred art rather than a canvas, so it is rendered for a subset -
`tiny`, `medium`, `large`, `xlarge`. A class with no logo falls back to scaling the
PNG, which costs a decode but never fails.

Nothing is composited above `xlarge`: a larger panel clamps to it and scales.

Which classes a platform's package contains is derived from its panel geometry in
`assets/config/platforms.json`; see `scripts/platform_manifest.py`.

### What a release actually ships

Two steps, and both are needed. `gen-splash-3d-<platform>` narrows what gets
**built**, but a release copies whatever `build/` happens to hold, so a tree left
over from another platform's build would ship with it. `release-clean-assets` in
`mk/cross.mk` then calls `platform_manifest.py prune-assets`, which is what bounds
the payload: it drops splash classes the panel cannot select, printer renders at
the other size, and the source PNGs.

Dropping the PNGs is safe because `get_prerendered_printer_path()` degrades
rather than fails - prerendered, then PNG, then `generic-corexy`. The prune keeps
the `generic-corexy` render always, and keeps every PNG if *any* printer lacks a
render at the size being kept, so a missing image can never become no image.

Measured against a full asset tree:

| Platform | Before | After | Saved |
|----------|--------|-------|-------|
| K2 (800x480, 300px art) | 42.2 MB | 8.9 MB | 33.3 MB |
| CC1 (480x272, 150px art) | 42.2 MB | 2.8 MB | 39.3 MB |
| Pi (panel unknown) | 42.2 MB | 42.2 MB | nothing, by design |

### File Format

`.lvbin` files contain:
- 12-byte header (magic, version, dimensions, color format)
- Raw ARGB8888 pixel data (4 bytes per pixel)

Example: `splash-logo-medium.lvbin` = 12 + (400 × 400 × 4) = 640,012 bytes (~625KB)

## Usage

### Generate Images

```bash
# All sizes (for Pi with variable displays)
make gen-images

# AD5M only (800x480 fixed display)
make gen-images-ad5m

# Specific sizes
TARGET_SIZES=medium,large ./scripts/regen_images.sh
```

### Makefile Targets

| Target | Description |
|--------|-------------|
| `gen-images` | Generate all pre-rendered images |
| `gen-images-ad5m` | Generate only 800x480 size for AD5M |
| `gen-images-pi` | Generate all sizes for Pi |
| `clean-images` | Remove generated images |
| `list-images` | Show what would be generated |

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `OUTPUT_DIR` | `build/assets/images/prerendered` | Output directory |
| `TARGET_SIZES` | (all sizes) | Comma-separated classes: `tiny,medium,large,xlarge` |

## Integration with Build System

Pre-rendered images are **build artifacts** (not committed to the repository). They are automatically generated during:

- `make deploy-pi` / `make deploy-pi-*` (generates all sizes)
- `make deploy-ad5m` / `make deploy-ad5m-*` (generates only `medium`)
- `make release-pi` (generates all sizes)
- `make release-ad5m` (generates only `medium`)

### Platform-Specific Generation

The AD5M has a fixed 800×480 display, so only the `medium` class is generated. The Raspberry Pi can have various displays, so all classes are generated.

## Runtime Behavior

`splash_screen.cpp` implements this logic:

```cpp
const char* get_splash_3d_size_name(int screen_width, int screen_height) {
    // A wide, short bar display gets its own class rather than a tier.
    if (screen_width >= 1100 && screen_height < 500)
        return "ultrawide";

    // breakpoint_for() is the project's one resolution ladder, keyed on the
    // narrow axis - which is what a full-screen canvas has to fit into.
    UiBreakpoint bp = breakpoint_for(std::min(screen_width, screen_height));
    if (bp > UiBreakpoint::XLarge)
        bp = UiBreakpoint::XLarge;
    return breakpoint_name(bp);
}
```

The caller then builds `splash-3d-<mode>-<class>.bin`, checks it exists, and
compares `get_splash_3d_target_height()` against the real screen height before
using it. A missing file or an oversized canvas falls back to the PNG.

If no pre-rendered image is found, the splash screen falls back to PNG loading with runtime scaling (slower but works for any screen size).

## Adding New Pre-rendered Images

1. Add the source image to `assets/images/`
2. Update `IMAGES_TO_RENDER` in `scripts/regen_images.sh`:
   ```bash
   IMAGES_TO_RENDER=(
       "assets/images/helixscreen-logo.png:splash-logo:Splash screen logo"
       "assets/images/new-image.png:new-image:Description"
   )
   ```
3. Update the C++ code to use the pre-rendered image

## Troubleshooting

### Images not found at runtime

Check that images exist in the correct location:
```bash
ls -la build/assets/images/prerendered/
```

### Wrong image size displayed

The screen width breakpoints in `splash_screen.cpp` must match the sizes defined in `regen_images.sh`. If you add new screen sizes, update both files.

### Build fails with missing Pillow

Install the Python dependency:
```bash
pip install Pillow
# or with the project venv:
.venv/bin/pip install Pillow
```

## Technical Details

### LVGLImage.py

The `scripts/LVGLImage.py` script (from LVGL's tools) handles the conversion with these key options:

- `--cf ARGB8888`: Color format (32-bit with alpha)
- `--ofmt BIN`: Output format (LVGL binary)
- `--resize WxH`: Target size in pixels
- `--resize-fit`: Preserve aspect ratio (letterbox if needed)

### Why Not Use LVGL's Built-in Image Conversion?

LVGL can convert images at build time via its CMake integration, but:

1. This project uses Make, not CMake
2. We need exact pixel sizes for each screen resolution
3. The Python script gives us more control over the conversion process

### Memory Considerations

Pre-rendered images are larger than PNGs (uncompressed vs. compressed), but:
- They're stored on the filesystem, not in RAM
- They're loaded directly into LVGL's image cache
- The AD5M has 512MB RAM and 8GB storage, so space isn't a concern
