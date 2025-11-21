# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Dependency Management Module
# Handles dependency checking, installation, and libhv building

# Dependency checker - comprehensive validation with install instructions
check-deps:
	$(ECHO) "$(CYAN)Checking build dependencies...$(RESET)"
	@ERROR=0; WARN=0; MISSING_DEPS=""; \
	if ! command -v $(CC) >/dev/null 2>&1; then \
		echo "$(RED)✗ $(CC) not found$(RESET)"; ERROR=1; \
		MISSING_DEPS="$$MISSING_DEPS $(CC)"; \
		echo "  Install: $(YELLOW)sudo apt install clang$(RESET) or $(YELLOW)sudo apt install gcc$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install clang$(RESET) or $(YELLOW)sudo dnf install gcc$(RESET) (Fedora/RHEL)"; \
		echo "         $(YELLOW)brew install llvm$(RESET) or $(YELLOW)xcode-select --install$(RESET) (macOS)"; \
	else \
		echo "$(GREEN)✓ $(CC) found:$(RESET) $$($(CC) --version | head -n1)"; \
	fi; \
	if ! command -v $(CXX) >/dev/null 2>&1; then \
		echo "$(RED)✗ $(CXX) not found$(RESET)"; ERROR=1; \
		MISSING_DEPS="$$MISSING_DEPS $(CXX)"; \
		echo "  Install: $(YELLOW)sudo apt install clang$(RESET) or $(YELLOW)sudo apt install g++$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install clang$(RESET) or $(YELLOW)sudo dnf install gcc-c++$(RESET) (Fedora/RHEL)"; \
		echo "         $(YELLOW)brew install llvm$(RESET) or $(YELLOW)xcode-select --install$(RESET) (macOS)"; \
	else \
		echo "$(GREEN)✓ $(CXX) found:$(RESET) $$($(CXX) --version | head -n1)"; \
	fi; \
	if [ "$$(uname -s)" = "Darwin" ]; then \
		MACOS_VERSION=$$(sw_vers -productVersion 2>/dev/null | cut -d. -f1-2); \
		REQUIRED_VERSION="10.15"; \
		if [ -n "$$MACOS_VERSION" ]; then \
			MAJOR=$$(echo "$$MACOS_VERSION" | cut -d. -f1); \
			MINOR=$$(echo "$$MACOS_VERSION" | cut -d. -f2); \
			REQ_MAJOR=$$(echo "$$REQUIRED_VERSION" | cut -d. -f1); \
			REQ_MINOR=$$(echo "$$REQUIRED_VERSION" | cut -d. -f2); \
			if [ "$$MAJOR" -lt "$$REQ_MAJOR" ] || { [ "$$MAJOR" -eq "$$REQ_MAJOR" ] && [ "$$MINOR" -lt "$$REQ_MINOR" ]; }; then \
				echo "$(RED)✗ macOS version $$MACOS_VERSION is too old$(RESET)"; ERROR=1; \
				echo "  Required: macOS $$REQUIRED_VERSION (Catalina) or newer"; \
				echo "  Reason: CoreWLAN/CoreLocation modern APIs for WiFi support"; \
			else \
				echo "$(GREEN)✓ macOS version $$MACOS_VERSION >= $$REQUIRED_VERSION$(RESET)"; \
			fi; \
		fi; \
	fi; \
	if command -v sdl2-config >/dev/null 2>&1; then \
		echo "$(GREEN)✓ SDL2:$(RESET) Using system version $$(sdl2-config --version)"; \
	else \
		echo "$(GREEN)✓ SDL2:$(RESET) Will build from submodule (sdl2/)"; \
		if ! command -v cmake >/dev/null 2>&1; then \
			echo "$(RED)✗ cmake not found (required for SDL2 build)$(RESET)"; ERROR=1; \
			MISSING_DEPS="$$MISSING_DEPS cmake"; \
			echo "  Install: $(YELLOW)brew install cmake$(RESET) (macOS)"; \
			echo "         $(YELLOW)sudo apt install cmake$(RESET) (Debian/Ubuntu)"; \
			echo "         $(YELLOW)sudo dnf install cmake$(RESET) (Fedora/RHEL)"; \
		else \
			echo "$(GREEN)✓ cmake found:$(RESET) $$(cmake --version | head -n1)"; \
		fi; \
	fi; \
	if ! command -v make >/dev/null 2>&1; then \
		echo "$(RED)✗ make not found$(RESET)"; ERROR=1; \
		MISSING_DEPS="$$MISSING_DEPS make"; \
		echo "  Install: $(YELLOW)sudo apt install make$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install make$(RESET) (Fedora/RHEL)"; \
		echo "         $(YELLOW)xcode-select --install$(RESET) (macOS)"; \
	else \
		echo "$(GREEN)✓ make found:$(RESET) $$(make --version | head -n1)"; \
	fi; \
	if ! command -v python3 >/dev/null 2>&1; then \
		echo "$(RED)✗ python3 not found$(RESET)"; ERROR=1; \
		MISSING_DEPS="$$MISSING_DEPS python3"; \
		echo "  Install: $(YELLOW)sudo apt install python3$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install python3$(RESET) (Fedora/RHEL)"; \
		echo "         $(YELLOW)brew install python3$(RESET) (macOS)"; \
	else \
		echo "$(GREEN)✓ python3 found:$(RESET) $$(python3 --version)"; \
	fi; \
	if ! command -v npm >/dev/null 2>&1; then \
		echo "$(RED)✗ npm not found$(RESET) (needed for font generation)"; ERROR=1; \
		MISSING_DEPS="$$MISSING_DEPS npm"; \
		echo "  Install: $(YELLOW)brew install node$(RESET) (macOS)"; \
		echo "         $(YELLOW)sudo apt install npm$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install npm$(RESET) (Fedora/RHEL)"; \
	else \
		echo "$(GREEN)✓ npm found:$(RESET) $$(npm --version)"; \
		if [ ! -f "node_modules/.bin/lv_font_conv" ]; then \
			echo "$(YELLOW)⚠ lv_font_conv not installed$(RESET)"; WARN=1; \
			echo "  Run: $(YELLOW)npm install$(RESET)"; \
		else \
			echo "$(GREEN)✓ lv_font_conv installed$(RESET)"; \
		fi; \
	fi; \
	if ! command -v clang-format >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ clang-format not found$(RESET) (needed for code formatting)"; WARN=1; \
		echo "  Install: $(YELLOW)brew install clang-format$(RESET) (macOS)"; \
		echo "         $(YELLOW)sudo apt install clang-format$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install clang-tools-extra$(RESET) (Fedora/RHEL)"; \
	else \
		echo "$(GREEN)✓ clang-format found:$(RESET) $$(clang-format --version | head -n1)"; \
	fi; \
	if ! command -v xmllint >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ xmllint not found$(RESET) (needed for XML validation/formatting)"; WARN=1; \
		echo "  Install: $(YELLOW)brew install libxml2$(RESET) (macOS)"; \
		echo "         $(YELLOW)sudo apt install libxml2-utils$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install libxml2$(RESET) (Fedora/RHEL)"; \
	else \
		echo "$(GREEN)✓ xmllint found$(RESET)"; \
	fi; \
	if ! command -v pkg-config >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ pkg-config not found$(RESET) (needed for canvas/lv_img_conv)"; WARN=1; \
		echo "  Install: $(YELLOW)brew install pkg-config$(RESET) (macOS)"; \
		echo "         $(YELLOW)sudo apt install pkg-config$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install pkgconfig$(RESET) (Fedora/RHEL)"; \
	else \
		echo "$(GREEN)✓ pkg-config found$(RESET)"; \
		CANVAS_MISSING=""; \
		if ! pkg-config --exists cairo 2>/dev/null; then \
			echo "$(YELLOW)⚠ cairo not found$(RESET) (needed for canvas/lv_img_conv)"; WARN=1; \
			CANVAS_MISSING="$$CANVAS_MISSING cairo"; \
		else \
			echo "$(GREEN)✓ cairo found:$(RESET) $$(pkg-config --modversion cairo)"; \
		fi; \
		if ! pkg-config --exists pango 2>/dev/null; then \
			echo "$(YELLOW)⚠ pango not found$(RESET) (needed for canvas text rendering)"; WARN=1; \
			CANVAS_MISSING="$$CANVAS_MISSING pango"; \
		else \
			echo "$(GREEN)✓ pango found:$(RESET) $$(pkg-config --modversion pango)"; \
		fi; \
		if ! pkg-config --exists libpng 2>/dev/null; then \
			echo "$(YELLOW)⚠ libpng not found$(RESET) (needed for PNG support)"; WARN=1; \
			CANVAS_MISSING="$$CANVAS_MISSING libpng"; \
		else \
			echo "$(GREEN)✓ libpng found$(RESET)"; \
		fi; \
		if ! pkg-config --exists libjpeg 2>/dev/null; then \
			echo "$(YELLOW)⚠ libjpeg not found$(RESET) (optional, for JPEG support)"; \
			CANVAS_MISSING="$$CANVAS_MISSING libjpeg"; \
		else \
			echo "$(GREEN)✓ libjpeg found$(RESET)"; \
		fi; \
		if ! pkg-config --exists librsvg-2.0 2>/dev/null; then \
			echo "$(YELLOW)⚠ librsvg not found$(RESET) (optional, for SVG support)"; \
			CANVAS_MISSING="$$CANVAS_MISSING librsvg"; \
		else \
			echo "$(GREEN)✓ librsvg found$(RESET)"; \
		fi; \
		if [ -n "$$CANVAS_MISSING" ]; then \
			echo "  $(CYAN)To install canvas dependencies:$(RESET)"; \
			if [ "$(UNAME_S)" = "Darwin" ]; then \
				echo "  $(YELLOW)brew install$$CANVAS_MISSING$(RESET)"; \
			elif [ -f /etc/debian_version ]; then \
				DEBIAN_PKGS=""; \
				for lib in $$CANVAS_MISSING; do \
					case $$lib in \
						cairo) DEBIAN_PKGS="$$DEBIAN_PKGS libcairo2-dev";; \
						pango) DEBIAN_PKGS="$$DEBIAN_PKGS libpango1.0-dev";; \
						libpng) DEBIAN_PKGS="$$DEBIAN_PKGS libpng-dev";; \
						libjpeg) DEBIAN_PKGS="$$DEBIAN_PKGS libjpeg-dev";; \
						librsvg) DEBIAN_PKGS="$$DEBIAN_PKGS librsvg2-dev";; \
					esac; \
				done; \
				echo "  $(YELLOW)sudo apt install$$DEBIAN_PKGS$(RESET)"; \
			elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then \
				FEDORA_PKGS=""; \
				for lib in $$CANVAS_MISSING; do \
					case $$lib in \
						cairo) FEDORA_PKGS="$$FEDORA_PKGS cairo-devel";; \
						pango) FEDORA_PKGS="$$FEDORA_PKGS pango-devel";; \
						libpng) FEDORA_PKGS="$$FEDORA_PKGS libpng-devel";; \
						libjpeg) FEDORA_PKGS="$$FEDORA_PKGS libjpeg-turbo-devel";; \
						librsvg) FEDORA_PKGS="$$FEDORA_PKGS librsvg2-devel";; \
					esac; \
				done; \
				echo "  $(YELLOW)sudo dnf install$$FEDORA_PKGS$(RESET)"; \
			fi; \
		fi; \
	fi; \
	if pkg-config --exists libhv 2>/dev/null; then \
		echo "$(GREEN)✓ libhv:$(RESET) Using system version $$(pkg-config --modversion libhv 2>/dev/null || echo 'unknown')"; \
	elif [ ! -f "$(LIBHV_DIR)/lib/libhv.a" ]; then \
		echo "$(YELLOW)⚠ libhv not built$(RESET)"; WARN=1; \
		echo "  Run: $(YELLOW)make libhv-build$(RESET)"; \
	else \
		echo "$(GREEN)✓ libhv:$(RESET) Using submodule version"; \
	fi; \
	if [ -d "/usr/include/spdlog" ] || [ -d "/usr/local/include/spdlog" ] || [ -d "/opt/homebrew/include/spdlog" ]; then \
		echo "$(GREEN)✓ spdlog:$(RESET) Using system version (header-only)"; \
	elif [ ! -d "$(SPDLOG_DIR)/include" ]; then \
		echo "$(RED)✗ spdlog not found$(RESET) (submodule)"; ERROR=1; \
		echo "  Run: $(YELLOW)git submodule update --init --recursive$(RESET)"; \
	else \
		echo "$(GREEN)✓ spdlog:$(RESET) Using submodule version (header-only)"; \
	fi; \
	if pkg-config --exists fmt 2>/dev/null; then \
		echo "$(GREEN)✓ fmt:$(RESET) Using system version $$(pkg-config --modversion fmt 2>/dev/null || echo 'unknown')"; \
	else \
		echo "$(YELLOW)⚠ fmt not found$(RESET) (required by header-only spdlog)"; WARN=1; \
		echo "  Install: $(YELLOW)brew install fmt$(RESET) (macOS)"; \
		echo "         $(YELLOW)sudo apt install libfmt-dev$(RESET) (Debian/Ubuntu)"; \
		echo "         $(YELLOW)sudo dnf install fmt-devel$(RESET) (Fedora/RHEL)"; \
	fi; \
	if [ ! -d "$(LVGL_DIR)/src" ]; then \
		echo "$(RED)✗ LVGL not found$(RESET) (submodule)"; ERROR=1; \
		echo "  Run: $(YELLOW)git submodule update --init --recursive$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL found:$(RESET) $(LVGL_DIR)"; \
	fi; \
	if [ "$(UNAME_S)" != "Darwin" ]; then \
		if [ ! -d "$(WPA_DIR)/wpa_supplicant" ]; then \
			echo "$(RED)✗ wpa_supplicant not found$(RESET) (submodule)"; ERROR=1; \
			echo "  Run: $(YELLOW)git submodule update --init --recursive$(RESET)"; \
		else \
			echo "$(GREEN)✓ wpa_supplicant found:$(RESET) $(WPA_DIR)"; \
		fi; \
	fi; \
	echo ""; \
	if [ $$ERROR -eq 1 ]; then \
		echo "$(RED)$(BOLD)✗ Dependency check failed!$(RESET)"; \
		echo ""; \
		echo "$(CYAN)Quick fix:$(RESET) Run $(YELLOW)make install-deps$(RESET) to auto-install missing dependencies"; \
		echo "$(CYAN)Or manually install:$(RESET)$$MISSING_DEPS"; \
		exit 1; \
	elif [ $$WARN -eq 1 ]; then \
		echo "$(YELLOW)⚠ Some optional dependencies missing$(RESET)"; \
		echo "$(CYAN)Run$(RESET) $(YELLOW)make install-deps$(RESET) $(CYAN)to install them automatically$(RESET)"; \
	else \
		echo "$(GREEN)$(BOLD)✓ All dependencies satisfied!$(RESET)"; \
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
			libjpeg:brew) echo "jpeg";; \
			libjpeg:apt) echo "libjpeg-dev";; \
			libjpeg:dnf) echo "libjpeg-turbo-devel";; \
			librsvg:brew) echo "librsvg";; \
			librsvg:apt) echo "librsvg2-dev";; \
			librsvg:dnf) echo "librsvg2-devel";; \
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
		if ! pkg-config --exists libjpeg 2>/dev/null; then \
			INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg libjpeg)"; \
		fi; \
		if ! pkg-config --exists librsvg-2.0 2>/dev/null; then \
			INSTALL_NEEDED=1; TO_INSTALL="$$TO_INSTALL $$(add_pkg librsvg)"; \
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
libhv-build:
	$(ECHO) "$(CYAN)Building libhv...$(RESET)"
