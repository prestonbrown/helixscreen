# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Upstream Patch Management Module
# Handles automatic application of patches to LVGL and other dependencies

# Apply LVGL patches if not already applied
apply-patches:
	$(ECHO) "$(CYAN)Checking LVGL patches...$(RESET)"
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/sdl/lv_sdl_window.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL SDL window position patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_sdl_window_position.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_sdl_window_position.patch && \
			echo "$(GREEN)✓ SDL window position patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL SDL window position patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/themes/default/lv_theme_default.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL theme breakpoints patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_theme_breakpoints.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_theme_breakpoints.patch && \
			echo "$(GREEN)✓ Theme breakpoints patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL theme breakpoints patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/xml/parsers/lv_xml_image_parser.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL image parser contain/cover patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_image_parser_contain.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_image_parser_contain.patch && \
			echo "$(GREEN)✓ Image parser contain/cover patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL image parser contain/cover patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/layouts/flex/lv_flex.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL flex SIZE_CONTENT propagation patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_flex_size_content_propagate.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_flex_size_content_propagate.patch && \
			echo "$(GREEN)✓ Flex SIZE_CONTENT propagation patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL flex SIZE_CONTENT propagation patch already applied$(RESET)"; \
	fi
