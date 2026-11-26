#!/bin/bash
# Configures and builds a Linux Release inside the Linux image, with ccache (compose.yaml).
#   linux_build.sh <build dir> [cmake configure args ...]
# JOBS is the parallel jobs (default 8; tools/docker/build.sh sets it to the container's CPU limit).
# Every run configures with the release's settings (release_options.sh) before the arguments given, so an option an
# earlier run of the same build folder set (a test key, the sanitizer) never stays in a release.
set -euo pipefail
build=$1
shift
. "$(dirname "$0")/release_options.sh"
release_options .
if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S . -B "$build" -G Ninja -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        "${release[@]}" "$@"
else
    # An existing build folder takes the options given now; generator and compilers stay as configured.
    cmake -B "$build" "${release[@]}" "$@"
fi
cmake --build "$build" -j "${JOBS:-8}"
ccache -s | sed -n '1,6p'
