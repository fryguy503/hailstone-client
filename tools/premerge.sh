#!/bin/sh
# What a merge to main needs on Linux (AGENTS.md "Build and test", docs/ci.md "Before a merge"): the build, every
# hailstone_tests case and every CI check (tools/checks.sh), then the commit they ran on.
#
# Usage: tools/premerge.sh [--build-dir DIR] [--jobs N]
#   --build-dir  the build directory, configured here if it is not yet (default: build)
#   --jobs       parallel compile and test jobs (default: the CPU count, at most 20; HAILSTONE_JOBS too)
#
# It does not cover the MSVC build under Wine, the sanitizer build or a live session: docs/ci.md says when those
# are needed.
set -eu

cd "$(dirname "$0")/.."

build_dir=build
jobs=${HAILSTONE_JOBS:-}
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) build_dir=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    *) echo "premerge: unknown option $1" >&2; exit 2 ;;
  esac
done
if [ -z "$jobs" ]; then
  jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
  [ "$jobs" -gt 20 ] && jobs=20
fi

commit=$(git rev-parse HEAD)
dirty=$(git status --porcelain --untracked-files=no)

step() {
  printf '\n== premerge: %s\n' "$*"
}

step "checks"
tools/checks.sh

step "build ($build_dir, $jobs jobs)"
if [ ! -f "$build_dir/CMakeCache.txt" ]; then
  cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
cmake --build "$build_dir" -j "$jobs"

step "hailstone_tests"
ctest --test-dir "$build_dir" -j "$jobs" --output-on-failure

step "passed"
if [ -n "$dirty" ]; then
  echo "premerge: passed on $commit plus uncommitted changes: commit them and run it again before merging"
else
  echo "premerge: passed on $commit"
fi
[ -n "${EQ_GAME_DIR:-}" ] || echo "premerge: EQ_GAME_DIR unset: the tests that read the game folder checked nothing"
[ -n "${HAILSTONE_VECTORS_DIR:-}" ] || echo "premerge: HAILSTONE_VECTORS_DIR unset: the recorded-vector tests checked nothing"
[ -z "$dirty" ]
