# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Translation Generation Module
# Handles i18n translation file generation from YAML master files

# Python venv for translation generator
VENV_PYTHON_TRANS := .venv/bin/python3

# Holds the HELIX_LANG stamp below
TRANS_GEN_DIR := src/generated
TRANS_XML := ui_xml/translations/translations.xml

# Source files for translations
TRANS_YAML := $(wildcard translations/*.yml)
TRANS_SCRIPT := scripts/generate_translations.py
TRANS_VARIANTS := scripts/translations/locale_variants.py

# HELIX_LANG: optional comma-separated locale whitelist (e.g. "en" or "en,zh")
# When set, generate_translations.py emits only those locales' packs. Base
# locale (en) is always kept as fallback. Unset = all locales from
# translations/*.yml. Platform Makefiles set this to shrink what ships on
# memory-constrained targets. See mk/cross.mk (cc1).
HELIX_LANG ?=
ifneq ($(HELIX_LANG),)
    TRANS_LANG_FLAG := --languages "$(HELIX_LANG)"
else
    TRANS_LANG_FLAG :=
endif

# Stamp file tracks the HELIX_LANG value that generated the current packs.
# When HELIX_LANG changes between builds (e.g. switching platforms), the stamp
# is rewritten and forces translation regeneration. Without this, the existing
# packs pass YAML-mtime dependency checks even though their language set is
# stale.
TRANS_LANG_STAMP := $(TRANS_GEN_DIR)/.helix-lang-stamp
.PHONY: $(TRANS_LANG_STAMP)-update
$(TRANS_LANG_STAMP): $(TRANS_LANG_STAMP)-update
	@mkdir -p $(TRANS_GEN_DIR)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(HELIX_LANG)" ]; then \
		printf '%s' '$(HELIX_LANG)' > $@; \
	fi
$(TRANS_LANG_STAMP)-update:
	@:

# Generate translations from YAML master files
# Creates: translations.xml + per-locale <lang>.xml (the runtime translation
# packs), which the app loads through LVGL's lv_translation_* API.
# The generator writes the master before the per-locale packs, so a run that
# dies between the two leaves a fresh-mtime master over a partial set, and the
# next build would see the target up to date and ship it. A failed generation
# must leave no master behind.
$(TRANS_XML): $(TRANS_YAML) $(TRANS_SCRIPT) $(TRANS_VARIANTS) $(TRANS_LANG_STAMP)
	$(ECHO) "$(CYAN)Generating translations from YAML...$(if $(HELIX_LANG), (HELIX_LANG=$(HELIX_LANG)))$(RESET)"
	$(Q)mkdir -p $(TRANS_GEN_DIR)
	$(Q)if [ -x "$(VENV_PYTHON_TRANS)" ] && $(VENV_PYTHON_TRANS) -c "import yaml" 2>/dev/null; then \
		$(VENV_PYTHON_TRANS) $(TRANS_SCRIPT) $(TRANS_LANG_FLAG) && \
		echo "$(GREEN)✓ Translations generated$(RESET)" || { rm -f $@; exit 1; }; \
	elif python3 -c "import yaml" 2>/dev/null; then \
		python3 $(TRANS_SCRIPT) $(TRANS_LANG_FLAG) && \
		echo "$(GREEN)✓ Translations generated (system python3)$(RESET)" || { rm -f $@; exit 1; }; \
	elif [ -f "$(TRANS_XML)" ]; then \
		echo "$(YELLOW)⚠ Python venv not available - using existing generated translations$(RESET)"; \
		touch $(TRANS_XML); \
	else \
		echo "$(RED)✗ Python with yaml not available and no generated files - run 'make venv-setup'$(RESET)"; \
		exit 1; \
	fi

# Phony target for manual regeneration
.PHONY: translations
translations:
	$(ECHO) "$(CYAN)Regenerating translations...$(RESET)"
	$(Q)mkdir -p $(TRANS_GEN_DIR)
	$(Q)if [ -x "$(VENV_PYTHON_TRANS)" ]; then \
		$(VENV_PYTHON_TRANS) $(TRANS_SCRIPT) && \
		echo "$(GREEN)✓ Translations regenerated$(RESET)"; \
	else \
		echo "$(RED)✗ Python venv not available - run 'make venv-setup'$(RESET)"; \
		exit 1; \
	fi

# Translation sync tool - manage translation string lifecycle
TRANS_SYNC_SCRIPT := scripts/translation_sync.py

# Sync translations: extract from XML/C++, merge new keys to YAML
# Use translation-sync-dry-run first to preview changes
# After sync, checks if CJK text fonts need regeneration (new characters added)
.PHONY: translation-sync
translation-sync:
	$(ECHO) "$(CYAN)Syncing translation strings...$(RESET)"
	$(Q)if [ -x "$(VENV_PYTHON_TRANS)" ]; then \
		$(VENV_PYTHON_TRANS) $(TRANS_SYNC_SCRIPT) sync; \
	else \
		echo "$(RED)✗ Python venv not available - run 'make venv-setup'$(RESET)"; \
		exit 1; \
	fi
	$(Q)scripts/check_cjk_font_staleness.sh

# Preview sync without modifying files
.PHONY: translation-sync-dry-run
translation-sync-dry-run:
	$(ECHO) "$(CYAN)Previewing translation sync...$(RESET)"
	$(Q)if [ -x "$(VENV_PYTHON_TRANS)" ]; then \
		$(VENV_PYTHON_TRANS) $(TRANS_SYNC_SCRIPT) sync --dry-run; \
	else \
		echo "$(RED)✗ Python venv not available - run 'make venv-setup'$(RESET)"; \
		exit 1; \
	fi

# Show translation coverage statistics
.PHONY: translation-coverage
translation-coverage:
	$(ECHO) "$(CYAN)Translation coverage report...$(RESET)"
	$(Q)if [ -x "$(VENV_PYTHON_TRANS)" ]; then \
		$(VENV_PYTHON_TRANS) $(TRANS_SYNC_SCRIPT) coverage --show-missing; \
	else \
		echo "$(RED)✗ Python venv not available - run 'make venv-setup'$(RESET)"; \
		exit 1; \
	fi

# Regenerate the terminology glossary (translations/GLOSSARY.md) from the YAML.
# The glossary pins canonical per-language renderings of recurring terms so
# translators (human or agent) stay consistent. Run after adding/translating
# strings that introduce or change a canonical term.
.PHONY: translation-glossary
translation-glossary:
	$(ECHO) "$(CYAN)Regenerating translation glossary...$(RESET)"
	$(Q)if [ -x "$(VENV_PYTHON_TRANS)" ]; then \
		$(VENV_PYTHON_TRANS) $(TRANS_SYNC_SCRIPT) glossary; \
	else \
		echo "$(RED)✗ Python venv not available - run 'make venv-setup'$(RESET)"; \
		exit 1; \
	fi

# Find obsolete translation keys (not used in XML)
.PHONY: translation-obsolete
translation-obsolete:
	$(ECHO) "$(CYAN)Finding obsolete translation keys...$(RESET)"
	$(Q)if [ -x "$(VENV_PYTHON_TRANS)" ]; then \
		$(VENV_PYTHON_TRANS) $(TRANS_SYNC_SCRIPT) obsolete; \
	else \
		echo "$(RED)✗ Python venv not available - run 'make venv-setup'$(RESET)"; \
		exit 1; \
	fi
