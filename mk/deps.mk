# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Dependency Management Module
# Handles dependency checking, installation, and libhv building

# Python virtual environment for build-time dependencies
VENV := .venv
VENV_PYTHON := $(VENV)/bin/python3
VENV_PIP := $(VENV)/bin/pip3

# Dependency checker - calls modular script for maintainability
# Set SKIP_OPTIONAL_DEPS=1 for cross-compilation builds (minimal check)
#
# The script is organized into functions by dependency category:
#   - check_essential: CC, CXX, make, pkg-config
#   - check_submodules: LVGL, spdlog, libhv, wpa_supplicant
#   - check_libraries: fmt, OpenSSL
#   - check_desktop_tools: SDL2, npm, python venv, clang-format (--minimal skips)
#   - check_canvas_libs: cairo, pango, libpng (--minimal skips)
#
# Dependency check stamp file location (must match rules.mk)
DEPS_CHECKED_MARKER := $(BUILD_DIR)/.deps-checked

check-deps:
	@mkdir -p $(BUILD_DIR)
ifeq ($(SKIP_OPTIONAL_DEPS),1)
	@CC="$(CC)" CXX="$(CXX)" ENABLE_SSL="$(ENABLE_SSL)" \
		LVGL_DIR="$(LVGL_DIR)" SPDLOG_DIR="$(SPDLOG_DIR)" \
		LIBHV_DIR="$(LIBHV_DIR)" WPA_DIR="$(WPA_DIR)" VENV="$(VENV)" \
		./scripts/check-deps.sh --minimal
else
	@CC="$(CC)" CXX="$(CXX)" ENABLE_SSL="$(ENABLE_SSL)" \
		LVGL_DIR="$(LVGL_DIR)" SPDLOG_DIR="$(SPDLOG_DIR)" \
		LIBHV_DIR="$(LIBHV_DIR)" WPA_DIR="$(WPA_DIR)" VENV="$(VENV)" \
		./scripts/check-deps.sh
endif
	@touch "$(DEPS_CHECKED_MARKER)"
	@# Auto-enable git hooks if not already set up
	@if [ -f ".githooks/pre-commit" ] && [ "$$(git config core.hooksPath 2>/dev/null)" != ".githooks" ]; then \
		git config core.hooksPath .githooks; \
		echo "$(GREEN)✓ Git pre-commit hooks enabled (auto-format with clang-format)$(RESET)"; \
	fi
	@# Auto-register the lesson-stats merge driver. .gitattributes marks
	@# .claude-recall/*.json as merge=recall-stats; with no driver configured
	@# git quietly falls back to a text merge, so the counters conflict any
	@# time two branches both cited a lesson. (Setting the .name key without
	@# .driver is worse - that combination aborts the merge outright.) Config
	@# is shared across worktrees, so registering once covers all of them.
	@if [ -f "scripts/recall_stats_merge.py" ] && [ -z "$$(git config merge.recall-stats.driver 2>/dev/null)" ]; then \
		git config merge.recall-stats.name "recall lesson-stats union merge"; \
		git config merge.recall-stats.driver "python3 scripts/recall_stats_merge.py %O %A %B"; \
		echo "$(GREEN)✓ Lesson-stats merge driver registered (counters merge additively)$(RESET)"; \
	fi

