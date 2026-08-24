#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_font_tier_coverage.py.
#
# Both halves of the contract matter: the shape it must catch (a platform whose
# max tier crosses a `#if HELIX_MAX_FONT_TIER >= N` guard without that tier's
# faces in its sources) and the shapes it must stay quiet about (a platform
# whose guard is false, and `FONT_TIERS := all`). A gate that fires on correct
# config gets switched off.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
GATE="$WORKTREE_ROOT/scripts/check_font_tier_coverage.py"

setup() {
    load helpers
    FAKE="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$FAKE/mk" "$FAKE/src/application" "$FAKE/src/system" "$FAKE/scripts"
    cp "$GATE" "$FAKE/scripts/"
}

# Write a synthetic tree. Args: $1 = FONT_TIERS value for the one platform,
# $2 = the medium-tier selection condition in mk/fonts.mk.
make_tree() {
    local tiers="$1" medium_cond="$2"

    cat > "$FAKE/mk/fonts.mk" << EOF
FONTS_CORE := assets/fonts/noto_sans_14.c assets/fonts/noto_sans_16.c
FONTS_MICRO := assets/fonts/noto_sans_8.c
FONTS_TINY := assets/fonts/noto_sans_10.c
FONTS_SMALL := assets/fonts/noto_sans_12.c
FONTS_MEDIUM := assets/fonts/noto_sans_26.c
FONTS_LARGE := assets/fonts/noto_sans_28.c
FONTS_XLARGE := assets/fonts/noto_sans_32.c
FONTS_XXLARGE := assets/fonts/noto_sans_40.c

FONT_TIERS ?= all
ifeq (\$(FONT_TIERS),all)
    TIER_FONT_SRCS := \$(FONTS_ALL)
else
    TIER_FONT_SRCS := \$(FONTS_CORE)
    ifneq (\$(filter micro,\$(FONT_TIERS)),)
        TIER_FONT_SRCS += \$(FONTS_MICRO)
    endif
    ifneq (\$(filter tiny,\$(FONT_TIERS)),)
        TIER_FONT_SRCS += \$(FONTS_TINY)
    endif
    ifneq (\$(filter small,\$(FONT_TIERS)),)
        TIER_FONT_SRCS += \$(FONTS_SMALL)
    endif
    ifneq (\$(filter ${medium_cond},\$(FONT_TIERS)),)
        TIER_FONT_SRCS += \$(FONTS_MEDIUM)
    endif
    ifneq (\$(filter large xlarge xxlarge,\$(FONT_TIERS)),)
        TIER_FONT_SRCS += \$(FONTS_LARGE)
    endif
    ifneq (\$(filter xlarge xxlarge,\$(FONT_TIERS)),)
        TIER_FONT_SRCS += \$(FONTS_XLARGE)
    endif
    ifneq (\$(filter xxlarge,\$(FONT_TIERS)),)
        TIER_FONT_SRCS += \$(FONTS_XXLARGE)
    endif
endif
EOF

    cat > "$FAKE/mk/cross.mk" << EOF
else ifeq (\$(PLATFORM_TARGET),testplat)
    FONT_TIERS := ${tiers}
endif
EOF

    cat > "$FAKE/src/application/asset_manager.cpp" << 'EOF'
int register_medium_tier_fonts() {
#if HELIX_MAX_FONT_TIER >= 3
    lv_xml_register_font(nullptr, "noto_sans_26", &noto_sans_26);
#endif
    return 0;
}
int register_large_tier_fonts() {
#if HELIX_MAX_FONT_TIER >= 4
    lv_xml_register_font(nullptr, "noto_sans_28", &noto_sans_28);
#endif
    return 0;
}
EOF
    : > "$FAKE/src/system/cjk_font_manager.cpp"
}

@test "gate catches a platform whose max tier crosses a guard without the faces" {
    # "large xlarge" -> max 5, so `>= 3` compiles, but set-membership selection
    # leaves FONTS_MEDIUM out. This is the real k2 shape.
    make_tree "large xlarge" "medium"

    run python3 "$FAKE/scripts/check_font_tier_coverage.py"

    [ "$status" -eq 1 ]
    [[ "$output" == *"testplat"* ]]
    [[ "$output" == *"noto_sans_26"* ]]
}

@test "gate is silent when the selection condition uses >= semantics" {
    make_tree "large xlarge" "medium large xlarge xxlarge"

    run python3 "$FAKE/scripts/check_font_tier_coverage.py"

    [ "$status" -eq 0 ]
}

@test "gate is silent when the guard is false for that platform" {
    # micro tiny -> max 1, so `>= 3` never compiles; absent faces are correct.
    make_tree "micro tiny" "medium"

    run python3 "$FAKE/scripts/check_font_tier_coverage.py"

    [ "$status" -eq 0 ]
}

@test "gate is silent for FONT_TIERS := all" {
    make_tree "all" "medium"

    run python3 "$FAKE/scripts/check_font_tier_coverage.py"

    [ "$status" -eq 0 ]
}

@test "gate reports rather than passes when it cannot parse its inputs" {
    # An empty fonts.mk must not read as "nothing to check, all good" — a gate
    # that silently passes on a parse failure is worse than no gate.
    make_tree "large xlarge" "medium"
    : > "$FAKE/mk/fonts.mk"

    run python3 "$FAKE/scripts/check_font_tier_coverage.py"

    [ "$status" -eq 2 ]
    [[ "$output" == *"could not parse"* ]]
}

@test "gate passes against the real repository" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"platforms satisfy guards"* ]]
}
