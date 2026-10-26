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
# MAME names its output directory after the host toolchain. On Linux the
# archives are linked into the VST3 shared object, so they must be PIC.
EXTRA_MAKE_ARGS=()
DEFAULT_JOBS=16
case "$(uname -s)" in
	Darwin)
		MAME_TOOLCHAIN_DIR=osx_clang ;;
	Linux)
		# MAME's default Linux goal always builds with gcc into linux_gcc
		# (its makefile ignores an environment CC).
		if [[ "$(uname -m)" != x86_64 ]]; then
			echo "error: only x86_64 Linux is supported (got $(uname -m))" >&2
			exit 1
		fi
		MAME_TOOLCHAIN_DIR=linux_gcc
		EXTRA_MAKE_ARGS+=("ARCHOPTS=-fPIC")
		# MAME's emumem_* files need ~1.5 GB per cc1plus, and an OOM-killed compile
		# can leave an empty .o that make later treats as up to date. Default to
		# one job per 2 GB of RAM, capped at the CPU count.
		mem_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
		DEFAULT_JOBS=$(( mem_kb / (2 * 1024 * 1024) ))
		cpus="$(nproc)"
		if (( DEFAULT_JOBS > cpus )); then DEFAULT_JOBS=$cpus; fi
		if (( DEFAULT_JOBS < 1 )); then DEFAULT_JOBS=1; fi ;;
	*)
		echo "error: unsupported host $(uname -s); use the Windows build docs" >&2
		exit 1 ;;
esac
MAME_BIN="$MAME_DIR/build/$MAME_TOOLCHAIN_DIR/bin/x64/Release"
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
(cd "$MAME_DIR" && JOBS="${JOBS:-$DEFAULT_JOBS}" ./scripts/korgprophecy_build.sh "$@" ${EXTRA_MAKE_ARGS[@]+"${EXTRA_MAKE_ARGS[@]}"} PYTHON_EXECUTABLE="$PYBIN")

REV="$(git -C "$MAME_DIR" rev-parse HEAD)"
DIRTY=""
if ! git -C "$MAME_DIR" diff --quiet HEAD -- src/ 2>/dev/null; then
	DIRTY="-dirty"
fi
echo "${REV}${DIRTY}" > "$STAMP"
echo "stamped $STAMP = ${REV}${DIRTY}"
