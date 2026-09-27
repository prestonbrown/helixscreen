#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The files the app writes into config/ at runtime are named in five hand-kept
# lists, each enforcing a different rule:
#
#   user        HELIX_USER_CONFIG_FILES     scripts/lib/installer/platform.sh
#               linked into printer_data so a Moonraker update keeps them
#   cpp         is_non_shippable_config_file  src/application/android_asset_extractor.cpp
#   gradle      nonShippableConfig          android/app/build.gradle
#               never packaged into, or extracted from, an APK
#   strip       release-strip-pii           mk/cross.mk
#               deleted from a release tarball
#   deploy      DEPLOY_RUNTIME_EXCLUDES     mk/cross.mk
#               never rsynced from a dev tree over a device's own state
#
# Every literal file name in any list must be matched by an entry in each of
# the four exclusion lists. The user list is narrower by design (only data worth
# keeping), so it contributes names but need not cover the others.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

# list<TAB>name<TAB>why the list legitimately leaves the name out
EXCEPTIONS="$(cat <<'TABLE'
cpp	helixscreen.env	shipped default the installer needs
gradle	helixscreen.env	shipped default the installer needs
strip	helixscreen.env	shipped default the installer needs
strip	settings.json	personal config is stripped per target, some ship a curated default
strip	settings-test.json	personal config is stripped per target, some ship a curated default
cpp	.disabled_services	written by the installer on the device, never exists in a build tree
gradle	.disabled_services	written by the installer on the device, never exists in a build tree
strip	.disabled_services	written by the installer on the device, never exists in a build tree
deploy	.disabled_services	written by the installer on the device, never exists in a build tree
TABLE
)"

list_user() {
    grep -m1 '^HELIX_USER_CONFIG_FILES=' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh" |
        sed 's/^[^"]*"//; s/".*$//' | tr ' ' '\n'
}

list_cpp() {
    local body
    body=$(sed -n '/^bool is_non_shippable_config_file/,/^}/p' \
        "$WORKTREE_ROOT/src/application/android_asset_extractor.cpp")
    printf '%s\n' "$body" | grep -o 'filename == "[^"]*"' | sed 's/.*"\(.*\)"/\1/'
    printf '%s\n' "$body" | grep -o 'rfind("[^"]*", 0)' | sed 's/rfind("\(.*\)", 0)/\1*/'
}

list_gradle() {
    sed -n '/^def nonShippableConfig = \[/,/^\]/p' "$WORKTREE_ROOT/android/app/build.gradle" |
        grep -o "'[^']*'" | tr -d "'"
}

list_strip() {
    sed -n '/^define release-strip-pii/,/^endef/p' "$WORKTREE_ROOT/mk/cross.mk" |
        grep -o '/config/[^ ]*' | sed 's|/config/||'
}

# Deploy coverage also counts DEPLOY_ASSET_EXCLUDES, which already keeps
# settings*.json and helixscreen.env off the device.
list_deploy() {
    grep -E '^DEPLOY_(RUNTIME|ASSET)_EXCLUDES :=' "$WORKTREE_ROOT/mk/cross.mk" |
        grep -o "\-\-exclude='[^']*'" | sed "s/--exclude='//; s/'$//"
}

# Names come only from DEPLOY_RUNTIME_EXCLUDES: the asset list is mostly source
# tree junk (test_gcodes, *.pyc) that is nobody's runtime state.
names_deploy() {
    grep -E '^DEPLOY_RUNTIME_EXCLUDES :=' "$WORKTREE_ROOT/mk/cross.mk" |
        grep -o "\-\-exclude='[^']*'" | sed "s/--exclude='//; s/'$//"
}

# Every non-glob entry of every list.
all_names() {
    { list_user; list_cpp; list_gradle; list_strip; names_deploy; } | grep -v '[*?]' | sort -u
}

# 0 when some entry of list $1 matches file name $2 as a shell glob.
covers() {
    local pattern
    while IFS= read -r pattern; do
        [ -n "$pattern" ] || continue
        # shellcheck disable=SC2053 # the entry IS the glob
        [[ "$2" == $pattern ]] && return 0
    done < <("list_$1")
    return 1
}

excepted() {
    printf '%s\n' "$EXCEPTIONS" | grep -qF "$(printf '%s\t%s\t' "$1" "$2")"
}

@test "every parser finds its list" {
    # A parser that silently reads nothing would make every check below vacuous.
    for list in user cpp gradle strip deploy; do
        run "list_$list"
        [ "$status" -eq 0 ]
        [ "$(printf '%s\n' "$output" | grep -c .)" -ge 4 ] || {
            echo "list_$list found fewer than 4 entries: $output"
            return 1
        }
    done
}

@test "every runtime file is in every exclusion list" {
    local missing="" name list
    while IFS= read -r name; do
        for list in cpp gradle strip deploy; do
            covers "$list" "$name" || excepted "$list" "$name" || missing+="$list: $name"$'\n'
        done
    done < <(all_names)
    [ -z "$missing" ] || {
        printf 'missing (add the name, or an EXCEPTIONS row saying why):\n%s' "$missing"
        return 1
    }
}

@test "every exception still names a file some list mentions" {
    # An exception outliving its file would quietly excuse the next one with that name.
    local names stale="" list name
    names=$(all_names)
    while IFS=$'\t' read -r list name _; do
        printf '%s\n' "$names" | grep -qxF "$name" || stale+="$list: $name"$'\n'
        covers "$list" "$name" && stale+="$list: $name (list now covers it)"$'\n'
    done <<<"$EXCEPTIONS"
    [ -z "$stale" ] || {
        printf 'stale exceptions:\n%s' "$stale"
        return 1
    }
}
