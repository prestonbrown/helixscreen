#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: no C++ identifier may collide with an X11 macro inside a translation
# unit that reaches SDL or X11.
#
# X11's headers are pre-standard C: they publish their constants as object-like
# #defines, not as enums or constexprs. `None` is `#define None 0L`, `Status` is
# `#define Status int`. A macro has no scope, so it rewrites a QUALIFIED C++ name
# too -- `InvalidationScope::None` preprocesses to `InvalidationScope::0L` and the
# compiler reports something that looks nothing like the cause.
#
# That shipped. v0.99.118 tagged, then failed to build on x86_64 Debian, Raspberry
# Pi and Raspberry Pi 32-bit because include/gcode_selection_state.h declared
#   enum class InvalidationScope { None, SolidCache, SolidAndGhost };
# and src/rendering/gcode_gles_renderer.cpp includes <SDL.h>, whose Linux include
# chain reaches <X11/X.h>. Every printer target built clean -- none of them link
# X11 -- so nothing saw it until the release job ran, after the tag was cut.
# Fixed by renaming the enumerator to `Nothing` (b942c8b1e).
#
# WHAT IS FLAGGED
#   A colliding identifier used in one of the two shapes the preprocessor rewrites:
#     qualified   Foo::None, helix::gcode::InvalidationScope::None
#     enumerator  enum class Foo { None, ... }   (the declaration itself)
#   ...in a file that shares a translation unit with an SDL/X11 include.
#
# WHAT IS NOT FLAGGED, deliberately
#   - Any file no SDL/X11-including TU pulls in. The tree has ~96 uses of `::None`
#     and they are all correct; the danger is not today's code, it is the day
#     someone adds an SDL include to one of those files -- or adds a colliding
#     name to a file that already has one. Flagging all 96 would be noise, and a
#     gate that cries wolf gets switched off.
#   - Lowercase or differently-cased spellings. Macros are case-sensitive: a local
#     `none`, a member `status_`, a string "None", a comment. Comments and string
#     literals are blanked before matching.
#   - Bare unqualified uses. `Complex`, `Above`, `Success` are ordinary English
#     words; requiring the `::` or the enum body is what keeps the rule precise.
#     An unscoped `enum { None }` is still caught by the enumerator rule at its
#     declaration, which is where the fix belongs anyway.
#   - Any line carrying `// X11_MACRO_OK: <reason>` (or on one of the 3 lines
#     above it, since enum bodies and long qualified names wrap).
#
# TWO TIERS, AND WHY
#   xcore -- macros from <X11/X.h>.               FATAL.
#   xlib  -- macros only in <X11/Xlib.h>/<Xutil.h>. Reported, not fatal.
#
#   This is not squeamishness, it is what the evidence says. X.h is what SDL's
#   chain reaches; Xlib.h is a level further out and is pulled only by
#   SDL_syswm.h / SDL_egl.h, which nothing here includes. The proof is in the
#   tree: src/application/application.cpp includes <SDL.h> AND writes
#   `UpdateChecker::Status` (line 2377). `Status` is `#define Status int` in
#   Xlib.h -- if that macro were reachable the line would preprocess to
#   `UpdateChecker::int` and fail -- and that file compiles on all three
#   platforms that the `None` collision broke. So the xlib tier is a real but
#   unproven exposure: it is counted and printed so it stays visible, and it does
#   not fail the build, because failing on a known-good line is how a gate gets
#   deleted. `--strict` promotes it if the include graph ever changes.
#
# INCLUDE FOLLOWING: PROJECT HEADERS ONLY, ONE HOP INTO THE SYSTEM
#   Includes are resolved inside src/ and include/ only. A system include is never
#   opened; it is only asked whether its NAME is an SDL/X11 root (SDL*.h, SDL2/*,
#   X11/*, EGL/*, GL/glx.h). Following system headers would need a real
#   preprocessor plus the per-platform -D/-I set -- and the platform is the whole
#   problem here, since the collision only exists on the three targets that have
#   X11 at all. The one-hop rule loses nothing: X11 arrives through a system
#   header whose name says so.
#
#   Exposure is per TRANSLATION UNIT and ORDER-AWARE: the walk expands a TU's
#   includes in textual order and a file is exposed only from the point the first
#   SDL/X11 root has been pulled in. That is not a refinement, it is the whole
#   gate. gcode_selection_state.h includes no SDL -- it was reached at
#   gcode_gles_renderer.cpp:11, ELEVEN LINES BEFORE the <SDL.h> at line 22 -- so
#   its `enum class InvalidationScope { None }` preprocessed with no macro live
#   and compiled. What failed were the two USES at lines 1729 and 1735 of that
#   same .cpp, past the include. Ignore order and the gate reports 47 sites on a
#   tree that builds clean on all three affected platforms; model it and the same
#   tree reports 0, while the pre-fix tree reports exactly those two lines.
#
#   Include guards are modelled as `#pragma once` (every project header has one),
#   so the first expansion in a TU is the one that counts -- that is what keeps
#   include/ams_types.h safe when main.cpp pulls it in early and something else
#   would have pulled it late. A header exposed in ANY TU is exposed everywhere:
#   a `::None` in include/theme_manager.h really would break src/main.cpp, even
#   though src/ui/theme_manager.cpp includes that header first thing.
#
#   Includes behind #ifdef are followed -- <SDL.h> in application.cpp sits under
#   `#ifdef HELIX_DISPLAY_SDL`, which is exactly the desktop build that has X11.
#   The one exception is a POSITIVE guard naming a platform whose SDL has no X11
#   backend (__ANDROID__, _WIN32, __APPLE__ ...): src/app_globals.cpp includes
#   <SDL.h> under `#ifdef __ANDROID__`, and counting that reported
#   `EndlessSpoolRestriction::None` in include/ams_types.h, a line that has
#   always compiled. `#ifndef` and `#if !defined(...)` are not exceptions.
#
# THE MACRO LIST
#   Derived on 2026-08-28 from /usr/include/X11/X.h (346 object-like macros after
#   dropping the reserved `_`-prefixed and `X_PROTOCOL*` guards) and
#   /usr/include/X11/Xlib.h + /usr/include/X11/Xutil.h (163 more). Embedded rather
#   than read at runtime, because the gate has to give the same verdict on a CI
#   container and a printer, neither of which ships X11 headers. `--derive`
#   re-reads the local headers when present and diffs them against the embedded
#   set, so the list can be refreshed deliberately instead of drifting.
#
# Usage:
#   check_x11_macro_collisions.py                 # fail on any xcore collision
#   check_x11_macro_collisions.py --max-allowed 0 # ratcheting baseline
#   check_x11_macro_collisions.py --list          # every site, file:line
#   check_x11_macro_collisions.py --summary       # counts only
#   check_x11_macro_collisions.py --exposed       # list the exposed TUs/headers
#   check_x11_macro_collisions.py --strict        # xlib tier fails too
#   check_x11_macro_collisions.py --derive        # re-derive list from /usr/include/X11

