# GuppyScreen UI Prototype Makefile
# LVGL 9 + SDL2 simulator

# Compilers
CC := clang
CXX := clang++
CFLAGS := -std=c11 -Wall -Wextra -O2 -g
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g

# Directories
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

# LVGL
LVGL_DIR := lvgl
LVGL_INC := -I$(LVGL_DIR) -I$(LVGL_DIR)/src
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name "*.c")
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))

# Application
APP_SRCS := $(wildcard $(SRC_DIR)/*.cpp)
APP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(APP_SRCS))

# SDL2
SDL2_CFLAGS := $(shell sdl2-config --cflags)
SDL2_LIBS := $(shell sdl2-config --libs)

# Include paths
INCLUDES := -I. -I$(INC_DIR) $(LVGL_INC) $(SDL2_CFLAGS)

# Linker flags
LDFLAGS := $(SDL2_LIBS) -lm -lpthread

# Binary
TARGET := $(BIN_DIR)/guppy-ui-proto

# LVGL configuration
LV_CONF := -DLV_CONF_INCLUDE_SIMPLE

# Parallel build
NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

.PHONY: all clean run

all: $(TARGET)

# Link binary
$(TARGET): $(APP_OBJS) $(LVGL_OBJS)
	@mkdir -p $(BIN_DIR)
	@echo "Linking $@..."
	@$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Build complete: $@"

# Compile app sources
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Compile LVGL sources
$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling LVGL: $<..."
	@$(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Run the prototype
run: $(TARGET)
	@echo "Running UI prototype..."
	@$(TARGET)

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@echo "Clean complete"

# Parallel build target
build:
	@echo "Building with $(NPROC) parallel jobs..."
	@$(MAKE) -j$(NPROC) all

