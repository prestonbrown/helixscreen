# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Font & Icon Generation Module
# Handles font generation and Material Design icons

# =============================================================================
# Per-tier font file lists
# =============================================================================
# Each tier lists the text fonts and icon fonts its globals.xml constants reference.
# FONT_SRCS is assembled from the union of declared FONT_TIERS.

FONTS_MICRO := assets/fonts/noto_sans_10.c assets/fonts/noto_sans_14.c \
               assets/fonts/noto_sans_bold_16.c \
               assets/fonts/noto_sans_light_10.c \
               assets/fonts/source_code_pro_8.c \
               assets/fonts/mdi_icons_16.c assets/fonts/mdi_icons_24.c \
               assets/fonts/mdi_icons_32.c

FONTS_TINY := assets/fonts/noto_sans_12.c assets/fonts/noto_sans_16.c \
              assets/fonts/noto_sans_bold_18.c \
              assets/fonts/noto_sans_light_11.c \
              assets/fonts/source_code_pro_10.c \
              assets/fonts/mdi_icons_24.c assets/fonts/mdi_icons_32.c \
              assets/fonts/mdi_icons_48.c

FONTS_SMALL := assets/fonts/noto_sans_14.c assets/fonts/noto_sans_20.c \
               assets/fonts/noto_sans_bold_20.c \
               assets/fonts/noto_sans_light_12.c \
               assets/fonts/source_code_pro_12.c \
               assets/fonts/mdi_icons_16.c assets/fonts/mdi_icons_24.c \
               assets/fonts/mdi_icons_32.c assets/fonts/mdi_icons_48.c \
               assets/fonts/mdi_icons_64.c

FONTS_MEDIUM := assets/fonts/noto_sans_18.c assets/fonts/noto_sans_26.c \
                assets/fonts/noto_sans_bold_28.c \
                assets/fonts/noto_sans_light_16.c \
                assets/fonts/source_code_pro_14.c \
                assets/fonts/mdi_icons_16.c assets/fonts/mdi_icons_24.c \
                assets/fonts/mdi_icons_32.c assets/fonts/mdi_icons_48.c \
                assets/fonts/mdi_icons_64.c

FONTS_LARGE := assets/fonts/noto_sans_20.c assets/fonts/noto_sans_28.c \
               assets/fonts/noto_sans_bold_28.c \
               assets/fonts/noto_sans_light_14.c assets/fonts/noto_sans_light_18.c \
               assets/fonts/source_code_pro_16.c \
               assets/fonts/mdi_icons_16.c assets/fonts/mdi_icons_24.c \
               assets/fonts/mdi_icons_32.c assets/fonts/mdi_icons_48.c \
               assets/fonts/mdi_icons_64.c

FONTS_XLARGE := assets/fonts/noto_sans_24.c assets/fonts/noto_sans_32.c \
                assets/fonts/noto_sans_bold_32.c \
                assets/fonts/noto_sans_light_16.c assets/fonts/noto_sans_light_20.c \
                assets/fonts/source_code_pro_18.c \
                assets/fonts/mdi_icons_24.c assets/fonts/mdi_icons_32.c \
                assets/fonts/mdi_icons_48.c assets/fonts/mdi_icons_64.c \
                assets/fonts/mdi_icons_80.c

FONTS_XXLARGE := assets/fonts/noto_sans_32.c assets/fonts/noto_sans_40.c \
                 assets/fonts/noto_sans_bold_40.c \
                 assets/fonts/noto_sans_light_20.c assets/fonts/noto_sans_light_26.c \
                 assets/fonts/source_code_pro_20.c assets/fonts/source_code_pro_24.c \
                 assets/fonts/mdi_icons_32.c assets/fonts/mdi_icons_48.c \
                 assets/fonts/mdi_icons_64.c assets/fonts/mdi_icons_96.c \
                 assets/fonts/mdi_icons_128.c