# Auto-install missing dependencies (interactive, requires confirmation)
install-deps:
	$(ECHO) "$(CYAN)$(BOLD)Dependency Auto-Installer$(RESET)"
	$(ECHO) ""
	@if [ "$(UNAME_S)" = "Darwin" ]; then \
		PLATFORM_TYPE="macOS"; \
		PKG_MGR="brew"; \
	elif [ -f /etc/debian_version ]; then \
		PLATFORM_TYPE="Debian/Ubuntu"; \
		PKG_MGR="apt"; \
	elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then \
		PLATFORM_TYPE="Fedora/RHEL"; \
		PKG_MGR="dnf"; \
	else \
		PLATFORM_TYPE="Unknown"; \
		PKG_MGR="unknown"; \
	fi; \
	add_pkg() { \
		case "$$1:$$PKG_MGR" in \
			sdl2:brew) echo "sdl2";; \
			sdl2:apt) echo "libsdl2-dev";; \
			sdl2:dnf) echo "SDL2-devel";; \
			npm:brew) echo "node";; \
			npm:*) echo "npm";; \
			python3-venv:apt) echo "python3-venv";; \
			python3-venv:dnf) echo "python3-libs";; \
			python3-venv:brew) echo "";; \
			clang-format:brew|clang-format:apt) echo "clang-format";; \
			clang-format:dnf) echo "clang-tools-extra";; \
			xmllint:brew) echo "libxml2";; \
			xmllint:apt) echo "libxml2-utils";; \
			xmllint:dnf) echo "libxml2";; \
			pkg-config:brew|pkg-config:apt) echo "pkg-config";; \
			pkg-config:dnf) echo "pkgconfig";; \
			fmt:brew) echo "fmt";; \
			fmt:apt) echo "libfmt-dev";; \
			fmt:dnf) echo "fmt-devel";; \
			cairo:brew) echo "cairo";; \
			cairo:apt) echo "libcairo2-dev";; \
			cairo:dnf) echo "cairo-devel";; \
			pango:brew) echo "pango";; \
			pango:apt) echo "libpango1.0-dev";; \
			pango:dnf) echo "pango-devel";; \
			libpng:brew) echo "libpng";; \
			libpng:apt) echo "libpng-dev";; \
			libpng:dnf) echo "libpng-devel";; \
			librsvg:brew) echo "librsvg";; \
			librsvg:apt) echo "librsvg2-dev";; \
			librsvg:dnf) echo "librsvg2-devel";; \
			openssl:brew) echo "openssl";; \
			openssl:apt) echo "libssl-dev";; \
			openssl:dnf) echo "openssl-devel";; \
			shellcheck:brew|shellcheck:apt) echo "shellcheck";; \
			shellcheck:dnf) echo "ShellCheck";; \
			bats:brew) echo "bats-core";; \
			bats:apt|bats:dnf) echo "bats";; \
			docker-buildx:brew) echo "docker-buildx";; \
			docker-buildx:apt) echo "";; \
			docker-buildx:dnf) echo "";; \
			*) echo "$$1";; \
		esac; \
	}; \
	echo "$(CYAN)Detected platform:$(RESET) $$PLATFORM_TYPE"; \
	echo ""; \
	INSTALL_NEEDED=0; TO_INSTALL=""; \
	if ! command -v sdl2-config >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg sdl2)"; \
	fi; \
	if ! command -v npm >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg npm)"; \
	fi; \
	if ! command -v python3 >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL python3"; \
	elif ! python3 -c "import venv, ensurepip" 2>/dev/null; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg python3-venv)"; \
	fi; \
	if ! command -v clang >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL clang"; \
	fi; \
	if ! command -v clang-format >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg clang-format)"; \
	fi; \
	if ! command -v xmllint >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg xmllint)"; \
	fi; \
	if ! command -v shellcheck >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg shellcheck)"; \
	fi; \
	if ! command -v bats >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg bats)"; \
	fi; \
	if ! command -v pkg-config >/dev/null 2>&1; then \
		INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg pkg-config)"; \
	else \
		if ! pkg-config --exists fmt 2>/dev/null; then \
			INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg fmt)"; \
		fi; \
		if ! pkg-config --exists cairo 2>/dev/null; then \
			INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg cairo)"; \
		fi; \
		if ! pkg-config --exists pango 2>/dev/null; then \
			INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg pango)"; \
		fi; \
		if ! pkg-config --exists libpng 2>/dev/null; then \
			INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg libpng)"; \
		fi; \
		if ! pkg-config --exists librsvg-2.0 2>/dev/null; then \
			INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg librsvg)"; \
		fi; \
		if [ "$(UNAME_S)" != "Darwin" ]; then \
			if ! pkg-config --exists openssl 2>/dev/null && ! pkg-config --exists libssl 2>/dev/null; then \
				if [ ! -f "/usr/include/openssl/ssl.h" ] && [ ! -f "/usr/local/include/openssl/ssl.h" ]; then \
					INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg openssl)"; \
				fi; \
			fi; \
		fi; \
	fi; \
	if command -v docker >/dev/null 2>&1; then \
		if ! docker buildx version >/dev/null 2>&1; then \
			BUILDX_PKG=$$(add_pkg docker-buildx); \
			if [ -n "$$BUILDX_PKG" ]; then \
				INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$BUILDX_PKG"; \
			else \
				echo "$(YELLOW)⚠ docker buildx not found (cross-compilation feature)$(RESET)"; \
				echo "  See: https://docs.docker.com/go/buildx/"; \
			fi; \
		fi; \
	fi; \
	if [ $$INSTALL_NEEDED -eq 0 ]; then \
		echo "$(GREEN)✓ No missing system dependencies$(RESET)"; \
	else \
		echo "$(YELLOW)The following packages will be installed:$(RESET)$$TO_INSTALL"; \
		echo ""; \
		if [ "$$PKG_MGR" = "brew" ]; then \
			CMD="brew install$$TO_INSTALL"; \
		elif [ "$$PKG_MGR" = "apt" ]; then \
			CMD="sudo apt update && sudo apt install -y$$TO_INSTALL"; \
		elif [ "$$PKG_MGR" = "dnf" ]; then \
			CMD="sudo dnf install -y$$TO_INSTALL"; \
		else \
			echo "$(RED)✗ Unknown package manager for platform: $$PLATFORM_TYPE$(RESET)"; \
			exit 1; \
		fi; \
		echo "$(CYAN)Command:$(RESET) $$CMD"; \
		echo ""; \
		read -p "$(YELLOW)Continue? [y/N]:$(RESET) " -n 1 -r; \
		echo ""; \
		if [[ $$REPLY =~ ^[Yy]$$ ]]; then \
			echo "$(CYAN)Installing...$(RESET)"; \
			eval $$CMD || { echo "$(RED)✗ Installation failed$(RESET)"; exit 1; }; \
			echo "$(GREEN)✓ System packages installed$(RESET)"; \
		else \
			echo "$(YELLOW)Installation cancelled$(RESET)"; \
			exit 1; \
		fi; \
	fi; \
	echo ""; \
	if [ ! -d "$(LVGL_DIR)/src" ]; then \
		echo "$(CYAN)Initializing git submodules...$(RESET)"; \
		git submodule update --init --recursive && echo "$(GREEN)✓ Submodules initialized$(RESET)" || echo "$(RED)✗ Submodule init failed$(RESET)"; \
	else \
		echo "$(GREEN)✓ Submodules already initialized$(RESET)"; \
	fi; \
	echo ""; \
	if ! command -v npm >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ npm not available - skipping npm install$(RESET)"; \
	elif [ ! -f "node_modules/.bin/lv_font_conv" ]; then \
		echo "$(CYAN)Installing npm packages (lv_font_conv, lv_img_conv)...$(RESET)"; \
		npm install && echo "$(GREEN)✓ npm packages installed$(RESET)" || echo "$(RED)✗ npm install failed$(RESET)"; \
	else \
		echo "$(GREEN)✓ npm packages already installed$(RESET)"; \
	fi; \
	echo ""; \
	if ! command -v python3 >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ python3 not available - skipping venv setup$(RESET)"; \
	else \
		NEED_VENV_SETUP=0; \
		if [ ! -f "$(VENV_PYTHON)" ]; then \
			NEED_VENV_SETUP=1; \
		elif ! $(VENV_PYTHON) -c "import png" >/dev/null 2>&1 || ! $(VENV_PYTHON) -c "import lz4" >/dev/null 2>&1; then \
			echo "$(YELLOW)⚠ Python packages missing in venv$(RESET)"; \
			NEED_VENV_SETUP=1; \
		fi; \
		if [ $$NEED_VENV_SETUP -eq 1 ]; then \
			echo "$(CYAN)Setting up Python venv and installing packages...$(RESET)"; \
			$(MAKE) venv-setup && echo "$(GREEN)✓ Python venv set up$(RESET)" || echo "$(RED)✗ venv setup failed$(RESET)"; \
		else \
			echo "$(GREEN)✓ Python venv already set up$(RESET)"; \
		fi; \
	fi; \
	echo ""; \
	if [ ! -f "$(LIBHV_LIB)" ]; then \
		echo "$(CYAN)Building libhv...$(RESET)"; \
		$(MAKE) libhv-build && echo "$(GREEN)✓ libhv built$(RESET)" || echo "$(RED)✗ libhv build failed$(RESET)"; \
	else \
		echo "$(GREEN)✓ libhv already built$(RESET)"; \
	fi; \
	echo ""; \
	echo "$(GREEN)$(BOLD)✓ Dependency installation complete!$(RESET)"; \
	echo "$(CYAN)Run$(RESET) $(YELLOW)make$(RESET) $(CYAN)to build the project$(RESET)"

