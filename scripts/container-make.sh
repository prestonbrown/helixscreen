#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# container-make.sh: make for a container whose MAKEFLAGS names the pool's FIFO.
#
# scripts/pool-docker.sh gives an idf.py container MAKEFLAGS with
# `--jobserver-auth=fifo:PATH`, the form ninja 1.13 reads. gcc's lto-wrapper
# reads the same MAKEFLAGS at the firmware's LTO link and runs $MAKE, but the
# image's make (4.3) predates the fifo form and stops with "invalid
# --jobserver-auth string". This opens the FIFO on a descriptor and hands make
# the descriptor form it does read, so the link draws from the same pool.
# A FIFO it cannot open drops the jobserver and leaves make serial.

real=${HELIX_CONTAINER_REAL_MAKE:-/usr/bin/make}
case " ${MAKEFLAGS-} " in
*" --jobserver-auth=fifo:"*)
    fifo=${MAKEFLAGS#*--jobserver-auth=fifo:}
    fifo=${fifo%% *}
    if [ -p "$fifo" ] && [ -r "$fifo" ] && [ -w "$fifo" ]; then
        exec 9<>"$fifo"
        auth="--jobserver-auth=9,9"
    else
        auth=""
    fi
    MAKEFLAGS=$(printf '%s\n' "$MAKEFLAGS" | sed "s|--jobserver-auth=fifo:[^ ]*|$auth|")
    export MAKEFLAGS
    ;;
esac
exec "$real" "$@"