# Core fonts — referenced unconditionally by C++ code (asset_manager, theme_manager,
# cjk_font_manager, ui_button, helix_watchdog, many panels/widgets) regardless of
# breakpoint tier. These are always included in every platform build.
#
# This list MUST cover every lv_font_t whose address is taken in C/C++ without a
# HELIX_MAX_FONT_TIER guard. Key sources of unconditional references:
#   - asset_manager.cpp register_fonts(): noto_sans_{10,11,12,14,16,18,20,24},
#     noto_sans_light_{10,11,12}, noto_sans_bold_{14,16,18,20,24,28},
#     source_code_pro_{10,12,14,16}, mdi_icons_{14,16,24,32,48,64}
#   - cjk_font_manager.cpp REGULAR/BOLD/LIGHT_FONTS tables: adds noto_sans_{26,28},
#     noto_sans_light_{14,16,18} (xlarge/xxlarge entries are #if-guarded)
#   - helix_watchdog.cpp: noto_sans_14, noto_sans_bold_{16,24}, mdi_icons_64
#   - ui_button.cpp / theme_manager.cpp is_icon_font(): mdi_icons_{14,16,24,32,48,64}
#   - LVGL default font (lv_conf) resolves to noto_sans_14 on this build
# The micro-only 8px fonts are included too — they are tiny and simpler to keep
# in core than to gate the is_micro branch on a separate HELIX_HAS_MICRO define.
FONTS_CORE := assets/fonts/noto_sans_8.c \
              assets/fonts/noto_sans_10.c assets/fonts/noto_sans_11.c \
              assets/fonts/noto_sans_12.c assets/fonts/noto_sans_14.c \
              assets/fonts/noto_sans_16.c assets/fonts/noto_sans_18.c \
              assets/fonts/noto_sans_20.c assets/fonts/noto_sans_24.c \
              assets/fonts/noto_sans_bold_14.c assets/fonts/noto_sans_bold_16.c \
              assets/fonts/noto_sans_bold_18.c assets/fonts/noto_sans_bold_20.c \
              assets/fonts/noto_sans_bold_24.c assets/fonts/noto_sans_bold_28.c \
              assets/fonts/noto_sans_light_10.c assets/fonts/noto_sans_light_11.c \
              assets/fonts/noto_sans_light_12.c \
              assets/fonts/source_code_pro_8.c assets/fonts/source_code_pro_10.c \
              assets/fonts/source_code_pro_12.c assets/fonts/source_code_pro_14.c \
              assets/fonts/source_code_pro_16.c \
              assets/fonts/mdi_icons_14.c assets/fonts/mdi_icons_16.c \
              assets/fonts/mdi_icons_24.c assets/fonts/mdi_icons_32.c \
              assets/fonts/mdi_icons_48.c assets/fonts/mdi_icons_64.c

FONTS_ALL := $(sort $(FONTS_CORE) $(FONTS_MICRO) $(FONTS_TINY) $(FONTS_SMALL) $(FONTS_MEDIUM) \
             $(FONTS_LARGE) $(FONTS_XLARGE) $(FONTS_XXLARGE))

# Assemble TIER_FONT_SRCS from declared tiers (sort deduplicates)
# FONTS_CORE is always included — every platform needs these.
FONT_TIERS ?= all
ifeq ($(FONT_TIERS),all)
    TIER_FONT_SRCS := $(FONTS_ALL)
else
    TIER_FONT_SRCS := $(FONTS_CORE)
    ifneq ($(filter micro,$(FONT_TIERS)),)
        TIER_FONT_SRCS += $(FONTS_MICRO)
    endif
    ifneq ($(filter tiny,$(FONT_TIERS)),)
        TIER_FONT_SRCS += $(FONTS_TINY)
    endif
    ifneq ($(filter small,$(FONT_TIERS)),)
        TIER_FONT_SRCS += $(FONTS_SMALL)
    endif
    # Tiers medium and up are selected by ">= that tier", NOT by set membership.
    # asset_manager.cpp / cjk_font_manager.cpp guard those faces with
    # `#if HELIX_MAX_FONT_TIER >= N`, and cross.mk derives MAX from the HIGHEST
    # declared tier. So a platform whose max is >= N compiles a reference to
    # tier N's faces even when N is absent from its declared set: k2 declares
    # "large xlarge" (max 5) and would reference noto_sans_26 / noto_sans_light_16
    # with FONTS_MEDIUM missing from its sources -- an undefined-reference link
    # failure. Matching the guards' ">=" semantics here keeps the two sides
    # consistent by construction rather than by every platform remembering to
    # spell out its lower tiers.
    ifneq ($(filter medium large xlarge xxlarge,$(FONT_TIERS)),)
        TIER_FONT_SRCS += $(FONTS_MEDIUM)
    endif
    ifneq ($(filter large xlarge xxlarge,$(FONT_TIERS)),)
        TIER_FONT_SRCS += $(FONTS_LARGE)
    endif
    ifneq ($(filter xlarge xxlarge,$(FONT_TIERS)),)
        TIER_FONT_SRCS += $(FONTS_XLARGE)
    endif
    ifneq ($(filter xxlarge,$(FONT_TIERS)),)
        TIER_FONT_SRCS += $(FONTS_XXLARGE)
    endif
    TIER_FONT_SRCS := $(sort $(TIER_FONT_SRCS))
endif

