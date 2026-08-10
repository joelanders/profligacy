#!/usr/bin/env bash
#
# build_console.sh - build ./console_host from THIS (top-level) repo.
#
# Demonstrates the target structure: our code lives at the top level (src/), and
# MAME is a dependency underneath (extern/mame, a submodule of the fork). This
# compiles src/console_main.cpp against MAME's headers and links the static
# archives MAME's own build produced. Nothing from MAME is copied or vendored here.
#
# MAME_DIR points at the MAME fork tree (source + its build/). It defaults to the
# submodule; for a quick prototype before the submodule is built you can point it
# at an already-built sibling worktree:
#
#   MAME_DIR=../mame-profligacy ./scripts/build_console.sh
#
# A/B builds can keep independent objects and executables:
#   PROPHOST_BUILD_DIR=build-clean PROPHOST_CONSOLE_OUT=console_host-clean \
#     PROPHOST_LIFECYCLE_OUT=lifecycle_host-clean \
#     MAME_DIR=../mame-clean ./scripts/build_console.sh
#
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

MAME_DIR="${MAME_DIR:-$REPO/extern/mame}"
BIN="$MAME_DIR/build/osx_clang/bin/x64/Release"
OBJ="$MAME_DIR/build/osx_clang/obj/x64/Release"
HOST_BUILD_DIR="${PROPHOST_BUILD_DIR:-$REPO/build}"
CONSOLE_OUT="${PROPHOST_CONSOLE_OUT:-$REPO/console_host}"
LIFECYCLE_OUT="${PROPHOST_LIFECYCLE_OUT:-$REPO/lifecycle_host}"

if [[ ! -f "$BIN/libemu.a" ]]; then
	echo "error: MAME archives not found under $BIN" >&2
	echo "  build the submodule once (~30 min foundation build):" >&2
	echo "    scripts/build_mame_core.sh --regen" >&2
	echo "    (needs a real 'python' on PATH -- see README)" >&2
	echo "  or point MAME_DIR at a prebuilt tree:  MAME_DIR=/path/to/mame $0" >&2
	exit 1
fi
for o in "$OBJ/src/mame/mame.o" "$OBJ/generated/mame/propmin/drivlist.o" "$OBJ/generated/version.o"; do
	[[ -f "$o" ]] || { echo "error: missing prebuilt object $o" >&2; exit 1; }
done
cmake -DMAME_BIN="$BIN" -P "$REPO/scripts/check_mame_host_abi.cmake"

mkdir -p "$HOST_BUILD_DIR"

SDL_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
DEFINES="-DNDEBUG -DCRLF=2 -DLSB_FIRST -DFLAC__NO_DLL -DPUGIXML_HEADER_ONLY -DASMJIT_STATIC \
-DLUA_COMPAT_ALL -DLUA_COMPAT_5_1 -DLUA_COMPAT_5_2 -DUSE_NETWORK -DOSD_NET_USE_PCAP -DSDLMAME_NO_X11 \
-DUSE_XINPUT=0 -DUSE_XINPUT_WII_LIGHTGUN_HACK=0 -DSDLMAME_SDL2=1 -DOSD_SDL -DSDLMAME_UNIX \
-DMACOSX_USE_LIBSDL -DSDLMAME_MACOSX -DSDLMAME_DARWIN"
INCLUDES="-I$MAME_DIR/src/osd -I$MAME_DIR/src/emu -I$MAME_DIR/src/devices -I$MAME_DIR/src/mame \
-I$MAME_DIR/src/lib -I$MAME_DIR/src/lib/util -I$MAME_DIR/3rdparty -I$MAME_DIR/build/generated/mame/layout \
-I$MAME_DIR/3rdparty/zlib -I$MAME_DIR/3rdparty/flac/include -I$MAME_DIR/src/osd/sdl -I$MAME_DIR/src/osd/modules \
-I${SDL_PREFIX}/include $(sdl2-config --cflags)"
CXXFLAGS="-std=c++17 -arch arm64 -O2 -pipe -fno-strict-aliasing -Wno-unknown-pragmas -Wno-unknown-warning-option \
-Wno-conversion -Wno-sign-compare -Wno-deprecated-declarations -Wno-unused-value -Wno-tautological-compare"