import argparse
import os
import re
import sys

SCAN_DIRS = ['src', 'include']
SCAN_EXTS = ('.cpp', '.cc', '.cxx', '.c', '.h', '.hpp', '.hxx')
TU_EXTS = ('.cpp', '.cc', '.cxx', '.c')

OPT_OUT = 'X11_MACRO_OK'
OPT_OUT_LOOKBACK = 3

# Headers whose NAME says X11 is coming. Not opened -- see the docstring.
#   SDL*.h / SDL2/*  : <SDL.h> reaches <X11/X.h> on the desktop Linux builds.
#   X11/*            : direct.
#   EGL/*, GL/glx.h  : Mesa's <EGL/eglplatform.h> includes <X11/Xlib.h> when its
#                      USE_X11 is on, which is a property of the platform's Mesa
#                      build, not of ours -- so treat it as a root rather than
#                      guess per toolchain. (Costs nothing today: the only EGL
#                      includer, gcode_gles_renderer.cpp, is SDL-exposed already.)
TAINT_ROOT_RE = re.compile(r'^(?:SDL2?/|SDL[\w]*\.h$|X11/|EGL/|GL/glx\.h$)')

# Platforms whose SDL cannot reach X11. `src/app_globals.cpp` includes <SDL.h>
# under `#ifdef __ANDROID__`; Android's SDL has no X11 backend, so counting that
# as a root taints include/ams_types.h and reports `EndlessSpoolRestriction::None`
# on a file that has always compiled. A POSITIVE guard on one of these is proof
# the include never coexists with X11; `#ifndef` / `#if !defined(...)` is not.
NON_X11_PLATFORM_RE = re.compile(r'\b(?:__ANDROID__|ANDROID|_WIN32|_WIN64|__APPLE__|'
                                 r'__MACH__|__EMSCRIPTEN__|TARGET_OS_\w+)\b')
