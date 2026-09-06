# SPDX-License-Identifier: GPL-3.0-or-later
# Bluetooth plugin shared library — runtime-loaded via dlopen()
#
# Builds libhelix-bluetooth.so from src/bluetooth/*.cpp.
# Links against libsystemd (sd-bus for BlueZ D-Bus) and libbluetooth (RFCOMM).
#
# Built as part of 'all' when dependencies are available, silently skipped otherwise.

BT_SRCS := $(wildcard src/bluetooth/*.cpp)
BT_OBJS := $(BT_SRCS:src/bluetooth/%.cpp=$(OBJ_DIR)/bluetooth/%.o)
BT_SO   := $(BUILD_DIR)/lib/libhelix-bluetooth.so

# miniLZO (LZO1X compression for MakeID protocol, compiled into the BT plugin)
MINILZO_OBJ := $(OBJ_DIR)/bluetooth/minilzo.o

# --- Dependency detection ---
# Native: use pkg-config / compile test
# Cross: check sysroot for libraries
ifneq ($(CROSS_COMPILE),)
    # Cross-compilation: check if target libraries exist in sysroot
    BT_BLUEZ_OK := $(shell test -f /usr/lib/$(TARGET_TRIPLE)/libbluetooth.so -o \
                                 -f /usr/lib/$(TARGET_TRIPLE)/libbluetooth.a && echo yes)
    BT_SYSTEMD_OK := $(shell test -f /usr/lib/$(TARGET_TRIPLE)/libsystemd.so -o \
                                   -f /usr/lib/$(TARGET_TRIPLE)/libsystemd.a && echo yes)
else
    # Native: use pkg-config for both
    BT_SYSTEMD_OK := $(shell pkg-config --exists libsystemd 2>/dev/null && echo yes)
    BT_BLUEZ_OK := $(shell pkg-config --exists bluez 2>/dev/null && echo yes)
endif

ifeq ($(BT_SYSTEMD_OK)-$(BT_BLUEZ_OK),yes-yes)
    BT_AVAILABLE := yes
else
    BT_AVAILABLE := no
endif

# Compiler/linker flags for plugin
BT_CXXFLAGS := $(CXXFLAGS) -fPIC -I$(INC_DIR) $(SPDLOG_INC) -isystem lib/minilzo
ifneq ($(CROSS_COMPILE),)
    BT_LDFLAGS := -shared -lbluetooth -lsystemd
else
    BT_LDFLAGS := -shared -lbluetooth $(shell pkg-config --libs libsystemd 2>/dev/null)
endif

# Add systemd libs when available (handles platform differences)
ifneq ($(SYSTEMD_LIBS),)
    # SYSTEMD_LIBS already computed by main Makefile — prefer it for cross builds
    ifneq ($(CROSS_COMPILE),)
        BT_LDFLAGS := -shared -lbluetooth $(SYSTEMD_LIBS)
    endif
endif

# --- Build rules ---

.PHONY: bluetooth-plugin

bluetooth-plugin:
ifeq ($(BT_AVAILABLE),yes)
	@echo "$(CYAN)$(BOLD)Building Bluetooth plugin...$(RESET)"
	$(Q)$(MAKE) $(BT_SO)
else
	@echo "$(YELLOW)Bluetooth plugin: skipped (missing libbluetooth-dev or libsystemd-dev)$(RESET)"
endif

$(BT_SO): $(BT_OBJS) $(MINILZO_OBJ) | $(BUILD_DIR)/lib
	$(ECHO) "$(GREEN)[LD] $@$(RESET)"
	$(Q)$(CXX) -o $@ $^ $(BT_LDFLAGS)

$(OBJ_DIR)/bluetooth/%.o: src/bluetooth/%.cpp $(ABI_STAMP) | $(OBJ_DIR)/bluetooth
	$(ECHO) "$(CYAN)[CXX] $<$(RESET)"
	$(Q)$(CXX) $(BT_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(MINILZO_OBJ): lib/minilzo/minilzo.c | $(OBJ_DIR)/bluetooth
	$(ECHO) "$(CYAN)[CC]  $<$(RESET)"
	$(Q)$(CC) $(CFLAGS) -fPIC -isystem lib/minilzo -c $< -o $@

$(OBJ_DIR)/bluetooth:
	$(Q)mkdir -p $@

# Clean target
.PHONY: clean-bluetooth
clean-bluetooth:
	$(Q)rm -rf $(OBJ_DIR)/bluetooth $(BT_SO)

# Include dependency files for header tracking
-include $(wildcard $(OBJ_DIR)/bluetooth/*.d)