# Generate MDI icon fonts using the authoritative regen script
# Triggered when regen_mdi_fonts.sh changes (single source of truth for icon codepoints)
.fonts.stamp: scripts/regen_mdi_fonts.sh
	$(ECHO) "$(CYAN)Checking font generation...$(RESET)"
	$(Q)if ! command -v lv_font_conv >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ lv_font_conv not found - skipping font generation$(RESET)"; \
		echo "$(YELLOW)  Run 'npm install' to enable font regeneration$(RESET)"; \
		touch $@; \
	else \
		echo "$(YELLOW)→ Regenerating MDI icon fonts from regen_mdi_fonts.sh...$(RESET)"; \
		./scripts/regen_mdi_fonts.sh && touch $@ && echo "$(GREEN)✓ Fonts regenerated successfully$(RESET)"; \
	fi

# Fonts depend on stamp file to ensure they're regenerated when needed
$(FONT_SRCS): .fonts.stamp

generate-fonts: .fonts.stamp

# Validate that all icons in ui_icon_codepoints.h are present in compiled fonts
# This prevents the bug where icons are added to code but fonts aren't regenerated
validate-fonts:
	$(ECHO) "$(CYAN)Validating icon font codepoints...$(RESET)"
	$(Q)if [ -f scripts/validate_icon_fonts.sh ]; then \
		if ./scripts/validate_icon_fonts.sh; then \
			echo "$(GREEN)✓ All icon codepoints present in fonts$(RESET)"; \
		else \
			echo "$(RED)✗ Missing icon codepoints - run 'make regen-fonts' to fix$(RESET)"; \
			exit 1; \
		fi; \
	else \
		echo "$(YELLOW)⚠ validate_icon_fonts.sh not found - skipping$(RESET)"; \
	fi

# Regenerate MDI icon fonts from scratch using the regen script
# Use this when adding new icons to include/ui_icon_codepoints.h
regen-fonts:
	$(ECHO) "$(CYAN)Regenerating MDI icon fonts...$(RESET)"
	$(Q)if [ -f scripts/regen_mdi_fonts.sh ]; then \
		./scripts/regen_mdi_fonts.sh; \
		echo "$(GREEN)✓ Fonts regenerated - rebuild required$(RESET)"; \
	else \
		echo "$(RED)✗ regen_mdi_fonts.sh not found$(RESET)"; \
		exit 1; \
	fi

# Regenerate Noto Sans text fonts with extended Unicode (Latin, Cyrillic, CJK)
# Use this for internationalization support
regen-text-fonts:
	$(ECHO) "$(CYAN)Regenerating Noto Sans text fonts...$(RESET)"
	$(Q)if [ -f scripts/regen_text_fonts.sh ]; then \
		./scripts/regen_text_fonts.sh; \
		echo "$(GREEN)✓ Text fonts regenerated - rebuild required$(RESET)"; \
	else \
		echo "$(RED)✗ regen_text_fonts.sh not found$(RESET)"; \
		exit 1; \
	fi

# Auto-regenerate text fonts when CJK translation files change
# Only triggers if CJK source fonts are present (they're gitignored)
CJK_FONT_SC := assets/fonts/NotoSansCJKsc-Regular.otf
CJK_FONT_JP := assets/fonts/NotoSansCJKjp-Regular.otf
CJK_TRANS_YAML := translations/zh.yml translations/ja.yml
TEXT_FONT_STAMP := .text-fonts.stamp

# Check if any CJK translation YAML is newer than the text font stamp
$(TEXT_FONT_STAMP): $(CJK_TRANS_YAML) scripts/regen_text_fonts.sh
	$(Q)if command -v lv_font_conv >/dev/null 2>&1; then \
		echo "$(YELLOW)→ CJK translations changed - regenerating text fonts (will auto-download CJK fonts if needed)...$(RESET)"; \
		./scripts/regen_text_fonts.sh && touch $@ && echo "$(GREEN)✓ Text fonts regenerated with CJK$(RESET)"; \
	else \
		echo "$(YELLOW)⚠ lv_font_conv not found - skipping CJK font regeneration$(RESET)"; \
		touch $@; \
	fi

# Regenerate icon constants in globals.xml from ui_icon_codepoints.h
# Single source of truth: C++ header -> XML constants
regen-icon-consts:
	$(ECHO) "$(CYAN)Regenerating icon constants in globals.xml...$(RESET)"
	$(Q)python3 scripts/gen_icon_consts.py
	$(ECHO) "$(GREEN)✓ Icon constants regenerated$(RESET)"

