#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# build-image.sh - build the QNX 8.0 IFS of the Passerelle guest (see README.md).
#
# Usage:
#   ./build-image.sh            # image at build/qnx8/passerelle.ifs
#
# Environment:
#   QNX_SDP_ENV   path to qnxsdp-env.sh (default: $HOME/qnx800/qnxsdp-env.sh)
#
# The fragment skips fs9p/fsfuse when they are not built yet, so the image
# boots with the kernel, the C library and the PCI server alone.

set -eu

SDP_ENV="${QNX_SDP_ENV:-$HOME/qnx800/qnxsdp-env.sh}"

# mkifs needs QNX_HOST / QNX_TARGET / PROCESSOR in its environment.
if [ -z "${QNX_TARGET:-}" ]; then
    if [ ! -f "$SDP_ENV" ]; then
        echo "SDP not found: $SDP_ENV (set QNX_SDP_ENV)" >&2
        exit 1
    fi
    . "$SDP_ENV"
fi

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD="$ROOT/build/qnx8"
mkdir -p "$BUILD/stage"

stage() {
    # Copy a built driver into the staging dir when it is present; the fragment's
    # [+optional] entries skip the ones that do not exist.
    src="$1"
    name="$2"
    if [ -x "$src" ]; then
        cp "$src" "$BUILD/stage/$name"
        echo "staged $name from $src"
    else
        echo "note: $name not built yet - the image boots without it"
    fi
}

stage "$ROOT/build-qnx/drivers/fs9p" fs9p
stage "$ROOT/build-qnx/drivers/fsfuse" fsfuse

# One cross-compile, one message: every guest program is built the same way, and
# a missing source or a missing SDP compiler leaves the image booting without it
# (the fragment's entries are [+optional]).
#   build_for_target <source.c> <out-name> [dir]
# `dir` defaults to the staging directory; the FS-06 demo goes to the share, so
# the guest can reach it through the driver instead of through /proc/boot.
build_for_target() {
    src="$1"
    name="$2"
    dir="${3:-$BUILD/stage}"
    if [ ! -f "$src" ]; then
        echo "note: $src absent - the image boots without $name"
        return 0
    fi
    mkdir -p "$dir"
    if qcc -Vgcc_ntox86_64 -D_QNX_SOURCE=1 -O2 -o "$dir/$name" "$src" 2>/dev/null; then
        echo "built $name"
    else
        echo "note: $name did not build (no SDP compiler?)"
    fi
}

stage "$ROOT/build-qnx/drivers/fs9p" fs9p
stage "$ROOT/build-qnx/drivers/fsfuse" fsfuse

QNX8="$ROOT/tests/qemu/qnx8"
build_for_target "$QNX8/qbench.c" qbench
build_for_target "$QNX8/clockprobe.c" clockprobe
build_for_target "$QNX8/mapprobe.c" mapprobe
build_for_target "$QNX8/fs6-demo.c" fs6-demo "$BUILD/share"

cd "$ROOT"
mkifs -v -v -o "$BUILD" tests/qemu/qnx8/passerelle.build "$BUILD/passerelle.ifs"
ls -l "$BUILD/passerelle.ifs"