# Build libhv (configure + compile)
# Supports native and cross-compilation via CROSS_COMPILE variable
# Output: $(BUILD_DIR)/lib/libhv.a for architecture isolation
# Note: libhv builds in-tree, so we must clean before cross-compilation
# to avoid mixing object files from different architectures

# =============================================================================
# Library Clean Targets
# =============================================================================
# Individual clean targets for forcing rebuilds after flag changes or
# architecture switches. The main 'clean' target calls these automatically,
# but they're useful for targeted rebuilds without full clean.

# Clean libhv build artifacts
libhv-clean:
	$(ECHO) "$(CYAN)Cleaning libhv build artifacts...$(RESET)"
	# -L follows symlinks so worktrees (where $(LIBHV_DIR) may be a symlink to
	# the main repo's lib/libhv) also get their stale artifacts cleaned.
	$(Q)find -L $(LIBHV_DIR) -type f \( -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dylib' \) -delete 2>/dev/null || true
	$(Q)rm -f $(BUILD_DIR)/lib/libhv.a 2>/dev/null || true
	$(ECHO) "$(GREEN)✓ libhv cleaned$(RESET)"

# Clean SDL2 build artifacts (CMake build directory)
# Note: SDL2_BUILD_DIR is only set when building SDL from submodule
sdl2-clean:
	$(ECHO) "$(CYAN)Cleaning SDL2 build artifacts...$(RESET)"
ifdef SDL2_BUILD_DIR
	$(Q)rm -rf $(SDL2_BUILD_DIR) 2>/dev/null || true
else
	$(Q)rm -rf lib/sdl2/build 2>/dev/null || true
endif
	$(ECHO) "$(GREEN)✓ SDL2 cleaned$(RESET)"

# Clean LVGL compiled objects (forces full recompile)
lvgl-clean:
	$(ECHO) "$(CYAN)Cleaning LVGL build artifacts...$(RESET)"
	$(Q)rm -rf $(OBJ_DIR)/lvgl 2>/dev/null || true
	$(ECHO) "$(GREEN)✓ LVGL cleaned$(RESET)"

# Clean libnl build artifacts
libnl-clean:
	$(ECHO) "$(CYAN)Cleaning libnl build artifacts...$(RESET)"
	$(Q)if [ -d "lib/libnl" ] && [ -f "lib/libnl/Makefile" ]; then \
		$(MAKE) -C lib/libnl distclean 2>/dev/null || true; \
	fi
	$(Q)rm -f $(BUILD_DIR)/lib/libnl*.a 2>/dev/null || true
	$(ECHO) "$(GREEN)✓ libnl cleaned$(RESET)"

# Clean wpa_supplicant build artifacts (Linux only)
wpa-clean:
ifneq ($(UNAME_S),Darwin)
	$(ECHO) "$(CYAN)Cleaning wpa_supplicant build artifacts...$(RESET)"
	$(Q)if [ -d "$(WPA_DIR)/wpa_supplicant" ]; then \
		$(MAKE) -C $(WPA_DIR)/wpa_supplicant clean 2>/dev/null || true; \
	fi
	$(Q)rm -f $(BUILD_DIR)/lib/libwpa_client.a 2>/dev/null || true
	$(ECHO) "$(GREEN)✓ wpa_supplicant cleaned$(RESET)"
else
	@echo "wpa_supplicant not used on macOS"
endif

# Clean all submodule libraries
libs-clean: libhv-clean sdl2-clean lvgl-clean libnl-clean wpa-clean
	$(ECHO) "$(GREEN)✓ All library artifacts cleaned$(RESET)"

# =============================================================================
# Library Build Targets
# =============================================================================

# Per-arch out-of-tree libhv objects + archive (prestonbrown/helixscreen#1058).
# libhv used to compile objects IN-TREE (lib/libhv/base/hsocket.o etc.) and link
# lib/libhv/lib/libhv.a, so native (x86_64) and cross (aarch64/arm/mips) builds
# clobbered each other's .o AND the archive. We patched libhv's Makefile.in to
# honor OBJDIR/LIBDIR, so each arch's objects land under its own $(BUILD_DIR):
#   build/libhv-objs/ (native), build/pi/libhv-objs/ (pi), etc.
# This makes the stomping structurally impossible and retires both the in-tree
# clean step below and the cross-compile lock (mk/cross.mk).
LIBHV_OBJDIR := $(abspath $(BUILD_DIR)/libhv-objs)

libhv-build:
	$(ECHO) "$(CYAN)Building libhv...$(RESET)"
	$(Q)mkdir -p $(BUILD_DIR)/lib $(LIBHV_OBJDIR)/lib
ifneq ($(CROSS_COMPILE),)
	# Cross-compilation mode.
	# Pass cross-compiler to configure and make.
	# When SSL is enabled, map target -> toolchain OpenSSL prefix.
	$(Q)OPENSSL_PREFIX=""; \
	OPENSSL_INC=""; \
	OPENSSL_LIB_DIR=""; \
	OPENSSL_ARCHIVES=""; \
	if [ "$(ENABLE_SSL)" = "yes" ]; then \
		case "$(PLATFORM_TARGET)" in \
			ad5m|cc1) OPENSSL_PREFIX="/opt/arm-toolchain/arm-none-linux-gnueabihf" ;; \
			ad5x) OPENSSL_PREFIX="/opt/mipsel-zmod-ad5x/mipsel-buildroot-linux-gnu/sysroot/usr" ;; \
			mips|k1) OPENSSL_PREFIX="/opt/mips-toolchain/mipsel-buildroot-linux-musl/sysroot/usr" ;; \
			k2) OPENSSL_PREFIX="/opt/arm-musl-toolchain/arm-buildroot-linux-musleabihf/sysroot/usr" ;; \
		esac; \
		if [ -n "$$OPENSSL_PREFIX" ]; then \
			OPENSSL_INC=" -I$$OPENSSL_PREFIX/include"; \
			OPENSSL_LIB_DIR="-L$$OPENSSL_PREFIX/lib"; \
			if [ -f "$$OPENSSL_PREFIX/lib/libssl.a" ] && [ -f "$$OPENSSL_PREFIX/lib/libcrypto.a" ]; then \
				OPENSSL_ARCHIVES="$$OPENSSL_PREFIX/lib/libssl.a $$OPENSSL_PREFIX/lib/libcrypto.a"; \
			fi; \
			case "$(PLATFORM_TARGET)" in \
				ad5x) OPENSSL_ARCHIVES="" ;; \
			esac; \
		fi; \
	fi; \
	(cd $(LIBHV_DIR) && \
		CC="$(CC)" CXX="$(CXX)" AR="$(AR)" \
		CFLAGS="$(TARGET_CFLAGS) $(SANITIZE_FLAGS)$$OPENSSL_INC" \
		CXXFLAGS="$(TARGET_CFLAGS) $(SANITIZE_FLAGS)$$OPENSSL_INC" \
		LDFLAGS="$$OPENSSL_LIB_DIR $(SANITIZE_FLAGS)" \
		./configure --with-http-client $(if $(filter yes,$(ENABLE_SSL)),--with-openssl)); \
	# libhv's nested make has been flaky under cross toolchains when run with
	# inherited/parallel jobserver flags; keep only this sub-build serialized.
	# LDFLAGS= override prevents the parent's link flags (which on this project
	# include positional paths like `build/lib/libhv.a` and
	# `build/lib/libwpa_client.a`) from leaking in via the environment — those
	# paths resolve relative to libhv's own working directory, where they don't
	# exist, breaking libhv's internal example/test links under test-asan/tsan.
	if [ -n "$$OPENSSL_ARCHIVES" ]; then \
		CC="$(CC)" CXX="$(CXX)" AR="$(AR)" MAKEFLAGS= $(MAKE) -j1 -C $(LIBHV_DIR) OBJDIR="$(LIBHV_OBJDIR)" LIBDIR="$(LIBHV_OBJDIR)/lib" LDFLAGS= LIBHV_TARGET_TYPE=STATIC OPENSSL_LIBS="$$OPENSSL_ARCHIVES" libhv; \
	else \
		CC="$(CC)" CXX="$(CXX)" AR="$(AR)" MAKEFLAGS= $(MAKE) -j1 -C $(LIBHV_DIR) OBJDIR="$(LIBHV_OBJDIR)" LIBDIR="$(LIBHV_OBJDIR)/lib" LDFLAGS= LIBHV_TARGET_TYPE=STATIC libhv; \
	fi