# Update MDI icon metadata cache from Pictogrammers GitHub
# Run periodically when MDI library updates, or when adding new icons
update-mdi-cache:
	$(ECHO) "$(CYAN)Updating MDI metadata cache...$(RESET)"
	$(Q)curl -sL "https://raw.githubusercontent.com/Templarian/MaterialDesign/master/meta.json" | gzip > assets/mdi-icon-metadata.json.gz
	$(ECHO) "$(GREEN)✓ Updated assets/mdi-icon-metadata.json.gz$(RESET)"
	@ls -lh assets/mdi-icon-metadata.json.gz | awk '{print "$(CYAN)  Size: " $$5 "$(RESET)"}'

# Verify MDI codepoint labels match official metadata
verify-mdi-codepoints:
	$(ECHO) "$(CYAN)Verifying MDI codepoint labels...$(RESET)"
	$(Q)python3 scripts/verify_mdi_codepoints.py

# Generate macOS .icns icon from source logo
# Requires: ImageMagick (magick) for image processing
# Source: assets/images/helixscreen-logo.png
# Output: assets/images/helix-icon.icns (macOS), assets/images/helix-icon.png (Linux)
icon:
ifeq ($(UNAME_S),Darwin)
	$(ECHO) "$(CYAN)Generating macOS icon from logo...$(RESET)"
	@if ! command -v magick >/dev/null 2>&1; then \
		echo "$(RED)✗ ImageMagick (magick) not found$(RESET)"; \
		echo "$(YELLOW)Install with: brew install imagemagick$(RESET)"; \
		exit 1; \
	fi
	@if ! command -v iconutil >/dev/null 2>&1; then \
		echo "$(RED)✗ iconutil not found (should be built-in on macOS)$(RESET)"; \
		exit 1; \
	fi
else
	$(ECHO) "$(CYAN)Generating icon from logo (Linux - PNG only)...$(RESET)"
	@if ! command -v magick >/dev/null 2>&1; then \
		echo "$(RED)✗ ImageMagick (magick) not found$(RESET)"; \
		echo "$(YELLOW)Install with: sudo apt install imagemagick$(RESET)"; \
		exit 1; \
	fi
endif
	$(ECHO) "$(CYAN)  [1/6] Cropping logo to circular icon...$(RESET)"
	$(Q)magick assets/images/helixscreen-logo.png \
		-crop 700x580+162+100 +repage \
		-gravity center -background none -extent 680x680 \
		assets/images/helix-icon.png
	$(ECHO) "$(CYAN)  [2/6] Generating 128x128 icon for window...$(RESET)"
	$(Q)magick assets/images/helix-icon.png -resize 128x128 assets/images/helix-icon-128.png
	$(ECHO) "$(CYAN)  [3/6] Generating C header file for embedded icon...$(RESET)"
	$(Q)python3 scripts/generate_icon_header.py assets/images/helix-icon-128.png include/helix_icon_data.h
ifeq ($(UNAME_S),Darwin)
	$(ECHO) "$(CYAN)  [4/6] Generating icon sizes (16px to 1024px)...$(RESET)"
	$(Q)mkdir -p assets/images/icon.iconset
	$(Q)for size in 16 32 64 128 256 512; do \
		magick assets/images/helix-icon.png -resize $${size}x$${size} \
			assets/images/icon.iconset/icon_$${size}x$${size}.png; \
		magick assets/images/helix-icon.png -resize $$((size*2))x$$((size*2)) \
			assets/images/icon.iconset/icon_$${size}x$${size}@2x.png; \
	done
	$(ECHO) "$(CYAN)  [5/6] Creating .icns bundle...$(RESET)"
	$(Q)iconutil -c icns assets/images/icon.iconset -o assets/images/helix-icon.icns
	$(ECHO) "$(CYAN)  [6/6] Cleaning up temporary files...$(RESET)"
	$(Q)rm -rf assets/images/icon.iconset
	$(ECHO) "$(GREEN)✓ Icon generated: assets/images/helix-icon.icns + helix-icon-128.png + header$(RESET)"
	@ls -lh assets/images/helix-icon.icns assets/images/helix-icon-128.png include/helix_icon_data.h | awk '{print "$(CYAN)  " $$9 ": " $$5 "$(RESET)"}'
else
	$(ECHO) "$(CYAN)  [4/4] Icon generated (PNG format)...$(RESET)"
	$(ECHO) "$(GREEN)✓ Icon generated: assets/images/helix-icon.png + helix-icon-128.png + header$(RESET)"
	@ls -lh assets/images/helix-icon.png assets/images/helix-icon-128.png include/helix_icon_data.h | awk '{print "$(CYAN)  " $$9 ": " $$5 "$(RESET)"}'
	$(ECHO) "$(YELLOW)Note: .icns format requires macOS. PNG icons can be used for Linux apps.$(RESET)"
endif
