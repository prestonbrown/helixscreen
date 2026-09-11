# Shared by pre-commit and pre-merge-commit: announce that this process is
# holding the tree, so a peer reading the tree from outside need not guess
# whether a stalled-looking merge is live. Advisory; never fails the operation
# unless HELIX_CLAIM_STRICT=1.
#
# usage: claim_tree "<what is happening>"
#
# The claim carries $PPID, the git process, so it dies when git does and a
# crashed operation needs no cleanup.
# Whether pid $1 is an ancestor of this hook's git process, i.e. whether the
# claim belongs to the session running this merge. The claim a careful session
# takes BEFORE merging is held by that session, not by the git process, so
# without this the hook reports the caller to itself as a foreign holder.
# /proc field 2 after the last ')' is ppid; comm can contain spaces and parens.
_claim_is_own_session() {
    _p=$PPID
    _hops=0
    while [ "${_p:-0}" -gt 1 ] && [ "$_hops" -lt 40 ]; do
        [ "$_p" = "$1" ] && return 0
        _p=$(sed 's/.*) //' "/proc/$_p/stat" 2>/dev/null | awk '{print $2}') || return 1
        [ -n "$_p" ] || return 1
        _hops=$((_hops + 1))
    done
    return 1
}

claim_tree() {
    [ -x scripts/helix-claim ] || return 0
    _tree="worktree:$(basename "$(git rev-parse --show-toplevel)")"
    if ! scripts/helix-claim check "$_tree" >/dev/null 2>&1; then
        # A claim this session already holds is not a collision: say nothing and
        # leave it in place, so post-commit/post-merge cannot drop it either.
        _holder=$(scripts/helix-claim check "$_tree" 2>/dev/null |
                  sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | head -1)
        if [ -n "$_holder" ] && _claim_is_own_session "$_holder"; then
            return 0
        fi
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