else ifeq ($(UNAME_S),Darwin)
	$(Q)cd $(LIBHV_DIR) && \
		MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) \
		CFLAGS="$(MACOS_DEPLOYMENT_TARGET)" \
		CXXFLAGS="$(MACOS_DEPLOYMENT_TARGET)" \
		./configure --with-http-client
	# LIBHV_TARGET_TYPE=STATIC skips the dylib link, which would otherwise fail
	# because libhv's Makefile doesn't pass `-framework CoreFoundation -framework
	# Security` (needed by ssl/appletls.o). We only consume libhv as a static
	# archive anyway, so the dylib is just dead weight that breaks the build.
	$(Q)MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) $(MAKE) -C $(LIBHV_DIR) OBJDIR="$(LIBHV_OBJDIR)" LIBDIR="$(LIBHV_OBJDIR)/lib" LDFLAGS= LIBHV_TARGET_TYPE=STATIC libhv
else
	$(Q)cd $(LIBHV_DIR) && ./configure --with-http-client $(if $(filter yes,$(ENABLE_SSL)),--with-openssl)
	# LDFLAGS= override: see cross-compile branch above for rationale. Critical
	# for test-asan/test-tsan where the parent invokes us with the project's
	# full LDFLAGS containing `build/lib/lib*.a` paths.
	$(Q)$(MAKE) -C $(LIBHV_DIR) OBJDIR="$(LIBHV_OBJDIR)" LIBDIR="$(LIBHV_OBJDIR)/lib" LDFLAGS= libhv
