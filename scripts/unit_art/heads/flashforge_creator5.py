# SPDX-License-Identifier: GPL-3.0-or-later
"""FlashForge Creator 5, seen sideways: a near-square rail block, a narrower detachable tool beside it, and air channels either side of the nozzle."""
import math

from ..head import Drawing, HeadDef, rounded, wordmark

BLOCK, RECESS, TOOL, DUCT, DARK, HOTEND = "#7a7c81", "#5f6166", "#6c6e73", "#73757a", "#121315", "#1c1d20"
TOP = 1.16
BLOCK_LEFT, BLOCK_RIGHT, BLOCK_BOTTOM, BLOCK_DEPTH = -0.56, 0.115, 0.33, 0.66
TOOL_LEFT, TOOL_RIGHT, TOOL_BOTTOM, TOOL_FRONT, TOOL_BACK = 0.128, 0.56, 0.284, 0.17, 0.49
NOZZLE_Y = BLOCK_DEPTH / 2
FAN_Z = 0.53
# The rail runs front to back in a slot across the top of the block, against the tool.
RAIL_CUT_LEFT, RAIL_CUT_FLOOR = BLOCK_RIGHT - 0.23, TOP - 0.22
BLOCK_OUTLINE = [
    (BLOCK_LEFT, BLOCK_BOTTOM), (BLOCK_RIGHT, BLOCK_BOTTOM), (BLOCK_RIGHT, RAIL_CUT_FLOOR), (RAIL_CUT_LEFT, RAIL_CUT_FLOOR),
    (RAIL_CUT_LEFT, TOP), (BLOCK_LEFT, TOP),
]
TIP = (0.365, NOZZLE_Y, -0.08)
CHANNEL_END = [(0.43, 0.108), (0.455, 0.12), (0.465, 0.145), (0.465, 0.185), (0.45, 0.215)]
CHANNEL_OUTLINE = [(BLOCK_RIGHT, BLOCK_BOTTOM), (BLOCK_RIGHT, 0.108)] + CHANNEL_END
BLOCK_FOOT_OUTLINE = [(BLOCK_LEFT, BLOCK_BOTTOM), (BLOCK_LEFT, 0.108)] + CHANNEL_END + [(BLOCK_RIGHT, BLOCK_BOTTOM)]
CHANNEL_WIDTH = 0.16


def _tool_fan(d):
    """The cooling fan on the tool's outer face, behind a spoked guard."""
    x, n = TOOL_RIGHT + 0.001, 32

    def ring(r, lift=0.0):
        return [(x + lift, NOZZLE_Y + r * math.cos(2 * math.pi * i / n), FAN_Z + r * math.sin(2 * math.pi * i / n)) for i in range(n)]

    d.facet(ring(0.15), "#85878c")
    d.facet(ring(0.13, 0.001), DARK, amb=0.9, dif=0.2, spec=0.0)
    d.facet(ring(0.06, 0.002), "#85878c")
    d.facet(ring(0.045, 0.003), "#2b2c30", amb=0.8, dif=0.45, spec=0.2)
    if d.detail:
        for i in range(8):
            t = 2 * math.pi * i / 8
            d.scene.line([(x + 0.002, NOZZLE_Y + r * math.cos(t), FAN_Z + r * math.sin(t)) for r in (0.06, 0.13)], "#85878c", 0.008)


def _tool(d):
    d.solid(rounded([(TOOL_LEFT, TOOL_BOTTOM), (TOOL_RIGHT, TOOL_BOTTOM), (TOOL_RIGHT, TOP), (TOOL_LEFT, TOP)], 0.045), TOOL_FRONT, TOOL_BACK, TOOL, bevel=0.016)
    y = TOOL_FRONT - 0.002
    d.face(rounded([(0.23, 0.42), (0.38, 0.42), (0.38, 0.62), (0.23, 0.62)], 0.012), y, DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        for i in range(4):
            z = 0.455 + i * 0.045
            d.face([(0.235, z), (0.375, z), (0.375, z + 0.014), (0.235, z + 0.014)], y - 0.001, "#55575c")
    _tool_fan(d)
    d.tube(0.34, NOZZLE_Y, TOP, TOP + 0.05, 0.05, "#26272b")
    d.tube(0.34, NOZZLE_Y, TOP + 0.05, TOP + 0.16, 0.022, "#e8e8e6")


def _hotend(d):
    """The tall black hotend hanging from the tool, between the two air channels."""
    y0, y1 = NOZZLE_Y - 0.075, NOZZLE_Y + 0.075
    d.solid([(0.31, TOOL_BOTTOM + 0.01), (0.42, TOOL_BOTTOM + 0.01), (0.42, -0.005), (0.385, -0.04), (0.345, -0.04), (0.31, -0.005)], y0, y1, HOTEND)
    d.nozzle(TIP[0], TIP[1], -0.04, r_top=0.02, r_tip=0.008, z_tip=TIP[2])


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid(CHANNEL_OUTLINE, BLOCK_DEPTH - CHANNEL_WIDTH, BLOCK_DEPTH, DUCT)
    d.solid([(BLOCK_LEFT, 0.108), (BLOCK_RIGHT, 0.108), (BLOCK_RIGHT, BLOCK_BOTTOM), (BLOCK_LEFT, BLOCK_BOTTOM)], CHANNEL_WIDTH, BLOCK_DEPTH, DUCT)
    d.solid(BLOCK_FOOT_OUTLINE, 0.0, CHANNEL_WIDTH, DUCT)
    d.solid(BLOCK_OUTLINE, 0.0, BLOCK_DEPTH, BLOCK)
    if d.detail:
        wordmark(d, "FLASHFORGE", -0.22, 0.43, 0.042, 0.062, 0.012, "#e4e5e7", 0.008)
    _hotend(d)
    _tool(d)
    # The near channel passes in front of the tool and hotend, continuing the foot it was drawn with.
    d.solid(CHANNEL_OUTLINE, 0.0, CHANNEL_WIDTH, DUCT)


HEAD = HeadDef("flashforge_creator5", "FLASHFORGE_CREATOR5", draw_art, TIP)
