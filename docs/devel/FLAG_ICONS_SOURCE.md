# Flag Icons Source

## Flagpedia.net / FlagCDN

**URL**: https://flagpedia.net/download/icons

**License**: Public domain - free for commercial and non-commercial use (backlinks appreciated)

**CDN Pattern**: `https://flagcdn.com/{size}/{country_code}.png`

### Available Sizes (Waving Flags - 4:3 Aspect Ratio)
- 16×12, 20×15, 24×18, 28×21, 32×24, 36×27, 40×30
- **48×36** (used in HelixScreen)
- 56×42, 60×45, 64×48, 72×54, 80×60, 84×63
- 96×72, 108×81, 112×84, 120×90, 128×96
- 144×108, 160×120, 192×144, 224×168, 256×192

### Same Height Variants
- h20, h24, h40, h60, h80, h120

### Same Width Variants
- w20, w40, w80, w160

### Country Codes (ISO 3166-1 alpha-2)
- `us` - United States (English)
- `gb` - United Kingdom (English alternative)
- `de` - Germany
- `fr` - France
- `es` - Spain
- `ru` - Russia
- `it` - Italy
- `pt` - Portugal
- `nl` - Netherlands
- `pl` - Poland
- `jp` - Japan
- `cn` - China
- `kr` - South Korea
- ... (all countries available)

### Example Usage
```bash
# Download 48x36 waving flag for Germany
curl -sL "https://flagcdn.com/48x36/de.png" -o flag_de.png

# Download all flags at once (zip)
curl -sL "https://flagcdn.com/48x36.zip" -o flags_48x36.zip
```

### How the shipped icons are made

`assets/images/flags/flag_<lang>.png` is the 48×36 waving flag scaled to 32×24 with Pillow's
Lanczos filter, and `flag_<lang>.bin` is that PNG converted to an uncompressed ARGB8888 LVGL
image. `en` uses `us`, `en-GB` uses `gb`.

```bash
curl -sfL "https://flagcdn.com/48x36/gb.png" -o gb.png
.venv/bin/python3 -c "from PIL import Image; Image.open('gb.png').convert('RGBA').resize((32, 24), Image.LANCZOS).save('assets/images/flags/flag_en-GB.png', optimize=True)"
.venv/bin/python3 scripts/LVGLImage.py --cf ARGB8888 --ofmt BIN -o assets/images/flags --name flag_en-GB assets/images/flags/flag_en-GB.png
```

Register the new image in `src/application/asset_manager.cpp` beside the others.

### Notes
- Waving flags look nicer than flat rectangles
- 48x36 is a good size for touch buttons
- PNG format with transparency