endif
	# Copy built library from the per-arch out-of-tree objdir to the architecture-
	# specific output directory. Falls back to the legacy in-tree locations so an
	# unexpected build path still resolves.
	$(Q)cp $(LIBHV_OBJDIR)/lib/libhv.a $(BUILD_DIR)/lib/libhv.a 2>/dev/null || \
		cp $(LIBHV_DIR)/lib/libhv.a $(BUILD_DIR)/lib/libhv.a 2>/dev/null || \
		cp $(LIBHV_DIR)/libhv.a $(BUILD_DIR)/lib/libhv.a
	@# libhv's own build installs the include/hv/ copies our -isystem path
	@# resolves to, so the tracked header set is only complete once this ran.
	$(call record_abi_stamp)
	$(ECHO) "$(GREEN)✓ libhv built: $(BUILD_DIR)/lib/libhv.a$(RESET)"

# Build libnl from submodule (autotools)
# Required for WiFi backend on Linux/embedded targets
# Output: $(BUILD_DIR)/lib/libnl-3.a and libnl-genl-3.a
#
# Note: libnl uses autogen.sh to create configure script, then standard autotools flow
# We build static libraries only (--enable-static --disable-shared)
LIBNL_DIR := lib/libnl
LIBNL_PREFIX := $(abspath $(BUILD_DIR))/libnl-install

libnl-build:
	$(ECHO) "$(CYAN)Building libnl...$(RESET)"
	$(Q)mkdir -p $(BUILD_DIR)/lib $(LIBNL_PREFIX)
ifneq ($(CROSS_COMPILE),)
	# Cross-compilation mode
	$(ECHO) "$(YELLOW)→ Cross-compiling libnl for $(CROSS_COMPILE)...$(RESET)"
	$(Q)if [ ! -f "$(LIBNL_DIR)/configure" ]; then \
		echo "$(CYAN)→ Running autogen.sh...$(RESET)"; \
		cd $(LIBNL_DIR) && PATH="/usr/bin:$$PATH" ./autogen.sh; \
	fi
	$(Q)if [ -f "$(LIBNL_DIR)/Makefile" ]; then \
		$(MAKE) -C $(LIBNL_DIR) distclean 2>/dev/null || true; \
	fi
	@# Apply patches for GCC 14+ compatibility (implicit function declaration is now an error)
	$(Q)for p in patches/libnl-*.patch; do \
		[ -f "$$p" ] && git -C $(LIBNL_DIR) apply --check "../../$$p" 2>/dev/null && \
			git -C $(LIBNL_DIR) apply "../../$$p" && \
			echo "$(CYAN)→ Applied $$(basename $$p)$(RESET)" || true; \
	done
	$(Q)cd $(LIBNL_DIR) && \
		CC="$(CC)" \
		AR="$(AR)" \
		RANLIB="$(RANLIB)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		./configure \
			--host=$(patsubst %-,%,$(CROSS_COMPILE)) \
			--prefix=$(LIBNL_PREFIX) \
			--enable-static \
			--disable-shared \
			--disable-cli \
			--disable-pthreads \
			--disable-debug
	@# Build only the libraries we need (libnl-3 and libnl-genl-3)
	@# Skip xfrm/idiag/nf/route to avoid build errors and reduce size
	@#
	@# SERIAL ON PURPOSE — do not "optimize" this back to -j$(nproc).
	@# libnl's generated Makefile does not fully order its libtool objects
	@# against the .la link, so with enough jobs `CCLD lib/libnl-3.la` starts
	@# before every lib/libnl_3_la-*.lo exists and ar dies with
	@#   ar: lib/libnl_3_la-hash.o: No such file or directory
	@# It is load-dependent, so it passes on small builders and fails on big
	@# ones (seen on a 32-core host, never on 8). libnl is a couple of dozen
	@# small C files; serializing costs seconds and removes the flake.
	$(Q)$(MAKE) -C $(LIBNL_DIR) -j1 lib/libnl-3.la lib/libnl-genl-3.la
	@# Install just what we need - libs, pkgconfig, and headers
	$(Q)mkdir -p $(LIBNL_PREFIX)/lib $(LIBNL_PREFIX)/lib/pkgconfig $(LIBNL_PREFIX)/include/libnl3
	$(Q)cp $(LIBNL_DIR)/lib/.libs/libnl-3.a $(LIBNL_DIR)/lib/.libs/libnl-genl-3.a $(LIBNL_PREFIX)/lib/
	$(Q)cp $(LIBNL_DIR)/libnl-3.0.pc $(LIBNL_DIR)/libnl-genl-3.0.pc $(LIBNL_PREFIX)/lib/pkgconfig/
	$(Q)cp -r $(LIBNL_DIR)/include/netlink $(LIBNL_PREFIX)/include/libnl3/
	# Copy libraries to build output
	$(Q)cp $(LIBNL_PREFIX)/lib/libnl-3.a $(BUILD_DIR)/lib/
	$(Q)cp $(LIBNL_PREFIX)/lib/libnl-genl-3.a $(BUILD_DIR)/lib/
	$(ECHO) "$(GREEN)✓ libnl built (cross-compiled): $(BUILD_DIR)/lib/libnl-3.a$(RESET)"
