# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - Pre-rendered Image Generation Module
#
# Pre-renders PNG images to LVGL binary format for each supported screen size.
# This eliminates runtime PNG decoding, dramatically improving startup performance
# on embedded devices (2 FPS → 116 FPS on AD5M).
#
# Images are generated to the BUILD directory (not committed to repo).
# They are created automatically during embedded builds and deployments.
#
# Splash screen targets:
#   gen-images-ad5m  - Generate only 800x480 (AD5M fixed display)
#   gen-images-ad5x  - Generate only 800x480 (AD5X fixed display)
#   gen-images-pi    - Generate all sizes (Pi variable displays)
#   gen-images       - Generate all sizes (generic)
#
# Printer image targets:
#   gen-printer-images - Pre-render all printer images at 300px and 150px
#
# Output:
#   build/assets/images/prerendered/*.bin        (splash)
#   build/assets/images/printers/prerendered/*.bin (printers)
#
# See: docs/PRE_RENDERED_IMAGES.md

PRERENDERED_DIR := $(BUILD_DIR)/assets/images/prerendered
PRERENDERED_PRINTERS_DIR := $(BUILD_DIR)/assets/images/printers/prerendered
REGEN_IMAGES_SCRIPT := scripts/regen_images.sh
REGEN_PRINTER_IMAGES_SCRIPT := scripts/regen_printer_images.sh

# Pre-rendered image files (build artifacts, not in repo)
# AD5M only needs 'small' (800x480)
PRERENDERED_IMAGES_AD5M := $(PRERENDERED_DIR)/splash-logo-small.bin
# AD5X only needs 'small' (800x480)
PRERENDERED_IMAGES_AD5X := $(PRERENDERED_DIR)/splash-logo-small.bin

# Pi needs all sizes (unknown display at build time)
PRERENDERED_IMAGES_ALL := \
    $(PRERENDERED_DIR)/splash-logo-tiny.bin \
    $(PRERENDERED_DIR)/splash-logo-small.bin \
    $(PRERENDERED_DIR)/splash-logo-medium.bin \
    $(PRERENDERED_DIR)/splash-logo-large.bin

# Generate images for AD5M (800x480 fixed display only)
# NOTE: Uses mkdir -p instead of $(BUILD_DIR) dependency to avoid triggering 'build' target
# which runs 'make clean' and wipes cross-compiled binaries
.PHONY: gen-images-ad5m
gen-images-ad5m:
	$(ECHO) "$(CYAN)Generating pre-rendered images for AD5M (800x480)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)OUTPUT_DIR=$(PRERENDERED_DIR) TARGET_SIZES=small ./$(REGEN_IMAGES_SCRIPT)
	$(ECHO) "$(GREEN)✓ AD5M images generated$(RESET)"

# Generate images for AD5X (800x480 fixed display only)
# NOTE: Uses mkdir -p instead of $(BUILD_DIR) dependency to avoid triggering 'build' target
# which runs 'make clean' and wipes cross-compiled binaries
.PHONY: gen-images-ad5x
gen-images-ad5x:
	$(ECHO) "$(CYAN)Generating pre-rendered images for AD5X (800x480)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)OUTPUT_DIR=$(PRERENDERED_DIR) TARGET_SIZES=small ./$(REGEN_IMAGES_SCRIPT)
	$(ECHO) "$(GREEN)✓ AD5X images generated$(RESET)"

# Generate images for Pi (all sizes for variable displays)
.PHONY: gen-images-pi
gen-images-pi:
	$(ECHO) "$(CYAN)Generating pre-rendered images for Pi (all sizes)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)OUTPUT_DIR=$(PRERENDERED_DIR) ./$(REGEN_IMAGES_SCRIPT)
	$(ECHO) "$(GREEN)✓ Pi images generated$(RESET)"

# Generate all pre-rendered images (generic)
.PHONY: gen-images
gen-images:
	$(ECHO) "$(CYAN)Generating pre-rendered images (all sizes)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)OUTPUT_DIR=$(PRERENDERED_DIR) ./$(REGEN_IMAGES_SCRIPT)
	$(ECHO) "$(GREEN)✓ Pre-rendered images generated in $(PRERENDERED_DIR)/$(RESET)"

# Legacy alias
.PHONY: regen-images
regen-images: gen-images