ifeq ($(UNAME_S),Darwin)
	$(Q)cd $(LIBHV_DIR) && \
		MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) \
		CFLAGS="$(MACOS_DEPLOYMENT_TARGET)" \
		CXXFLAGS="$(MACOS_DEPLOYMENT_TARGET)" \
		./configure --with-http-client
	$(Q)MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) $(MAKE) -C $(LIBHV_DIR) -j$(NPROC) libhv
else
	$(Q)cd $(LIBHV_DIR) && ./configure --with-http-client
	$(Q)$(MAKE) -C $(LIBHV_DIR) -j$(NPROC) libhv
endif
	$(ECHO) "$(GREEN)✓ libhv built successfully$(RESET)"

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
	$(Q)MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) cmake --build $(SDL2_BUILD_DIR) --config Release -j$(NPROC)
else
	$(Q)cd $(SDL2_BUILD_DIR) && \
		cmake .. \
			-DCMAKE_BUILD_TYPE=Release \
			-DSDL_SHARED=OFF \
			-DSDL_STATIC=ON \
			-DSDL_TEST=OFF \
			-DSDL_TESTS=OFF
	$(Q)cmake --build $(SDL2_BUILD_DIR) --config Release -j$(NPROC)
endif
	$(ECHO) "$(GREEN)✓ SDL2 built successfully$(RESET)"

$(SDL2_LIB):
	$(Q)$(MAKE) sdl2-build

# Build wpa_supplicant client library (Linux only)
ifneq ($(UNAME_S),Darwin)
$(WPA_CLIENT_LIB):
	$(ECHO) "$(BOLD)$(BLUE)[WPA]$(RESET) Building wpa_supplicant client library..."
	$(Q)if [ ! -f "$(WPA_DIR)/wpa_supplicant/.config" ]; then \
		echo "$(RED)✗ wpa_supplicant/.config not found$(RESET)"; \
		echo "  Expected at: $(WPA_DIR)/wpa_supplicant/.config"; \
		exit 1; \
	fi
	$(Q)$(MAKE) -C $(WPA_DIR)/wpa_supplicant -j$(NPROC) libwpa_client.a
	$(ECHO) "$(GREEN)✓ libwpa_client.a built successfully$(RESET)"
endif