else ifeq ($(UNAME_S),Linux)
	# Native Linux build (for testing, normally use system package)
	$(ECHO) "$(YELLOW)→ Building libnl natively (prefer system package for dev)...$(RESET)"
	$(Q)if [ ! -f "$(LIBNL_DIR)/configure" ]; then \
		cd $(LIBNL_DIR) && ./autogen.sh; \
	fi
	$(Q)if [ -f "$(LIBNL_DIR)/Makefile" ]; then \
		$(MAKE) -C $(LIBNL_DIR) distclean 2>/dev/null || true; \
	fi
	$(Q)cd $(LIBNL_DIR) && ./configure \
		--prefix=$(LIBNL_PREFIX) \
		--enable-static \
		--disable-shared \
		--disable-cli
	@# Serial for the same reason as the cross-compile path above.
	$(Q)$(MAKE) -C $(LIBNL_DIR) -j1
	$(Q)$(MAKE) -C $(LIBNL_DIR) install
	$(Q)cp $(LIBNL_PREFIX)/lib/libnl-3.a $(BUILD_DIR)/lib/
	$(Q)cp $(LIBNL_PREFIX)/lib/libnl-genl-3.a $(BUILD_DIR)/lib/
	$(ECHO) "$(GREEN)✓ libnl built: $(BUILD_DIR)/lib/libnl-3.a$(RESET)"
else
	$(ECHO) "$(YELLOW)⚠ libnl not needed on macOS (WiFi uses native APIs)$(RESET)"
endif

# Build SDL2 from submodule (CMake build)
sdl2-build:
	$(ECHO) "$(CYAN)Building SDL2 from submodule...$(RESET)"
	$(Q)mkdir -p $(SDL2_BUILD_DIR)
ifeq ($(UNAME_S),Darwin)
	$(Q)cd $(SDL2_BUILD_DIR) && \
		MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) \
		cmake .. \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) \
			-DSDL_SHARED=OFF \
			-DSDL_STATIC=ON \
			-DSDL_TEST=OFF \
			-DSDL_TESTS=OFF
	$(Q)MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) cmake --build $(SDL2_BUILD_DIR) --config Release
else
	$(Q)cd $(SDL2_BUILD_DIR) && \
		cmake .. \
			-DCMAKE_BUILD_TYPE=Release \
			-DSDL_SHARED=OFF \
			-DSDL_STATIC=ON \
			-DSDL_TEST=OFF \
			-DSDL_TESTS=OFF
	$(Q)cmake --build $(SDL2_BUILD_DIR) --config Release
endif
	$(ECHO) "$(GREEN)✓ SDL2 built successfully$(RESET)"

$(SDL2_LIB):
	$(Q)$(MAKE) sdl2-build

# Build wpa_supplicant client library (Linux and cross-compilation targets)
# This is needed for WiFi control on embedded Linux systems
# Output: $(BUILD_DIR)/lib/libwpa_client.a for architecture isolation
#
# NOTE: Make conditionals (ifneq/else/endif) CANNOT be used inside recipes!
# They are processed at parse time, not run time. Use shell conditionals instead.

# wpa_supplicant builds its objects INSIDE the submodule tree, which every target
# in this checkout shares, and nothing on that path is platform-qualified. So the
# second target to build re-archives the first one's objects: an ARM object lands
# in an x86 libwpa_client.a and ld reports "relocations in generic ELF (EM: 40)",
# or links quietly when the two arches are close enough to fool it.
#
# The stamp records who owns the objects currently in the tree; a different owner
# cleans before building. The key is PLATFORM_TARGET *and* the compiler triple,
# and it needs both halves: pi, pi-fbdev and pi-both all resolve to
# aarch64-linux-gnu so the triple alone would let them inherit each other's
# objects built under different -march flags, while PLATFORM_TARGET alone would
# not notice the toolchain under one platform name being swapped.
# (prestonbrown/helixscreen#1481)
WPA_TOOLCHAIN_STAMP := $(WPA_DIR)/.helix-toolchain
define wpa-clean-if-foreign
	$(Q)want="$(PLATFORM_TARGET)|$$($(CC) -dumpmachine 2>/dev/null || echo unknown)"; \
	have=$$(cat $(WPA_TOOLCHAIN_STAMP) 2>/dev/null || echo none); \
	if [ "$$want" != "$$have" ]; then \
		if [ "$$have" != none ]; then \
			echo "$(YELLOW)→ wpa_supplicant objects belong to $$have — cleaning for $$want$(RESET)"; \
		fi; \
		$(MAKE) -C $(WPA_DIR)/wpa_supplicant clean >/dev/null 2>&1 || true; \
		rm -rf $(WPA_DIR)/build; \
		rm -f $(WPA_DIR)/wpa_supplicant/libwpa_client.a; \
		echo "$$want" > $(WPA_TOOLCHAIN_STAMP); \
	fi