# Clean generated images
.PHONY: clean-images
clean-images:
	$(ECHO) "$(CYAN)Cleaning pre-rendered images...$(RESET)"
	$(Q)rm -rf $(PRERENDERED_DIR)
	$(ECHO) "$(GREEN)✓ Cleaned $(PRERENDERED_DIR)$(RESET)"

# List what would be generated
.PHONY: list-images
list-images:
	$(Q)./$(REGEN_IMAGES_SCRIPT) --list

# Check if pre-rendered images exist in build directory
.PHONY: check-images
check-images:
	$(ECHO) "$(CYAN)Checking pre-rendered images...$(RESET)"
	$(Q)missing=0; \
	for img in $(PRERENDERED_IMAGES_ALL); do \
		if [ ! -f "$$img" ]; then \
			echo "$(RED)✗ Missing: $$img$(RESET)"; \
			missing=1; \
		fi; \
	done; \
	if [ $$missing -eq 1 ]; then \
		echo "$(RED)Run 'make gen-images' to generate missing files$(RESET)"; \
		exit 1; \
	else \
		echo "$(GREEN)✓ All pre-rendered images present$(RESET)"; \
	fi

# =============================================================================
# Placeholder Thumbnail Pre-rendering
# =============================================================================
# Pre-renders placeholder thumbnails for instant display on embedded devices
REGEN_PLACEHOLDER_SCRIPT := scripts/regen_placeholder_images.sh

.PHONY: gen-placeholder-images
gen-placeholder-images:
	$(ECHO) "$(CYAN)Generating pre-rendered placeholder thumbnails...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)OUTPUT_DIR=$(PRERENDERED_DIR) ./$(REGEN_PLACEHOLDER_SCRIPT)
	$(ECHO) "$(GREEN)✓ Placeholder images generated$(RESET)"

.PHONY: clean-placeholder-images
clean-placeholder-images:
	$(ECHO) "$(CYAN)Cleaning pre-rendered placeholder images...$(RESET)"
	$(Q)./$(REGEN_PLACEHOLDER_SCRIPT) --clean
	$(ECHO) "$(GREEN)✓ Cleaned placeholder images$(RESET)"

.PHONY: list-placeholder-images
list-placeholder-images:
	$(Q)./$(REGEN_PLACEHOLDER_SCRIPT) --list

# =============================================================================
# Printer Image Pre-rendering
# =============================================================================
# Generates optimized versions of printer images at 300px and 150px
# Original PNGs kept as fallbacks

# Generate pre-rendered printer images (all sizes)
# NOTE: Uses mkdir -p instead of $(BUILD_DIR) dependency (see gen-images-ad5m comment)
.PHONY: gen-printer-images
gen-printer-images:
	$(ECHO) "$(CYAN)Generating pre-rendered printer images...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_PRINTERS_DIR)
	$(Q)OUTPUT_DIR=$(PRERENDERED_PRINTERS_DIR) ./$(REGEN_PRINTER_IMAGES_SCRIPT)
	$(ECHO) "$(GREEN)✓ Printer images generated$(RESET)"

# Clean pre-rendered printer images
.PHONY: clean-printer-images
clean-printer-images:
	$(ECHO) "$(CYAN)Cleaning pre-rendered printer images...$(RESET)"
	$(Q)rm -rf $(PRERENDERED_PRINTERS_DIR)
	$(ECHO) "$(GREEN)✓ Cleaned $(PRERENDERED_PRINTERS_DIR)$(RESET)"

# List printer images that would be generated
.PHONY: list-printer-images
list-printer-images:
	$(Q)./$(REGEN_PRINTER_IMAGES_SCRIPT) --list

# =============================================================================
# 3D Splash Image Generation (full-screen composited)
# =============================================================================
# Generates full-screen 3D splash images (dark + light modes) at all sizes.
# These are composited images: logo centered on bg-colored canvas at screen res.
GEN_SPLASH_3D_SCRIPT := scripts/gen_splash_3d.py
SPLASH_3D_PYTHON := $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

# Generate 3D splash images for all sizes and modes
.PHONY: gen-splash-3d
gen-splash-3d:
	$(ECHO) "$(CYAN)Generating 3D splash images (all sizes, dark+light)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)$(SPLASH_3D_PYTHON) $(GEN_SPLASH_3D_SCRIPT) --output-dir $(PRERENDERED_DIR)
	$(ECHO) "$(GREEN)✓ 3D splash images generated$(RESET)"

