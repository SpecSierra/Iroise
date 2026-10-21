#!/bin/bash
# Build Ladybird with the Qt-free SFOS front end, natively on the aarch64 host.
# Usage: scripts/build.sh [ninja target]   (default: ladybird)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UP="$ROOT/upstream"
JOBS="${JOBS:-12}"

# Apply our patches once; the front end itself lives in this repo and is linked in.
# A patch that is neither applied nor cleanly applicable means upstream/ was edited by hand: regenerate that patch
# from the tree (git -C upstream diff -- <its files>) rather than letting patch half-apply it.
# NB: Once the whole series is in, upstream/.pacific-patches says which. A patch that later ones change where it
#     changed things (the touch patches) can no longer be undone alone, so the check below only works on its way in.
series="$(cat "$ROOT"/patches/[0-9]*.patch | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$UP/.pacific-patches" 2>/dev/null)" != "$series" ]; then
    for p in "$ROOT"/patches/[0-9]*.patch; do
        if patch -d "$UP" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
            continue
        elif patch -d "$UP" -p1 --dry-run -s -f < "$p" >/dev/null 2>&1; then
            patch -d "$UP" -p1 -s -f < "$p"
        else
            echo "$(basename "$p") does not match upstream/" >&2
            exit 1
        fi
    done
    echo "$series" > "$UP/.pacific-patches"
fi
ln -sfn "$ROOT/ui/SFOS" "$UP/UI/SFOS"
ln -sfn "$ROOT/ui/protocol" "$UP/UI/protocol"

# CMake >= 3.30 comes from the venv; gcc 14 from the Ubuntu archive.
export PATH="$ROOT/.venv/bin:$PATH"
export LADYBIRD_UI_SFOS=1

# Meta/ladybird.py refuses to configure as root, and this host only has root, so
# run the same three steps it would: vcpkg bootstrap, cmake preset, ninja.
export LADYBIRD_SOURCE_DIR="$UP" LADYBIRD_MAIN_SOURCE_DIR="$UP"
export VCPKG_ROOT="$UP/Build/vcpkg-release"
export PATH="$PATH:$VCPKG_ROOT"
export VCPKG_FORCE_SYSTEM_BINARIES=1 VCPKG_MAX_CONCURRENCY="$JOBS"
BUILD="$UP/Build/release"

# Profile-guided optimisation (scripts/pgo-profile.sh makes the profile):
#   PACIFIC_PGO=generate  an instrumented engine, in a build directory of its own, that writes its profile to
#                         PACIFIC_PGO_DIR as it runs;
#   PACIFIC_PGO=use       the engine optimised with the profile in PACIFIC_PGO_DIR.
# Profiles are named after the object files relative to the build directory, so the two directories can differ.
# A profile older than the code is fine: gcc leaves out what no longer matches, and optimises what the training
# never ran as it would without one.
PGO="${PACIFIC_PGO:-}"
PGO_FLAGS=""
# NB: gcc compares the prefix with the object files' real paths; upstream/ is a symlink on CI.
case "$PGO" in
    "") ;;
    generate)
        BUILD="$UP/Build/pgo-generate"
        PGO_FLAGS="-fprofile-generate=${PACIFIC_PGO_DIR:?} -fprofile-prefix-path=$(realpath -m "$BUILD") -fprofile-update=prefer-atomic" ;;
    use)
        # NB: The profile's own stamp is in the flags so that ccache, which does not follow -fprofile-prefix-path to
        #     the profile, compiles again when the profile changes.
        PGO_FLAGS="-fprofile-use=${PACIFIC_PGO_DIR:?} -fprofile-prefix-path=$(realpath -m "$BUILD") -fprofile-partial-training"
        PGO_FLAGS="$PGO_FLAGS -Wno-missing-profile -Wno-coverage-mismatch -DPACIFIC_PGO_PROFILE=$(cat "$PACIFIC_PGO_DIR/stamp")" ;;
    *) echo "PACIFIC_PGO must be generate or use, not $PGO" >&2; exit 1 ;;
esac

cd "$UP"
python3 Meta/ladybird.py vcpkg --preset Release -j "$JOBS"
# Skia's GL backend would link the desktop GL; ours gets its entry points from GLESContext (patch 0009). vcpkg picks
# up an overlay of its own skia port with that one change.
OVERLAY="$UP/Meta/CMake/vcpkg/overlay-ports/skia"
rm -rf "$OVERLAY"
cp -r "$VCPKG_ROOT/ports/skia" "$OVERLAY"
patch -d "$OVERLAY" -p1 -s < "$ROOT/patches/vcpkg-skia-gl-without-system-libraries.patch"
# mimalloc compiles itself for ARMv8.1 (its MI_OPT_ARCH) on arm64, which the Xperia 10 II's cores do not have.
OVERLAY="$UP/Meta/CMake/vcpkg/overlay-ports/mimalloc"
rm -rf "$OVERLAY"
# NB: An overlay replaces the version Ladybird pins, so it must be that version's port (2.2.7, this git tree from
#     versions/m-/mimalloc.json), not the newer one in ports/.
mkdir -p "$OVERLAY"
git -C "$VCPKG_ROOT" archive 5b9c09b6d1592392634727afb0abbe0b14e030a2 | tar -x -C "$OVERLAY"
sed -i 's/^\( *\)-DMI_USE_CXX=ON\(\r\?\)$/\1-DMI_USE_CXX=ON\2\n\1-DMI_NO_OPT_ARCH=ON\2/' "$OVERLAY/portfile.cmake"
grep -q MI_NO_OPT_ARCH "$OVERLAY/portfile.cmake"
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake --preset Release -S "$UP" -B "$BUILD" \
        -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
        -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" \
        -DBUILD_TESTING=ON
fi
# The tests' targets (test-web, for scripts/test-engine.sh); the default target builds none of them.
grep -q '^BUILD_TESTING:BOOL=ON' "$BUILD/CMakeCache.txt" || cmake -B "$BUILD" -DBUILD_TESTING=ON
# Plain ARMv8.0, not the build host's CPU: the Xperia 10 II's Cortex-A73/A53 have none of the host's ARMv8.2 extras.
grep -q '^ENABLE_ARMV8_BASELINE_CPU:BOOL=ON' "$BUILD/CMakeCache.txt" || cmake -B "$BUILD" -DENABLE_ARMV8_BASELINE_CPU=ON
# The profile flags go to every compile and link (the link too, for link-time optimisation and gcc's profile
# runtime). Configured again only when they change, which builds everything again.
pgo_generate=OFF
[ "$PGO" = generate ] && pgo_generate=ON
launcher="$(command -v ccache)"
[ "$PGO" = use ] && launcher="$ROOT/scripts/pgo-compile.sh"
cached() { grep -qxF -e "$1:STRING=$2" -e "$1:FILEPATH=$2" "$BUILD/CMakeCache.txt"; }
pgo_settings=()
for variable in CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS; do
    cached "$variable" "$PGO_FLAGS" || pgo_settings+=("-D$variable=$PGO_FLAGS")
done
for variable in CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER; do
    cached "$variable" "$launcher" || pgo_settings+=("-D$variable=$launcher")
done
grep -qxF "PACIFIC_PGO_GENERATE:BOOL=$pgo_generate" "$BUILD/CMakeCache.txt" || pgo_settings+=("-DPACIFIC_PGO_GENERATE=$pgo_generate")
[ ${#pgo_settings[@]} -eq 0 ] || cmake -B "$BUILD" "${pgo_settings[@]}"
exec ninja -C "$BUILD" -j "$JOBS" "${1:-ladybird}"
