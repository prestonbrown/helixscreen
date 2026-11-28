#!/bin/sh
# A slow index for a test: a command that writes the index holds git's lock
# for a second first, so two writes that overlap refuse one of them, exactly
# as git does. Every other command is the real git at once.
# The path of the real git is substituted where REAL_GIT stands.
case "$1" in
    add | update-index)
        lock="$(REAL_GIT rev-parse --git-dir)/index.lock"
        if ( set -C; : > "$lock" ) 2>/dev/null; then
            sleep 1
            mv "$lock" "$lock.held.$$"
        else
            echo "fatal: Unable to create '$lock': File exists" >&2
            exit 128
        fi
        ;;
esac
exec REAL_GIT "$@"