endef

# Linux native build (not macOS, not cross-compiling)
ifeq ($(UNAME_S),Linux)
ifndef CROSS_COMPILE
$(WPA_CLIENT_LIB): | $(BUILD_DIR)/lib
	$(ECHO) "$(BOLD)$(BLUE)[WPA]$(RESET) Building wpa_supplicant client library..."
	$(Q)if [ ! -f "$(WPA_DIR)/wpa_supplicant/.config" ]; then \
		if [ -f "$(WPA_DIR)/wpa_supplicant/defconfig" ]; then \
			echo "$(CYAN)→ Creating .config from defconfig...$(RESET)"; \
			cp "$(WPA_DIR)/wpa_supplicant/defconfig" "$(WPA_DIR)/wpa_supplicant/.config"; \
		else \
			echo "$(RED)✗ wpa_supplicant/.config not found and no defconfig$(RESET)"; \
			echo "  Expected at: $(WPA_DIR)/wpa_supplicant/.config"; \
			exit 1; \
		fi; \
	fi
	$(call wpa-clean-if-foreign)
	$(Q)$(MAKE) -C $(WPA_DIR)/wpa_supplicant libwpa_client.a
	$(Q)rm -f $(BUILD_DIR)/lib/libwpa_client.a 2>/dev/null || true
	$(Q)cp $(WPA_DIR)/wpa_supplicant/libwpa_client.a $(BUILD_DIR)/lib/libwpa_client.a
	$(Q)$(RANLIB) $(BUILD_DIR)/lib/libwpa_client.a
	$(ECHO) "$(GREEN)✓ libwpa_client.a built: $(BUILD_DIR)/lib/libwpa_client.a$(RESET)"
endif
endif

# Cross-compilation build (from any host to Linux target)
ifdef CROSS_COMPILE
# wpa_supplicant CFLAGS: Strip LTO and section flags since wpa_supplicant is a separate
# build system that doesn't participate in our LTO linking. LTO-compiled .a files contain
# GIMPLE IR instead of machine code, which breaks linking when mixed with our LTO objects.
# Use EXTRA_CFLAGS since wpa_supplicant's Makefile appends EXTRA_CFLAGS to its internal flags.
WPA_CFLAGS := $(filter-out -flto -ffunction-sections -fdata-sections,$(TARGET_CFLAGS))

# libnl paths for wpa_supplicant (cross-compilation uses our built libnl)
LIBNL_INC := -I$(abspath $(BUILD_DIR))/libnl-install/include/libnl3
LIBNL_LIB := -L$(abspath $(BUILD_DIR))/libnl-install/lib

# wpa_supplicant depends on libnl being built first (order-only prerequisite)
$(WPA_CLIENT_LIB): | $(BUILD_DIR)/lib libnl-build
	$(ECHO) "$(BOLD)$(BLUE)[WPA]$(RESET) Building wpa_supplicant client library (cross-compile)..."
	$(Q)if [ ! -f "$(WPA_DIR)/wpa_supplicant/.config" ]; then \
		if [ -f "$(WPA_DIR)/wpa_supplicant/defconfig" ]; then \
			echo "$(CYAN)→ Creating .config from defconfig...$(RESET)"; \
			cp "$(WPA_DIR)/wpa_supplicant/defconfig" "$(WPA_DIR)/wpa_supplicant/.config"; \
		else \
			echo "$(RED)✗ wpa_supplicant/.config not found and no defconfig$(RESET)"; \
			exit 1; \
		fi; \
	fi
	$(call wpa-clean-if-foreign)
	@# Use env -u to unset inherited CFLAGS from parent make, then set clean values
	@# wpa_supplicant uses EXTRA_CFLAGS for additional flags
	@# LIBNL_INC/LIBNL_LIB point to our cross-compiled libnl
	$(Q)env -u CFLAGS CC="$(CC)" EXTRA_CFLAGS="$(WPA_CFLAGS) $(LIBNL_INC)" LDFLAGS="$(LIBNL_LIB)" \
		$(MAKE) -C $(WPA_DIR)/wpa_supplicant libwpa_client.a
	$(Q)rm -f $(BUILD_DIR)/lib/libwpa_client.a 2>/dev/null || true
	$(Q)cp $(WPA_DIR)/wpa_supplicant/libwpa_client.a $(BUILD_DIR)/lib/libwpa_client.a
	$(Q)$(RANLIB) $(BUILD_DIR)/lib/libwpa_client.a
	$(ECHO) "$(GREEN)✓ libwpa_client.a built: $(BUILD_DIR)/lib/libwpa_client.a$(RESET)"
endif