# Generate 3D splash images for AD5M only (800x480)
.PHONY: gen-splash-3d-ad5m
gen-splash-3d-ad5m:
	$(ECHO) "$(CYAN)Generating 3D splash images for AD5M (800x480)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)$(SPLASH_3D_PYTHON) $(GEN_SPLASH_3D_SCRIPT) --output-dir $(PRERENDERED_DIR) --sizes small
	$(ECHO) "$(GREEN)✓ AD5M 3D splash images generated$(RESET)"

# Generate 3D splash images for AD5X only (800x480)
.PHONY: gen-splash-3d-ad5x
gen-splash-3d-ad5x:
	$(ECHO) "$(CYAN)Generating 3D splash images for AD5X (800x480)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)$(SPLASH_3D_PYTHON) $(GEN_SPLASH_3D_SCRIPT) --output-dir $(PRERENDERED_DIR) --sizes small
	$(ECHO) "$(GREEN)✓ AD5X 3D splash images generated$(RESET)"

# Generate 3D splash images for K1 only (480x400)
.PHONY: gen-splash-3d-k1
gen-splash-3d-k1:
	$(ECHO) "$(CYAN)Generating 3D splash images for K1 (480x400)...$(RESET)"
	$(Q)mkdir -p $(PRERENDERED_DIR)
	$(Q)$(SPLASH_3D_PYTHON) $(GEN_SPLASH_3D_SCRIPT) --output-dir $(PRERENDERED_DIR) --sizes tiny_alt
	$(ECHO) "$(GREEN)✓ K1 3D splash images generated$(RESET)"

# Clean 3D splash images
.PHONY: clean-splash-3d
clean-splash-3d:
	$(ECHO) "$(CYAN)Cleaning 3D splash images...$(RESET)"
	$(Q)rm -f $(PRERENDERED_DIR)/splash-3d-*.bin 2>/dev/null || true
	$(ECHO) "$(GREEN)✓ Cleaned 3D splash images$(RESET)"

# Generate ALL pre-rendered images (splash + 3D splash + printers + placeholders)
.PHONY: gen-all-images
gen-all-images: gen-images gen-splash-3d gen-printer-images gen-placeholder-images

# Clean ALL pre-rendered images
.PHONY: clean-all-images
clean-all-images: clean-images clean-splash-3d clean-printer-images clean-placeholder-images

# =============================================================================
# Help
# =============================================================================

# Help text for image targets
.PHONY: help-images
help-images:
	@echo "Pre-rendered image targets:"
	@echo ""
	@echo "  Splash screen:"
	@echo "    gen-images         - Generate splash .bin files (all sizes)"
	@echo "    gen-images-ad5m    - Generate splash for AD5M only (800x480)"
	@echo "    gen-images-ad5x    - Generate splash for AD5X only (800x480)"
	@echo "    gen-images-pi      - Generate splash for Pi (all sizes)"
	@echo "    clean-images       - Remove splash .bin files"
	@echo "    list-images        - Show splash targets"
	@echo "    gen-splash-3d      - Generate 3D splash (all sizes, dark+light)"
	@echo "    gen-splash-3d-ad5m - Generate 3D splash for AD5M only"
	@echo "    gen-splash-3d-ad5x - Generate 3D splash for AD5X only"
	@echo "    clean-splash-3d    - Remove 3D splash .bin files"
	@echo ""
	@echo "  Placeholder thumbnails:"
	@echo "    gen-placeholder-images   - Generate placeholder .bin files"
	@echo "    clean-placeholder-images - Remove placeholder .bin files"
	@echo "    list-placeholder-images  - Show placeholder targets"
	@echo ""
	@echo "  Printer images:"
	@echo "    gen-printer-images - Generate printer .bin files (300px, 150px)"
	@echo "    clean-printer-images - Remove printer .bin files"
	@echo "    list-printer-images  - Show printer targets"
	@echo ""
	@echo "  Combined:"
	@echo "    gen-all-images     - Generate all pre-rendered images"
	@echo "    clean-all-images   - Remove all pre-rendered images"
	@echo ""
	@echo "Output directories:"
	@echo "  $(PRERENDERED_DIR)/"
	@echo "  $(PRERENDERED_PRINTERS_DIR)/"
	@echo ""
	@echo "Note: Generated images are build artifacts, not committed to repo."
	@echo "      They are created during deploy-* and release builds."
	@echo ""
	@echo "Performance impact:"
	@echo "  PNG decoding:    ~2 FPS during splash (AD5M)"
	@echo "  Pre-rendered:    ~116 FPS (instant display)"
