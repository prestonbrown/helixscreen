// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file helix_features.h
 * @brief The default of every HELIX_HAS_* feature gate, in one place.
 *
 * An undefined identifier in `#if` evaluates to 0, so a build that forgot to pass a gate
 * would compile its feature out with no error. Every build force-includes this header
 * (through lvgl_pch.h), and each one passes -D only for a gate it changes: the Makefile
 * per platform (mk/cross.mk), the Android CMakeLists, the ESP32 firmware's helixapp
 * CMakeLists. scripts/check_feature_gates.py keeps every tested gate declared here and
 * every default under its own #ifndef.
 *
 * Presence flags the build sets by probing the host (HELIX_HAS_SOUND, HELIX_HAS_ALSA,
 * HELIX_HAS_SYSTEMD, ...) and flags derived from the platform (HELIX_HAS_CAMERA in
 * lv_conf.h) are not defaults and do not belong here.
 */

#pragma once

// Label printing (Brother QL, Phomemo, Niimbot, MakeID).
#ifndef HELIX_HAS_LABEL_PRINTER
#define HELIX_HAS_LABEL_PRINTER 1
#endif

// Vendor filament systems tied to one printer family. AFC and Happy Hare are
// user-installable Klipper add-ons that can appear on any printer, so they have no gate.
#ifndef HELIX_HAS_CFS
#define HELIX_HAS_CFS 1
#endif
#ifndef HELIX_HAS_IFS
#define HELIX_HAS_IFS 1
#endif
#ifndef HELIX_HAS_ACE
#define HELIX_HAS_ACE 1
#endif
#ifndef HELIX_HAS_QIDI
#define HELIX_HAS_QIDI 1
#endif
#ifndef HELIX_HAS_SNAPMAKER
#define HELIX_HAS_SNAPMAKER 1
#endif

// The 2D G-code renderer and the 3D bed-mesh renderer, code and runtime buffers.
#ifndef HELIX_HAS_GCODE_VIEWER
#define HELIX_HAS_GCODE_VIEWER 1
#endif
#ifndef HELIX_HAS_BED_MESH_3D
#define HELIX_HAS_BED_MESH_3D 1
#endif

// The Lua plugin runtime.
#ifndef HELIX_HAS_PLUGINS
#define HELIX_HAS_PLUGINS 1
#endif

// Timelapse viewing (list, download, playback). Capture control is plain JSON-RPC and
// is never gated.
#ifndef HELIX_HAS_TIMELAPSE_VIEWER
#define HELIX_HAS_TIMELAPSE_VIEWER 1
#endif

// The Belt Tension panel, which reads Klipper's /tmp results and so needs to run beside
// Klipper.
#ifndef HELIX_HAS_BELT_TUNER
#define HELIX_HAS_BELT_TUNER 1
#endif

// The home Toolhead widget and its preset editor.
#ifndef HELIX_HAS_TOOLHEAD_WIDGET
#define HELIX_HAS_TOOLHEAD_WIDGET 1
#endif

// The printer image tagger, the editor that places a photo's part callouts.
#ifndef HELIX_HAS_IMAGE_TAGGER
#define HELIX_HAS_IMAGE_TAGGER 1
#endif

// The font rungs above the authored tier ladder, reached only by a high-DPI scale factor.
#ifndef HELIX_HAS_HIDPI_FONTS
#define HELIX_HAS_HIDPI_FONTS 1
#endif

// The G-code console: the Advanced row, the console overlay and its settings, and the home
// Console widget.
#ifndef HELIX_HAS_CONSOLE
#define HELIX_HAS_CONSOLE 1
#endif

// The Snake easter egg behind seven taps on the About page's printer name.
#ifndef HELIX_HAS_SNAKE_GAME
#define HELIX_HAS_SNAKE_GAME 1
#endif

// Chinese and Japanese in the language list; a build without a CJK font face drops them.
#ifndef HELIX_HAS_CJK
#define HELIX_HAS_CJK 1
#endif
