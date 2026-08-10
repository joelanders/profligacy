#!/usr/bin/env bash
# Build the extern/mame archives AND stamp them with the submodule rev they were
# built from. CMake refuses to link archives whose stamp doesn't match the current
# submodule HEAD (scripts/check_mame_fresh.cmake) — this is what prevents the
# "fast-forwarded the submodule but linked week-old archives" failure (it shipped
# a stale app on 2026-07-14; don't bypass the stamp).
#
# Usage: scripts/build_mame_core.sh [extra make args, e.g. --regen JOBS=8]
# For an A/B package build without moving the release submodule pointer:
#   MAME_DIR=/path/to/mame-clean scripts/build_mame_core.sh
# Needs a real `python` on PATH; we pass PYTHON_EXECUTABLE and prepend a shim
# directory as a convenience when a version-manager shim is unavailable.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAME_DIR="${MAME_DIR:-$ROOT_DIR/extern/mame}"
MAME_BIN="$MAME_DIR/build/osx_clang/bin/x64/Release"
STAMP="$MAME_BIN/.prophecy_build_rev"

# Make sure a working `python` is first on PATH (MAME's makefile probes for one
# before honoring PYTHON_EXECUTABLE on some paths).
PYBIN="$(command -v python3 || true)"
if [[ -z "$PYBIN" ]]; then
	echo "error: no python3 on PATH" >&2
	exit 1
fi
SHIMDIR="$(mktemp -d)"
trap 'rm -rf "$SHIMDIR"' EXIT
ln -sf "$PYBIN" "$SHIMDIR/python"
export PATH="$SHIMDIR:$PATH"

# korgprophecy_build.sh recognizes --regen only as its first argument. Preserve
# the caller's option ordering; make variable assignments remain valid after it.
(cd "$MAME_DIR" && JOBS="${JOBS:-16}" ./scripts/korgprophecy_build.sh "$@" PYTHON_EXECUTABLE="$PYBIN")

REV="$(git -C "$MAME_DIR" rev-parse HEAD)"
DIRTY=""
if ! git -C "$MAME_DIR" diff --quiet HEAD -- src/ 2>/dev/null; then
	DIRTY="-dirty"
fi
echo "${REV}${DIRTY}" > "$STAMP"
echo "stamped $STAMP = ${REV}${DIRTY}"
