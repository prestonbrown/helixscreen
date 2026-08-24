# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Code Formatting Module
# Handles automatic code and XML formatting

# Python venv for XML formatter
VENV_PYTHON := .venv/bin/python

# ui_xml/translations/ is generator output (mk/translations.mk -> generate_translations.py),
# rewritten on every build. Formatting it makes the tree go dirty on the next compile.
# format-xml.py refuses these on its own (GENERATED_DIRS), but the xmllint fallback
# below has no such guard, so prune them from the file list too.
XML_FIND_PRUNE := -not -path "ui_xml/translations/*"

# android/ is pruned from the STAGED list below for the same reason: it holds
# AndroidManifest.xml and res/values/*.xml, which belong to the Android toolchain
# and not to this formatter's LVGL house style. format-xml.py self-guards via
# FOREIGN_DIRS; the xmllint fallback does not, and format-staged WRITES what it is
# handed, so an unrelated commit that stages a manifest would silently reflow it.
# The find-based lists above never reach android/ - they only walk ui_xml/.

# Resolve clang-format the same way scripts/quality-checks.sh does: the pinned
# wheel in .venv (clang-format==18.1.8, requirements) wins over the system binary,
# so a machine's Homebrew (newer) or distro (older 18.1.x) build cannot silently
# reflow the tree differently from CI. $(CLANG_FORMAT) overrides both.
CLANG_FORMAT ?= $(firstword $(wildcard .venv/bin/clang-format) clang-format)

# Format all C/C++ and XML files
format:
	$(ECHO) "$(CYAN)$(BOLD)Formatting code and XML files...$(RESET)"
	@FORMATTED_COUNT=0; \
	if ! command -v $(CLANG_FORMAT) >/dev/null 2>&1 && [ ! -x "$(CLANG_FORMAT)" ]; then \
		echo "$(RED)✗ clang-format not found$(RESET)"; \
		echo "  Install: $(YELLOW)brew install clang-format$(RESET) (macOS)"; \
		echo "         $(YELLOW)sudo apt install clang-format$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install clang-tools-extra$(RESET) (Fedora/RHEL)"; \
		exit 1; \
	fi; \
	echo "$(CYAN)Formatting C/C++ files...$(RESET)"; \
	C_FILES=$$(find src include -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.mm" \) 2>/dev/null | grep -v '/\.' || true); \
	if [ -n "$$C_FILES" ]; then \
		for file in $$C_FILES; do \
			if [ -f "$$file" ]; then \
				$(CLANG_FORMAT) -i "$$file" && FORMATTED_COUNT=$$((FORMATTED_COUNT + 1)); \
			fi; \
		done; \
		echo "$(GREEN)✓ Formatted $$FORMATTED_COUNT C/C++ files$(RESET)"; \
	else \
		echo "$(YELLOW)⚠ No C/C++ files found$(RESET)"; \
	fi; \
	echo "$(CYAN)Formatting XML files...$(RESET)"; \
	XML_COUNT=0; \
	XML_FILES=$$(find ui_xml -type f -name "*.xml" $(XML_FIND_PRUNE) 2>/dev/null || true); \
	if [ -n "$$XML_FILES" ]; then \
		if [ -x "$(VENV_PYTHON)" ] && $(VENV_PYTHON) -c "import lxml" 2>/dev/null; then \
			$(VENV_PYTHON) scripts/format-xml.py $$XML_FILES && \
			XML_COUNT=$$(echo "$$XML_FILES" | wc -w | tr -d ' '); \
			echo "$(GREEN)✓ Formatted $$XML_COUNT XML files$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Python venv or lxml not available - run 'make venv-setup'$(RESET)"; \
			echo "  Falling back to xmllint (basic indentation only)"; \
			for file in $$XML_FILES; do \
				if [ -f "$$file" ]; then \
					if xmllint --format "$$file" > "$$file.tmp" 2>/dev/null; then \
						mv "$$file.tmp" "$$file" && XML_COUNT=$$((XML_COUNT + 1)); \
					else \
						echo "$(RED)✗ Failed to format $$file$(RESET)"; \
						rm -f "$$file.tmp"; \
					fi; \
				fi; \
			done; \
			echo "$(GREEN)✓ Formatted $$XML_COUNT XML files (basic)$(RESET)"; \
		fi; \
	else \
		echo "$(YELLOW)⚠ No XML files found$(RESET)"; \
	fi; \
	echo ""; \
	echo "$(GREEN)$(BOLD)✓ Formatting complete!$(RESET)"; \
	echo "$(CYAN)Total files formatted:$(RESET) $$((FORMATTED_COUNT + XML_COUNT))"

# Format only staged files (useful before committing)
format-staged:
	$(ECHO) "$(CYAN)$(BOLD)Formatting staged files...$(RESET)"
	@FORMATTED_COUNT=0; \
	if ! command -v $(CLANG_FORMAT) >/dev/null 2>&1 && [ ! -x "$(CLANG_FORMAT)" ]; then \
		echo "$(RED)✗ clang-format not found$(RESET)"; \
		exit 1; \
	fi; \
	STAGED_C_FILES=$$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(c|cpp|h|mm)$$' | grep -v '^lib/' || true); \
	if [ -n "$$STAGED_C_FILES" ]; then \
		echo "$(CYAN)Formatting staged C/C++ files...$(RESET)"; \
		for file in $$STAGED_C_FILES; do \
			if [ -f "$$file" ]; then \
				$(CLANG_FORMAT) -i "$$file" && git add "$$file" && FORMATTED_COUNT=$$((FORMATTED_COUNT + 1)); \
				echo "  ✓ $$file"; \
			fi; \
		done; \
	fi; \
	STAGED_XML_FILES=$$(git diff --cached --name-only --diff-filter=ACM | grep '\.xml$$' | grep -v '^ui_xml/translations/' | grep -v '^android/' || true); \
	if [ -n "$$STAGED_XML_FILES" ]; then \
		echo "$(CYAN)Formatting staged XML files...$(RESET)"; \
		if [ -x "$(VENV_PYTHON)" ] && $(VENV_PYTHON) -c "import lxml" 2>/dev/null; then \
			for file in $$STAGED_XML_FILES; do \
				if [ -f "$$file" ]; then \
					$(VENV_PYTHON) scripts/format-xml.py "$$file" && git add "$$file" && FORMATTED_COUNT=$$((FORMATTED_COUNT + 1)); \
					echo "  ✓ $$file"; \
				fi; \
			done; \
		else \
			echo "$(YELLOW)⚠ Python venv or lxml not available - run 'make venv-setup'$(RESET)"; \
			for file in $$STAGED_XML_FILES; do \
				if [ -f "$$file" ]; then \
					if xmllint --format "$$file" > "$$file.tmp" 2>/dev/null; then \
						mv "$$file.tmp" "$$file" && git add "$$file" && FORMATTED_COUNT=$$((FORMATTED_COUNT + 1)); \
						echo "  ✓ $$file"; \
					else \
						rm -f "$$file.tmp"; \
					fi; \
				fi; \
			done; \
		fi; \
	fi; \
	if [ $$FORMATTED_COUNT -eq 0 ]; then \
		echo "$(GREEN)ℹ️  No staged files need formatting$(RESET)"; \
	else \
		echo ""; \
		echo "$(GREEN)$(BOLD)✓ Formatted $$FORMATTED_COUNT staged files$(RESET)"; \
	fi
