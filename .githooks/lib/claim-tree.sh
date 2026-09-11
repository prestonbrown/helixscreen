# Shared by pre-commit and pre-merge-commit: announce that this process is
# holding the tree, so a peer reading the tree from outside need not guess
# whether a stalled-looking merge is live. Advisory; never fails the operation
# unless HELIX_CLAIM_STRICT=1.
#
# usage: claim_tree "<what is happening>"
#
# The claim carries $PPID, the git process, so it dies when git does and a
# crashed operation needs no cleanup.
claim_tree() {
    [ -x scripts/helix-claim ] || return 0
    _tree="worktree:$(basename "$(git rev-parse --show-toplevel)")"
    if ! scripts/helix-claim check "$_tree" >/dev/null 2>&1; then
        printf '\n  Another session is working in this tree:\n' >&2
        scripts/helix-claim check "$_tree" >&2
        if [ "${HELIX_CLAIM_STRICT:-0}" = "1" ]; then
            printf '  Refusing: HELIX_CLAIM_STRICT=1.\n\n' >&2
            return 1
        fi
        printf '  Proceeding anyway (advisory). HELIX_CLAIM_STRICT=1 to refuse.\n\n' >&2
        return 0
    fi
    scripts/helix-claim take "$_tree" "$1" \
        --pid "$PPID" --note "git hook running; may take many minutes" >/dev/null 2>&1
    return 0
}
