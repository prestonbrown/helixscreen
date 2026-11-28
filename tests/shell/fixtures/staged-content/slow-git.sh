# shellcheck shell=bash
# A slow index for a test: git holds its lock for a second around every write,
# so two writes in one commit overlap for certain and refuse one of them,
# exactly as git does. Every other command is the real git at once.
#
# A shell function, exported to the hook, not a git on the PATH: git puts its
# own directory first on a hook's PATH, so a git on the PATH never reaches the
# hook that `git commit` runs. The lock is the one git would take, the index it
# is pointed at plus .lock; under `git commit -- <paths>` that ends in .lock.lock.
# The lock is moved aside, never removed: the scratch repo is bats' to delete.
git() {
    case "$1" in
        add | update-index)
            local index="${GIT_INDEX_FILE:-$(command git rev-parse --git-dir)/index}"
            if ( set -C; : > "$index.lock" ) 2>/dev/null; then
                sleep 1
                mv "$index.lock" "$index.lock.held.$$"
            else
                echo "fatal: Unable to create '$index.lock': File exists" >&2
                return 128
            fi
            ;;
    esac
    command git "$@"
}
