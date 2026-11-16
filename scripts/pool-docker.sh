#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# pool-docker.sh: run a container build inside the machine's jobpool budget.
#
#     scripts/pool-docker.sh docker run [OPTIONS] IMAGE make [ARGS...]
#     scripts/pool-docker.sh docker run [OPTIONS] IMAGE CMD [ARGS...]
#
# Any client with docker's `run` syntax works in place of docker.
#
# A make in the container joins the pool itself: the pool's state dir is
# mounted, the container opens the FIFO and exports MAKEFLAGS (`jobpool
# docker-args` / `container-env`), and the make's own -j is dropped, since a
# -j makes make leave a jobserver. If the container cannot open the FIFO
# (a remapped uid, a VM that cannot share a FIFO) that make keeps its -j.
#
# An idf.py or ninja command joins too, through ninja 1.13's jobserver client,
# so its jobs grow and shrink with the pool for the whole build: the state dir
# is mounted, scripts/ninja-jobserver.sh's ninja (fetched once, then cached) is
# mounted over the image's /usr/bin/ninja, MAKEFLAGS names the FIFO, and
# IDF_PY_BUILD_JOBS is emptied, since idf.py passes it as -j and any -j turns
# ninja's jobserver off. MAKE names scripts/container-make.sh, mounted in, so the
# firmware's LTO link (lto-wrapper runs $MAKE) joins the same pool. The container's root, or the -u uid, must be able to
# open the FIFO; one that cannot leaves ninja at its own default parallelism.
#
# Any other command (a build script), or idf.py when no ninja 1.13 can be had,
# cannot join, so it runs under `helix-claim hold`, which holds tokens for the
# container's life and passes their count in as JOBPOOL_SLOTS and
# IDF_PY_BUILD_JOBS (idf.py's ninja -j).
#
# Without jobpool (CI, a Mac, a fresh clone), with JOBPOOL=0, or when the pool
# will not start, the command runs exactly as given.
set -u

jp=${HELIX_JOBPOOL:-jobpool}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ "${JOBPOOL:-}" = 0 ] || ! command -v "$jp" >/dev/null 2>&1; then exec "$@"; fi
if [ $# -lt 3 ] || [ "$2" != run ]; then exec "$@"; fi
docker=$1
shift 2

opts=()
while [ $# -gt 0 ] && [ "$1" != make ]; do
    opts+=("$1")
    shift
done

dir=/run/jobpool
# The LTO link's make: gcc's lto-wrapper runs $MAKE, and the image's make cannot
# read the fifo MAKEFLAGS ninja needs (scripts/container-make.sh).
container_make=/usr/local/bin/helix-container-make
if [ $# -eq 0 ]; then
    ninja_cmd='(^|[[:space:];&|(])(idf\.py|ninja)([[:space:]]|$)'
    if [[ " ${opts[*]} " =~ $ninja_cmd ]] && mount=$("$jp" docker-args "$dir" 2>/dev/null) &&
        ninja=$("$here/ninja-jobserver.sh"); then
        exec env -u MAKEFLAGS -u MFLAGS "$jp" exec -- "$docker" run "$mount" \
            "--volume=$ninja:/usr/bin/ninja:ro" \
            "--volume=$here/container-make.sh:$container_make:ro" -e "MAKE=$container_make" \
            -e "MAKEFLAGS= -j --jobserver-auth=fifo:$dir/fifo" -e IDF_PY_BUILD_JOBS= "${opts[@]}"
    fi
    # shellcheck disable=SC2016  # expanded by the inner sh, under the hold
    exec "$here/helix-claim" hold -- sh -c \
        'exec "$0" run -e JOBPOOL_SLOTS -e IDF_PY_BUILD_JOBS="$JOBPOOL_SLOTS" "$@"' \
        "$docker" "${opts[@]}"
fi

shift
mount=$("$jp" docker-args "$dir" 2>/dev/null) || exec "$docker" run "${opts[@]}" make "$@"
join=$("$jp" container-env "$dir") || exec "$docker" run "${opts[@]}" make "$@"
args=() jflags=""
for a in "$@"; do
    case $a in
        -j* | --jobs*) jflags="$jflags $a" ;;
        *) args+=("$a") ;;
    esac
done
# Without MAKEFLAGS, exec registers this run with the pool even under a make
# whose jobserver is private (`make -j4`, no shim), which would otherwise hide
# the container's tokens from the daemon's idle reset.
exec env -u MAKEFLAGS -u MFLAGS "$jp" exec -- "$docker" run "$mount" "${opts[@]}" sh -c \
    "if (exec 3<>$dir/fifo) 2>/dev/null; then $join; else set -- \"\$@\"$jflags; fi; exec make \"\$@\"" \
    make "${args[@]}"