echo ">> compiling src/prophecy_engine.cpp  (MAME-side, MAME_DIR=$MAME_DIR)"
clang++ $CXXFLAGS $DEFINES $INCLUDES -Isrc -c src/prophecy_engine.cpp -o "$HOST_BUILD_DIR/prophecy_engine.o"

echo ">> compiling src/console_main.cpp  (host-side, NO MAME headers)"
clang++ -std=c++17 -arch arm64 -O2 -Isrc -c src/console_main.cpp -o "$HOST_BUILD_DIR/console_main.o"

echo ">> compiling src/lifecycle_main.cpp  (host-side, NO MAME headers)"
clang++ -std=c++17 -arch arm64 -O2 -Isrc -c src/lifecycle_main.cpp -o "$HOST_BUILD_DIR/lifecycle_main.o"

# Static archives in dependency order (from MAME's propmin link line).
ARCHIVES=(
	"$BIN/mame_propmin/libmame_propmin.a" "$BIN/libfrontend.a" "$BIN/mame_propmin/liboptional.a"
	"$BIN/libemu.a" "$BIN/libosd_sdl.a" "$BIN/libqtdbg_sdl.a" "$BIN/mame_propmin/libformats.a"
	"$BIN/mame_propmin/libdasm.a" "$BIN/libutils.a" "$BIN/libexpat.a" "$BIN/libsoftfloat.a"
	"$BIN/libsoftfloat3.a" "$BIN/libwdlfft.a" "$BIN/libymfm.a" "$BIN/libjpeg.a" "$BIN/lib7z.a"
	"$BIN/libasmjit.a" "$BIN/liblua.a" "$BIN/liblualibs.a" "$BIN/liblinenoise.a" "$BIN/libzlib.a"
	"$BIN/libzstd.a" "$BIN/libflac.a" "$BIN/libutf8proc.a" "$BIN/libsqlite3.a" "$BIN/libbgfx.a"
	"$BIN/libbimg.a" "$BIN/libbx.a" "$BIN/libocore_sdl.a"
)
FRAMEWORKS="-framework QuartzCore -framework OpenGL -framework IOKit -weak_framework Metal \
-framework CoreAudio -framework AudioToolbox -weak_framework CoreHaptics -weak_framework GameController \
-framework ForceFeedback -framework CoreVideo -framework Cocoa -framework Carbon -framework AudioUnit \
-framework CoreServices"

echo ">> linking $CONSOLE_OUT"
clang++ -arch arm64 -o "$CONSOLE_OUT" \
	"$HOST_BUILD_DIR/console_main.o" \
	"$HOST_BUILD_DIR/prophecy_engine.o" \
	"$OBJ/src/mame/mame.o" \
	"$OBJ/generated/mame/propmin/drivlist.o" \
	"$OBJ/generated/version.o" \
	"${ARCHIVES[@]}" \
	-L/opt/homebrew/lib -lSDL2 -lpthread -lm -lobjc $FRAMEWORKS

echo ">> linking $LIFECYCLE_OUT"
clang++ -arch arm64 -o "$LIFECYCLE_OUT" \
	"$HOST_BUILD_DIR/lifecycle_main.o" \
	"$HOST_BUILD_DIR/prophecy_engine.o" \
	"$OBJ/src/mame/mame.o" \
	"$OBJ/generated/mame/propmin/drivlist.o" \
	"$OBJ/generated/version.o" \
	"${ARCHIVES[@]}" \
	-L/opt/homebrew/lib -lSDL2 -lpthread -lm -lobjc $FRAMEWORKS

echo ">> done -> $CONSOLE_OUT, $LIFECYCLE_OUT"