# Python virtual environment setup
venv-setup:
	$(ECHO) "$(CYAN)Setting up Python virtual environment...$(RESET)"
	$(Q)if [ ! -f "$(VENV_PIP)" ]; then \
		rm -rf $(VENV) 2>/dev/null; \
		python3 -m venv $(VENV) || { \
			echo "$(RED)✗ Failed to create venv$(RESET)"; \
			echo "$(YELLOW)On Debian/Ubuntu, install: sudo apt install python3-venv$(RESET)"; \
			exit 1; \
		}; \
		if [ ! -f "$(VENV_PIP)" ]; then \
			echo "$(RED)✗ venv created but pip missing - python3-venv may not be fully installed$(RESET)"; \
			echo "$(YELLOW)On Debian/Ubuntu, install: sudo apt install python3-venv$(RESET)"; \
			rm -rf $(VENV); \
			exit 1; \
		fi; \
		echo "$(GREEN)✓ Virtual environment created$(RESET)"; \
	else \
		echo "$(GREEN)✓ Virtual environment exists$(RESET)"; \
	fi
	$(Q)if [ -f "requirements.txt" ]; then \
		echo "$(CYAN)Installing Python packages from requirements.txt...$(RESET)"; \
		$(VENV_PIP) install -r requirements.txt || { echo "$(RED)✗ Failed to install requirements$(RESET)"; exit 1; }; \
		echo "$(GREEN)✓ Python packages installed$(RESET)"; \
	else \
		echo "$(YELLOW)⚠ requirements.txt not found$(RESET)"; \
	fi
	$(ECHO) "$(GREEN)✓ Python venv setup complete$(RESET)"

$(VENV_PYTHON):
	$(Q)$(MAKE) venv-setup

# ============================================================================
# Git Hooks Setup
# ============================================================================

.PHONY: setup-hooks
setup-hooks:
	$(ECHO) "$(CYAN)Setting up git hooks...$(RESET)"
	$(Q)if [ -f ".githooks/pre-commit" ]; then \
		git config core.hooksPath .githooks; \
		echo "$(GREEN)✓ Git hooks enabled (.githooks/pre-commit)$(RESET)"; \
		echo "$(CYAN)  Pre-commit will auto-format C/C++ files with clang-format$(RESET)"; \
	else \
		echo "$(RED)✗ .githooks/pre-commit not found$(RESET)"; \
		exit 1; \
	fi

# ============================================================================
# Build/Dependency Help
# ============================================================================

.PHONY: libhv-clean sdl2-clean lvgl-clean libnl-clean wpa-clean libs-clean distclean help-build
help-build:
	@if [ -t 1 ] && [ -n "$(TERM)" ] && [ "$(TERM)" != "dumb" ]; then \
		B='$(BOLD)'; G='$(GREEN)'; Y='$(YELLOW)'; C='$(CYAN)'; X='$(RESET)'; \
	else \
		B=''; G=''; Y=''; C=''; X=''; \
	fi; \
	echo "$${B}Build & Dependency Targets$${X}"; \
	echo ""; \
	echo "$${C}Core Build:$${X}"; \
	echo "  $${G}all$${X}                 - Build main binary (default)"; \
	echo "  $${G}build$${X}               - Clean build with progress"; \
	echo "  $${G}clean$${X}               - Remove build artifacts (keeps deps)"; \
	echo "  $${G}distclean$${X}           - Deep clean (fresh checkout state)"; \
	echo "  $${G}run$${X}                 - Build and run the UI"; \
	echo ""; \
	echo "$${C}Dependency Management:$${X}"; \
	echo "  $${G}check-deps$${X}          - Verify all dependencies installed"; \
	echo "  $${G}install-deps$${X}        - Auto-install missing dependencies"; \
	echo "  $${G}libhv-build$${X}         - Build libhv WebSocket library"; \
	echo "  $${G}sdl2-build$${X}          - Build SDL2 from submodule"; \
	echo "  $${G}libhv-clean$${X}         - Clean libhv artifacts (force rebuild)"; \
	echo "  $${G}sdl2-clean$${X}          - Clean SDL2 build directory"; \
	echo "  $${G}lvgl-clean$${X}          - Clean LVGL compiled objects"; \
	echo "  $${G}libs-clean$${X}          - Clean all library artifacts"; \
	echo "  $${G}venv-setup$${X}          - Set up Python virtual environment"; \
	echo "  $${G}setup-hooks$${X}         - Enable git pre-commit hooks (clang-format)"; \
	echo ""; \
	echo "$${C}Patches:$${X}"; \
	echo "  $${G}apply-patches$${X}       - Apply LVGL/libhv patches (idempotent)"; \
	echo "  $${G}reset-patches$${X}       - Reset patched files to upstream"; \
	echo "  $${G}reapply-patches$${X}     - Force reapply all patches"; \
	echo ""; \
	echo "$${C}Code Quality:$${X}"; \
	echo "  $${G}format$${X}              - Auto-format all C/C++ and XML"; \
	echo "  $${G}format-staged$${X}       - Format only staged files"; \
	echo "  $${G}compile_commands$${X}    - Generate compile_commands.json"; \
	echo ""; \
	echo "$${C}Libraries (embedded targets):$${X}"; \
	echo "  $${G}display-lib$${X}         - Build display backend library"; \
	echo "  $${G}splash$${X}              - Build splash screen binary"; \
	echo ""; \
	echo "$${C}Build Options:$${X}"; \
	echo "  $${Y}V=1$${X}                 - Verbose (show compiler commands)"; \
	echo "  $${Y}JOBS=N$${X}              - Parallel job count"; \
	echo "  $${Y}NO_COLOR=1$${X}          - Disable colored output"; \
	echo "  $${Y}ENABLE_GLES_3D$${X}=no   - Disable 3D GLES rendering"
