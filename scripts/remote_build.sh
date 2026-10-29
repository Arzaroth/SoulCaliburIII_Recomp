#!/usr/bin/env bash
# Builds ps2EntryRunner on a remote x86-64 Linux box over ssh and copies it back into build/game, where scripts/run.sh
# and scripts/regress.sh find it. The game still runs on this machine.
# Usage: scripts/remote_build.sh [Release|RelWithDebInfo|Debug]
#   REMOTE_HOST    ssh host (default stallman)
#   REMOTE_DIR     work directory on the host, relative to its home (default sc3-remote)
#   REMOTE_NAME    name of this tree's sources and build there (default: the repository directory name)
#   REMOTE_JOBS    build parallelism (default: all cores of the host)
#   REMOTE_CFLAGS  C and C++ flags (default -msse4.2)
#   REMOTE_TOOLS   mise tools the build runs with (default: latest cmake and ninja, Clang 23.1.4 from conda-forge)
#   REMOTE_CC, REMOTE_CXX
#                  compilers of a new build directory (default: that Clang, on the host's own GCC headers,
#                  libstdc++ and linker rather than the conda-forge ones)
#   REMOTE_OUT     local build directory the runner is copied into (default build/game)
# The host needs rsync, mise, and the X11 and OpenGL development headers. FFmpeg comes from this machine: its headers
# and libraries are copied to <REMOTE_DIR>/ffmpeg, so the runner links against the FFmpeg it runs with.
source "$(dirname "$0")/common.sh"
BUILD_TYPE="${1:-RelWithDebInfo}"
HOST="${REMOTE_HOST:-stallman}"
RDIR="${REMOTE_DIR:-sc3-remote}"
NAME="${REMOTE_NAME:-$(basename "$ROOT")}"
FLAGS="${REMOTE_CFLAGS:--msse4.2}"
TOOLS="${REMOTE_TOOLS:-cmake@latest ninja@latest conda:clang@23.1.4 conda:clangxx@23.1.4}"
OUT="${REMOTE_OUT:-$GAME_BUILD}"
[ -f "$GENERATED/register_functions.cpp" ] || { echo "no generated code, run scripts/recomp.sh first"; exit 1; }

FFMPEG_LIBS=(libavcodec libavformat libavutil libswresample libswscale)
stamp=$(pkg-config --modversion "${FFMPEG_LIBS[@]}" | tr '\n' ' ')
if [ "$(ssh "$HOST" cat "$RDIR/ffmpeg/.stamp" 2>/dev/null)" != "$stamp" ]; then
    echo "== FFmpeg $stamp to $HOST:$RDIR/ffmpeg"
    stage=$(mktemp -d)
    trap 'rm -rf "$stage"' EXIT
    mkdir -p "$stage/include" "$stage/lib/pkgconfig"
    for lib in "${FFMPEG_LIBS[@]}"; do
        cp -r "$(pkg-config --variable=includedir "$lib")/$lib" "$stage/include/"
        cp -P "$(pkg-config --variable=libdir "$lib")/$lib".so* "$stage/lib/"
        sed -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' -e 's|^exec_prefix=.*|exec_prefix=${prefix}|' \
            -e 's|^libdir=.*|libdir=${prefix}/lib|' -e 's|^includedir=.*|includedir=${prefix}/include|' \
            "$(pkg-config --variable=pcfiledir "$lib")/$lib.pc" > "$stage/lib/pkgconfig/$lib.pc"
    done
    printf '%s' "$stamp" > "$stage/.stamp"
    tar -C "$stage" -c . | ssh "$HOST" "rm -rf $RDIR/ffmpeg && mkdir -p $RDIR/ffmpeg && tar -C $RDIR/ffmpeg -x"
fi

echo "== sources to $HOST:$RDIR/$NAME/src"
ssh "$HOST" mkdir -p "$RDIR/$NAME/src"
rsync -aL --delete --exclude=.git --exclude='/PS2Recomp/build*/' \
    "$ROOT/CMakeLists.txt" "$ROOT/runtime" "$ROOT/generated" "$ROOT/PS2Recomp" "$HOST:$RDIR/$NAME/src/"

echo "== build on $HOST"
args=$(printf '%q ' "$RDIR" "$NAME" "$BUILD_TYPE" "$FLAGS" "$TOOLS" "${REMOTE_JOBS:-}" "${REMOTE_CC:-}" \
    "${REMOTE_CXX:-}")
ssh "$HOST" "bash -s -- $args" <<'EOF'
set -euo pipefail
export MISE_EXPERIMENTAL=1
ffmpeg=$HOME/$1/ffmpeg
work=$HOME/$1/$2
read -ra tools <<< "$5"
jobs=${6:-$(nproc)}
flags=$4
ldflags="-Wl,-rpath-link,$ffmpeg/lib -Wl,--allow-shlib-undefined"
cc=${7:-clang}
cxx=${8:-clang++}
if [ -z "$8" ]; then
    flags="--no-default-config --target=$(gcc -dumpmachine) --gcc-install-dir=$(dirname "$(gcc -print-libgcc-file-name)") $flags"
    ldflags="--ld-path=$(command -v ld) $ldflags"
fi
compiler=()
if [ ! -f "$work/build/CMakeCache.txt" ]; then
    compiler=(-DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx")
fi
export PKG_CONFIG_PATH=$ffmpeg/lib/pkgconfig
MISE_JOBS=1 mise install "${tools[@]}"
cmake=(mise exec "${tools[@]}" -- cmake)
"${cmake[@]}" -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE="$3" -DPS2X_BUILD_STUDIO=OFF \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$ldflags" -DCMAKE_SKIP_RPATH=ON \
    -DGLFW_BUILD_WAYLAND=OFF "${compiler[@]}"
"${cmake[@]}" --build "$work/build" --target ps2EntryRunner -j "$jobs"
EOF

echo "== runner to $OUT"
mkdir -p "$OUT/PS2Recomp/ps2xRuntime"
rsync -tz "$HOST:$RDIR/$NAME/build/PS2Recomp/ps2xRuntime/ps2EntryRunner" "$OUT/PS2Recomp/ps2xRuntime/ps2EntryRunner"
