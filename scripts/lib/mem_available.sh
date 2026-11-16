# shellcheck shell=sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# helix_mem_available_kb: what a new process may allocate here, in kB.
#
# MemAvailable, bounded by the memory limit of the cgroup this runs in. Inside a
# capped container /proc/meminfo still reports the whole host, so a -j sized
# from it alone overruns the cap and the kernel kills compilers inside it. The
# limit counts only memory the cgroup cannot give back: its anonymous memory
# is subtracted, its file cache is not, since a cap reclaims cache first.
#
# Prints MemAvailable unchanged (empty where there is no /proc/meminfo, as on a
# Mac) when the cgroup has no limit. POSIX sh: test-host-run.sh also pipes this
# file into the test container's sh. HELIX_MEMINFO and HELIX_CGROUP_ROOT point
# the reads elsewhere for tests.

helix_mem_available_kb() {
    _hma_host=$(awk '/^MemAvailable:/ {print $2}' "${HELIX_MEMINFO:-/proc/meminfo}" 2>/dev/null)
    _hma_root=${HELIX_CGROUP_ROOT:-/sys/fs/cgroup}
    _hma_max=$(cat "$_hma_root/memory.max" 2>/dev/null)
    case "$_hma_max" in
        '' | max | *[!0-9]*)
            printf '%s\n' "$_hma_host"
            return 0
            ;;
    esac
    _hma_anon=$(sed -n 's/^anon \([0-9][0-9]*\)$/\1/p' "$_hma_root/memory.stat" 2>/dev/null)
    _hma_kb=$(( (_hma_max - ${_hma_anon:-0}) / 1024 ))
    [ "$_hma_kb" -ge 0 ] || _hma_kb=0
    if [ -n "$_hma_host" ] && [ "$_hma_host" -lt "$_hma_kb" ]; then
        _hma_kb=$_hma_host
    fi
    printf '%s\n' "$_hma_kb"
}
