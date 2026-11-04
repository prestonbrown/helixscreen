# SPDX-License-Identifier: GPL-3.0-or-later
#
# An ssh stand-in for the test-host-run.sh suites that can lose its link.
#
# install_drop_ssh replaces ssh with one that runs its command here and relays
# the output, and drops every connection open when MOCK_SSH_DROP appears: the
# output is cut and ssh exits 255. Connections made after the drop stay up. A
# detached job is no child of the connection and keeps running.
#
# The remote command's stderr is relayed with its stdout, so nothing it leaves
# running holds this ssh's own stderr open past the drop.
#
# How the far end takes the drop depends on MOCK_SSH_DROP_KEEP. Unset, the
# remote command is killed, as sshd's closing pipes end a shell blocked on its
# stdin. Set, it keeps running with its stdin open and its output going
# nowhere, as when sshd has not yet noticed the link is dead; the stand-in
# holding that stdin open is recorded in $BATS_TEST_TMPDIR/ssh-keepers.

install_drop_ssh() {
    export MOCK_SSH_DROP="$BATS_TEST_TMPDIR/link-down" HELIX_TEST_RECONNECT_PAUSE_SECS=0
    cat > "$BATS_TEST_TMPDIR/ssh-drop" <<'SSH'
#!/usr/bin/env bash
while [ $# -gt 0 ]; do
    case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac
done
shift
case "$1" in bash) ;; *) exec bash -c "$*" ;; esac
pre=0; [ ! -e "$MOCK_SSH_DROP" ] || pre=1
out=$(mktemp "$BATS_TEST_TMPDIR/ssh-out.XXXXXX")
if [ -n "${MOCK_SSH_DROP_KEEP:-}" ]; then
    fifo="$out.in"
    mkfifo "$fifo"
    bash -c "$*" < "$fifo" > "$out" 2>&1 &
    pid=$!
    exec 5>"$fifo"
    cat <&0 >&5 &
    copier=$!
    exec 5>&-
else
    bash -c "$*" <&0 > "$out" 2>&1 &
    pid=$!
fi
sent=0
flush() {
    local size
    size=$(( $(wc -c < "$out") ))
    if [ "$size" -gt "$sent" ]; then tail -c +$((sent + 1)) "$out" | head -c $((size - sent)); sent=$size; fi
}
while kill -0 "$pid" 2>/dev/null; do
    flush
    if [ "$pre" = 0 ] && [ -e "$MOCK_SSH_DROP" ]; then
        if [ -n "${MOCK_SSH_DROP_KEEP:-}" ]; then
            ( exec 6>"$fifo"; exec sleep 60 ) </dev/null >/dev/null 2>&1 &
            echo $! >> "$BATS_TEST_TMPDIR/ssh-keepers"
            kill "$copier" 2>/dev/null
        else
            kill "$pid" 2>/dev/null
        fi
        exit 255
    fi
    sleep 0.05
done
wait "$pid"; rc=$?; flush; exit "$rc"
SSH
    chmod +x "$BATS_TEST_TMPDIR/ssh-drop"
    mock_command_script ssh 'exec "$BATS_TEST_TMPDIR/ssh-drop" "$@"'
}