COND_OPEN_RE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
INCLUDE_LINE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"]+)[>"]')


def platform_gated_out(cond_stack):
    """True if some enclosing #if positively selects a platform that has no X11."""
    for directive, expr in cond_stack:
        if directive == 'ifndef':
            continue
        if '!' in expr:                       # `#if !defined(__ANDROID__)` etc.
            continue
        if NON_X11_PLATFORM_RE.search(expr):
            return True
    return False

# xcore: 346
XCORE = {
    'Above', 'AllTemporary', 'AllocAll', 'AllocNone', 'AllowExposures', 'AlreadyGrabbed',
    'Always', 'AnyButton', 'AnyKey', 'AnyModifier', 'AnyPropertyType', 'ArcChord',
    'ArcPieSlice', 'AsyncBoth', 'AsyncKeyboard', 'AsyncPointer', 'AutoRepeatModeDefault',
    'AutoRepeatModeOff', 'AutoRepeatModeOn', 'BadAccess', 'BadAlloc', 'BadAtom', 'BadColor',
    'BadCursor', 'BadDrawable', 'BadFont', 'BadGC', 'BadIDChoice', 'BadImplementation',
    'BadLength', 'BadMatch', 'BadName', 'BadPixmap', 'BadRequest', 'BadValue', 'BadWindow',
    'Below', 'BottomIf', 'Button1', 'Button1Mask', 'Button1MotionMask', 'Button2',
    'Button2Mask', 'Button2MotionMask', 'Button3', 'Button3Mask', 'Button3MotionMask',
    'Button4', 'Button4Mask', 'Button4MotionMask', 'Button5', 'Button5Mask',
    'Button5MotionMask', 'ButtonMotionMask', 'ButtonPress', 'ButtonPressMask',
    'ButtonRelease', 'ButtonReleaseMask', 'CWBackPixel', 'CWBackPixmap', 'CWBackingPixel',
    'CWBackingPlanes', 'CWBackingStore', 'CWBitGravity', 'CWBorderPixel', 'CWBorderPixmap',
    'CWBorderWidth', 'CWColormap', 'CWCursor', 'CWDontPropagate', 'CWEventMask', 'CWHeight',
    'CWOverrideRedirect', 'CWSaveUnder', 'CWSibling', 'CWStackMode', 'CWWidth',
    'CWWinGravity', 'CWX', 'CWY', 'CapButt', 'CapNotLast', 'CapProjecting', 'CapRound',
    'CenterGravity', 'CirculateNotify', 'CirculateRequest', 'ClientMessage',
    'ClipByChildren', 'ColormapChangeMask', 'ColormapInstalled', 'ColormapNotify',
    'ColormapUninstalled', 'Complex', 'ConfigureNotify', 'ConfigureRequest',
    'ControlMapIndex', 'ControlMask', 'Convex', 'CoordModeOrigin', 'CoordModePrevious',
    'CopyFromParent', 'CreateNotify', 'CurrentTime', 'CursorShape', 'DefaultBlanking',
    'DefaultExposures', 'DestroyAll', 'DestroyNotify', 'DirectColor', 'DisableAccess',
    'DisableScreenInterval', 'DisableScreenSaver', 'DoBlue', 'DoGreen', 'DoRed',
    'DontAllowExposures', 'DontPreferBlanking', 'EastGravity', 'EnableAccess',
    'EnterNotify', 'EnterWindowMask', 'EvenOddRule', 'Expose', 'ExposureMask',
    'FamilyChaos', 'FamilyDECnet', 'FamilyInternet', 'FamilyInternet6',
    'FamilyServerInterpreted', 'FillOpaqueStippled', 'FillSolid', 'FillStippled',
    'FillTiled', 'FirstExtensionError', 'FocusChangeMask', 'FocusIn', 'FocusOut',
    'FontChange', 'FontLeftToRight', 'FontRightToLeft', 'ForgetGravity', 'GCArcMode',
    'GCBackground', 'GCCapStyle', 'GCClipMask', 'GCClipXOrigin', 'GCClipYOrigin',
    'GCDashList', 'GCDashOffset', 'GCFillRule', 'GCFillStyle', 'GCFont', 'GCForeground',
    'GCFunction', 'GCGraphicsExposures', 'GCJoinStyle', 'GCLastBit', 'GCLineStyle',
    'GCLineWidth', 'GCPlaneMask', 'GCStipple', 'GCSubwindowMode', 'GCTile',
    'GCTileStipXOrigin', 'GCTileStipYOrigin', 'GXand', 'GXandInverted', 'GXandReverse',
    'GXclear', 'GXcopy', 'GXcopyInverted', 'GXequiv', 'GXinvert', 'GXnand', 'GXnoop',
    'GXnor', 'GXor', 'GXorInverted', 'GXorReverse', 'GXset', 'GXxor', 'GenericEvent',
    'GrabFrozen', 'GrabInvalidTime', 'GrabModeAsync', 'GrabModeSync', 'GrabNotViewable',
    'GrabSuccess', 'GraphicsExpose', 'GravityNotify', 'GrayScale', 'HostDelete',
    'HostInsert', 'IncludeInferiors', 'InputFocus', 'InputOnly', 'InputOutput',
    'IsUnmapped', 'IsUnviewable', 'IsViewable', 'JoinBevel', 'JoinMiter', 'JoinRound',
    'KBAutoRepeatMode', 'KBBellDuration', 'KBBellPercent', 'KBBellPitch', 'KBKey',
    'KBKeyClickPercent', 'KBLed', 'KBLedMode', 'KeyPress', 'KeyPressMask', 'KeyRelease',
    'KeyReleaseMask', 'KeymapNotify', 'KeymapStateMask', 'LASTEvent', 'LSBFirst',
    'LastExtensionError', 'LeaveNotify', 'LeaveWindowMask', 'LedModeOff', 'LedModeOn',
    'LineDoubleDash', 'LineOnOffDash', 'LineSolid', 'LockMapIndex', 'LockMask',
    'LowerHighest', 'MSBFirst', 'MapNotify', 'MapRequest', 'MappingBusy', 'MappingFailed',
    'MappingKeyboard', 'MappingModifier', 'MappingNotify', 'MappingPointer',
    'MappingSuccess', 'Mod1MapIndex', 'Mod1Mask', 'Mod2MapIndex', 'Mod2Mask',
    'Mod3MapIndex', 'Mod3Mask', 'Mod4MapIndex', 'Mod4Mask', 'Mod5MapIndex', 'Mod5Mask',
    'MotionNotify', 'NoEventMask', 'NoExpose', 'NoSymbol', 'Nonconvex', 'None',
    'NorthEastGravity', 'NorthGravity', 'NorthWestGravity', 'NotUseful', 'NotifyAncestor',
    'NotifyDetailNone', 'NotifyGrab', 'NotifyHint', 'NotifyInferior', 'NotifyNonlinear',
    'NotifyNonlinearVirtual', 'NotifyNormal', 'NotifyPointer', 'NotifyPointerRoot',
    'NotifyUngrab', 'NotifyVirtual', 'NotifyWhileGrabbed', 'Opposite',
    'OwnerGrabButtonMask', 'ParentRelative', 'PlaceOnBottom', 'PlaceOnTop',
    'PointerMotionHintMask', 'PointerMotionMask', 'PointerRoot', 'PointerWindow',
    'PreferBlanking', 'PropModeAppend', 'PropModePrepend', 'PropModeReplace',
    'PropertyChangeMask', 'PropertyDelete', 'PropertyNewValue', 'PropertyNotify',
    'PseudoColor', 'RaiseLowest', 'ReparentNotify', 'ReplayKeyboard', 'ReplayPointer',
    'ResizeRedirectMask', 'ResizeRequest', 'RetainPermanent', 'RetainTemporary',
    'RevertToNone', 'RevertToParent', 'RevertToPointerRoot', 'ScreenSaverActive',
    'ScreenSaverReset', 'SelectionClear', 'SelectionNotify', 'SelectionRequest',
    'SetModeDelete', 'SetModeInsert', 'ShiftMapIndex', 'ShiftMask', 'SouthEastGravity',
    'SouthGravity', 'SouthWestGravity', 'StaticColor', 'StaticGravity', 'StaticGray',
    'StippleShape', 'StructureNotifyMask', 'SubstructureNotifyMask',
    'SubstructureRedirectMask', 'Success', 'SyncBoth', 'SyncKeyboard', 'SyncPointer',
    'TileShape', 'TopIf', 'TrueColor', 'UnmapGravity', 'UnmapNotify', 'Unsorted',
    'VisibilityChangeMask', 'VisibilityFullyObscured', 'VisibilityNotify',
    'VisibilityPartiallyObscured', 'VisibilityUnobscured', 'WestGravity', 'WhenMapped',
    'WindingRule', 'XYBitmap', 'XYPixmap', 'YSorted', 'YXBanded', 'YXSorted', 'ZPixmap',
}
# xlib: 163
XLIB = {
    'AllHints', 'AllPlanes', 'AllValues', 'BitmapFileInvalid', 'BitmapNoMemory',
    'BitmapOpenFailed', 'BitmapSuccess', 'Bool', 'DontCareState', 'False', 'HeightValue',
    'IconMaskHint', 'IconPixmapHint', 'IconPositionHint', 'IconWindowHint', 'IconicState',
    'InactiveState', 'InputHint', 'NoValue', 'NormalState', 'PAllHints', 'PAspect',
    'PBaseSize', 'PMaxSize', 'PMinSize', 'PPosition', 'PResizeInc', 'PSize', 'PWinGravity',
    'QueuedAfterFlush', 'QueuedAfterReading', 'QueuedAlready', 'RectangleIn',
    'RectangleOut', 'RectanglePart', 'ReleaseByFreeingColormap', 'StateHint', 'Status',
    'True', 'USPosition', 'USSize', 'VisualAllMask', 'VisualBitsPerRGBMask',
    'VisualBlueMaskMask', 'VisualClassMask', 'VisualColormapSizeMask', 'VisualDepthMask',
    'VisualGreenMaskMask', 'VisualIDMask', 'VisualNoMask', 'VisualRedMaskMask',
    'VisualScreenMask', 'WidthValue', 'WindowGroupHint', 'WithdrawnState',
    'XBufferOverflow', 'XCNOENT', 'XCNOMEM', 'XCSUCCESS', 'XConverterNotFound',
    'XIMHighlight', 'XIMHotKeyStateOFF', 'XIMHotKeyStateON', 'XIMInitialState',
    'XIMPreeditArea', 'XIMPreeditCallbacks', 'XIMPreeditDisable', 'XIMPreeditEnable',
    'XIMPreeditNone', 'XIMPreeditNothing', 'XIMPreeditPosition', 'XIMPreeditUnKnown',
    'XIMPreserveState', 'XIMPrimary', 'XIMReverse', 'XIMSecondary', 'XIMStatusArea',
    'XIMStatusCallbacks', 'XIMStatusNone', 'XIMStatusNothing',
    'XIMStringConversionBottomEdge', 'XIMStringConversionBuffer', 'XIMStringConversionChar',
    'XIMStringConversionConcealed', 'XIMStringConversionLeftEdge',
    'XIMStringConversionLine', 'XIMStringConversionRetrieval',
    'XIMStringConversionRightEdge', 'XIMStringConversionSubstitution',
    'XIMStringConversionTopEdge', 'XIMStringConversionWord', 'XIMStringConversionWrapped',
    'XIMTertiary', 'XIMUnderline', 'XIMVisibleToBackword', 'XIMVisibleToCenter',
    'XIMVisibleToForward', 'XLocaleNotSupported', 'XLookupBoth', 'XLookupChars',
    'XLookupKeySym', 'XLookupNone', 'XNArea', 'XNAreaNeeded', 'XNBackground',
    'XNBackgroundPixmap', 'XNBaseFontName', 'XNClientWindow', 'XNColormap',
    'XNContextualDrawing', 'XNCursor', 'XNDefaultString', 'XNDestroyCallback',
    'XNDirectionalDependentDrawing', 'XNFilterEvents', 'XNFocusWindow', 'XNFontInfo',
    'XNFontSet', 'XNForeground', 'XNGeometryCallback', 'XNHotKey', 'XNHotKeyState',
    'XNInputStyle', 'XNLineSpace', 'XNMissingCharSet', 'XNOMAutomatic', 'XNOrientation',
    'XNPreeditAttributes', 'XNPreeditCaretCallback', 'XNPreeditDoneCallback',
    'XNPreeditDrawCallback', 'XNPreeditStartCallback', 'XNPreeditState',
    'XNPreeditStateNotifyCallback', 'XNQueryICValuesList', 'XNQueryIMValuesList',
    'XNQueryInputStyle', 'XNQueryOrientation', 'XNR6PreeditCallback', 'XNRequiredCharSet',
    'XNResetState', 'XNResourceClass', 'XNResourceName', 'XNSeparatorofNestedList',
    'XNSpotLocation', 'XNStatusAttributes', 'XNStatusDoneCallback', 'XNStatusDrawCallback',
    'XNStatusStartCallback', 'XNStdColormap', 'XNStringConversion',
    'XNStringConversionCallback', 'XNVaNestedList', 'XNVisiblePosition', 'XNegative',
    'XNoMemory', 'XUrgencyHint', 'XValue', 'X_HAVE_UTF8_STRING', 'XlibSpecificationRelease',
    'YNegative', 'YValue', 'ZoomState',
}

