# SPDX-License-Identifier: GPL-3.0-or-later
# ESP32 native-port feasibility audit (Phase 0). Throwaway-quality scaffolding,
# committed for reproducibility. See docs/devel/specs/plans/2026-06-10-esp32-native-audit.md
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(native_audit)