# `Foo::None`, `a::b::None`. The lookbehind stops `A::B::None` counting twice and
# keeps a leading `::` from being read as an identifier.
def qualified_re(names):
    return re.compile(r'(?<![\w:])\w+\s*::\s*(' + '|'.join(sorted(names)) + r')\b')

# `enum`, optional class/struct, optional name, optional `: underlying`, then a body.
ENUM_HEAD_RE = re.compile(r'\benum\b(?:\s+(?:class|struct))?(?:\s+[\w:]+)?'
                          r'(?:\s*:\s*[\w:\s<>,]+?)?\s*\{')


def blank_comments_and_strings(src):
    """Replace every comment / string / char literal with spaces, preserving length
    and newlines so offsets and line numbers stay exact."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            out[i] = out[i + 1] = ' '
            i += 2
            while i < n and not (src[i] == '*' and i + 1 < n and src[i + 1] == '/'):
                if src[i] != '\n':
                    out[i] = ' '
                i += 1
            if i + 1 < n:
                out[i] = out[i + 1] = ' '
                i += 2
        elif c in '"\'':
            quote = c
            out[i] = ' '
            i += 1
            while i < n and src[i] != quote:
                if src[i] == '\\' and i + 1 < n:
                    if src[i] != '\n':
                        out[i] = ' '
                    i += 1
                    if i < n and src[i] != '\n':
                        out[i] = ' '
                    i += 1
                    continue
                if src[i] != '\n':
                    out[i] = ' '
                i += 1
            if i < n:
                out[i] = ' '
                i += 1
        else:
            i += 1
    assert len(out) == n
    return ''.join(out)


def enum_bodies(code):
    """[(body_start, body_text)] for every enum with a body."""
    bodies = []
    for m in ENUM_HEAD_RE.finditer(code):
        start = m.end()          # just past the '{'
        depth = 1
        i = start
        while i < len(code) and depth:
            if code[i] == '{':
                depth += 1
            elif code[i] == '}':
                depth -= 1
            i += 1
        bodies.append((start, code[start:i - 1]))
    return bodies


ENUMERATOR_RE = re.compile(r'(?:^|,)\s*([A-Za-z_]\w*)')


def read_repo_files(root):
    files = {}
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        for dirpath, _, names in os.walk(base):
            for name in names:
                if name.endswith(SCAN_EXTS):
                    p = os.path.relpath(os.path.join(dirpath, name), root)
                    files[p.replace(os.sep, '/')] = None
    return files


def build_include_graph(root, files):
    """files: {relpath: source} -> {relpath: [(lineno, kind, target)]} in textual
    order. kind is 'root' (an SDL/X11 system header) or 'project' (a resolved
    repo file); anything else is dropped."""
    by_suffix = {}
    for p in files:
        parts = p.split('/')
        for i in range(len(parts)):
            by_suffix.setdefault('/'.join(parts[i:]), []).append(p)

    directives = {}
    for p in files:
        try:
            src = open(os.path.join(root, p), errors='ignore').read()
        except OSError:
            src = ''
        files[p] = src
        out = []
        cond_stack = []
        for lineno, line in enumerate(src.split('\n'), 1):
            cm = COND_OPEN_RE.match(line)
            if cm:
                kw, expr = cm.group(1), cm.group(2)
                if kw in ('if', 'ifdef', 'ifndef'):
                    cond_stack.append((kw, expr))
                elif kw in ('elif', 'else'):
                    if cond_stack:
                        cond_stack[-1] = ('if', expr if kw == 'elif' else '')
                elif kw == 'endif' and cond_stack:
                    cond_stack.pop()
                continue
            im = INCLUDE_LINE_RE.match(line)
            if not im:
                continue
            target = im.group(1)
            if TAINT_ROOT_RE.match(target):
                if not platform_gated_out(cond_stack):
                    out.append((lineno, 'root', target))
                continue
            rel = os.path.normpath(os.path.join(os.path.dirname(p), target)).replace(os.sep, '/')
            if rel in files:
                out.append((lineno, 'project', rel))
                continue
            cands = by_suffix.get(target)
            if cands and len(cands) == 1:
                out.append((lineno, 'project', cands[0]))
            elif cands:
                # ambiguous basename -- expand all of them rather than guess
                for c in cands:
                    out.append((lineno, 'project', c))
        directives[p] = out
    return directives


NEVER = float('inf')


def taint_lines(directives):
    """{relpath: first line from which the file sits under a live X11 macro}.

    Emulates the one thing that actually decides the outcome: the order in which
    the preprocessor expands the TU. A header pulled in BEFORE the SDL include is
    parsed with no macro defined and is fine -- which is why the pre-fix
    `enum class InvalidationScope { None, ... }` in gcode_selection_state.h
    (expanded at gcode_gles_renderer.cpp:11) compiled, while the two USES of it
    at lines 1729/1735 of that same .cpp, past the <SDL.h> at line 22, did not.
    Ignoring order instead reports 47 sites on a tree that builds clean on all
    three affected platforms, and a gate that does that gets switched off.

    Include guards are modelled as `#pragma once` (every project header here has
    one): the FIRST expansion in a TU is the one that counts, so a header first
    reached before the SDL include stays safe even where a later sibling would
    have pulled it in after. Per-TU, then the strongest (lowest) exposure line
    across all TUs wins for each file."""
    first = {}

    def record(p, line):
        if line < first.get(p, NEVER):
            first[p] = line

    def expand(p, visited, tainted, stack):
        """Expand p; return the taint state on the way out."""
        if p in visited or p in stack:
            return tainted
        visited.add(p)
        stack.add(p)
        if tainted:
            record(p, 1)                      # whole file sits under the macro
        turned_on_here = tainted
        for lineno, kind, target in directives.get(p, ()):
            if kind == 'root':
                if not tainted:
                    tainted = True
                    if not turned_on_here:
                        turned_on_here = True
                        record(p, lineno + 1)
            else:
                before = tainted
                tainted = expand(target, visited, tainted, stack)
                if tainted and not before and not turned_on_here:
                    turned_on_here = True
                    record(p, lineno + 1)
        stack.discard(p)
        return tainted

    for p in sorted(directives):
        if p.endswith(TU_EXTS):
            expand(p, set(), False, set())
    return first


def scan_file(path, src, macros, tier_of, taint_from):
    code = blank_comments_and_strings(src)
    lines = src.split('\n')
    hits = []

    def lineno_of(off):
        return code.count('\n', 0, off) + 1

    def opted_out(lineno):
        for i in range(lineno, max(0, lineno - 1 - OPT_OUT_LOOKBACK), -1):
            if OPT_OUT in lines[i - 1]:
                return True
        return False

    qre = qualified_re(macros)
    for m in qre.finditer(code):
        ln = lineno_of(m.start())
        if ln < taint_from or opted_out(ln):
            continue
        hits.append((path, ln, 'qualified', m.group(1), tier_of[m.group(1)],
                     lines[ln - 1].strip()[:110]))

    for start, body in enum_bodies(code):
        for em in ENUMERATOR_RE.finditer(body):
            name = em.group(1)
            if name not in macros:
                continue
            ln = lineno_of(start + em.start(1))
            if ln < taint_from or opted_out(ln):
                continue
            hits.append((path, ln, 'enumerator', name, tier_of[name],
                         lines[ln - 1].strip()[:110]))
    return hits


def derive_from_headers():
    tiers = {'xcore': ['/usr/include/X11/X.h'],
             'xlib': ['/usr/include/X11/Xlib.h', '/usr/include/X11/Xutil.h']}
    seen, found = set(), {}
    missing = []
    for tier, paths in tiers.items():
        names = set()
        for p in paths:
            if not os.path.exists(p):
                missing.append(p)
                continue
            for line in open(p, errors='ignore'):
                m = re.match(r'\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(\s|$)', line)
                if not m:
                    continue
                n = m.group(1)
                if n in seen or n.startswith('_') or n.startswith('X_PROTOCOL') or n == 'X_H':
                    continue
                seen.add(n)
                names.add(n)
        found[tier] = names
    return found, missing


def main():
    ap = argparse.ArgumentParser(
        description='Fail on a C++ identifier that an X11 macro would rewrite.',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Pass if fatal collisions <= N (ratcheting baseline). '
                         'Default: fail on any.')
    ap.add_argument('--strict', action='store_true',
                    help='Count the advisory xlib tier as fatal too')
    ap.add_argument('--list', action='store_true', help='Print every site')
    ap.add_argument('--summary', action='store_true', help='Per-tier counts only')
    ap.add_argument('--exposed', action='store_true',
                    help='List the files that share a TU with an SDL/X11 include')
    ap.add_argument('--derive', action='store_true',
                    help='Re-derive the macro list from /usr/include/X11 and diff')
    ap.add_argument('--repo-root', default=None, help='Repo root (default: script parent)')
    ap.add_argument('paths', nargs='*', help='Restrict the report to these files')
    args = ap.parse_args()

    root = args.repo_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    if args.derive:
        found, missing = derive_from_headers()
        if missing:
            print('X11 headers not installed here: ' + ', '.join(missing))
            print('The embedded list stands; re-run --derive on a box with libx11-dev.')
            return 0
        rc = 0
        for tier, embedded in (('xcore', XCORE), ('xlib', XLIB)):
            live = found[tier]
            print(f'{tier}: {len(live)} derived, {len(embedded)} embedded')
            for label, diff in (('only in headers', live - embedded),
                                ('only embedded', embedded - live)):
                if diff:
                    rc = 1
                    print(f'  {label}: {" ".join(sorted(diff))}')
        print('✅ macro list matches the local X11 headers' if not rc
              else '❌ macro list has drifted from the local X11 headers')
        return rc

    tier_of = {}
    for n in XCORE:
        tier_of[n] = 'xcore'
    for n in XLIB:
        tier_of[n] = 'xlib'
    macros = set(tier_of)

    files = read_repo_files(root)
    directives = build_include_graph(root, files)
    taint = taint_lines(directives)
    exposed = sorted(taint)

    if args.exposed:
        for p in exposed:
            ln = taint[p]
            where = 'whole file' if ln <= 1 else f'from line {ln}'
            print(f'{p}  ({where})')
        print(f'  {len(exposed)} files are preprocessed with an X11 macro live')
        return 0

    wanted = None
    if args.paths:
        wanted = {os.path.relpath(os.path.abspath(p), root).replace(os.sep, '/')
                  for p in args.paths}

    hits = []
    for p in exposed:
        if wanted is not None and p not in wanted:
            continue
        hits += scan_file(p, files[p] or '', macros, tier_of, taint[p])

    fatal = [h for h in hits if h[4] == 'xcore' or args.strict]
    advisory = [] if args.strict else [h for h in hits if h[4] != 'xcore']

    if args.list:
        for path, ln, rule, macro, tier, text in hits:
            mark = 'FATAL' if (tier == 'xcore' or args.strict) else 'advisory'
            print(f'{path}:{ln}: [{rule}/{tier}] `{macro}` collides with an X11 macro '
                  f'({mark})\n      {text}')
        print()

    if args.list or args.summary:
        print(f'  exposed files      {len(exposed):>5}')
        print(f'  xcore (X.h)        {len([h for h in hits if h[4] == "xcore"]):>5}   fatal')
        print(f'  xlib (Xlib/Xutil)  {len([h for h in hits if h[4] == "xlib"]):>5}   '
              f'{"fatal (--strict)" if args.strict else "advisory"}')

    if advisory and not args.list:
        print(f'ℹ️  X11 macros: {len(advisory)} advisory (xlib-tier) collision(s); '
              f'run --list to see them.')

    total = len(fatal)
    limit = args.max_allowed
    if limit is not None and total > limit:
        print(f'❌ X11 macro collisions: {total} exceeds baseline ({limit}).')
    elif limit is None and total:
        print(f'❌ X11 macro collisions: {total} identifier(s) an X11 macro would rewrite.')
    else:
        if limit is not None and total < limit:
            print(f'✅ X11 macro collisions: {total} (baseline {limit} — ratchet it down)')
        else:
            print(f'✅ X11 macro collisions: none in the {len(exposed)} X11-exposed files')
        return 0

    print('   These names sit in a translation unit that reaches <SDL.h>/<X11/*>, where')
    print('   the X11 header defines them as object-like macros — the preprocessor')
    print('   rewrites them even through a `::`. Rename the identifier (InvalidationScope')
    print('   ::None became ::Nothing in b942c8b1e), or annotate `// X11_MACRO_OK: reason`.')
    print('   Run: python3 scripts/check_x11_macro_collisions.py --list')
    return 1


if __name__ == '__main__':
    sys.exit(main())
