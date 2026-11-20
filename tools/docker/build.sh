#!/bin/bash
# Builds, tests and installs the client for one platform, or all, in containers (compose.yaml, docs/ci.md
# "Builds in containers").
#   tools/docker/build.sh linux-x64|linux-arm64|windows-x64|all [--no-test]
# The install lands in dist/<platform>/hailstone, ready for tools/make_update.py --dist (docs/updater.md
# "Making a release"); the build folder is build-<platform>/.
#   CPUS        the containers' CPU limit (default 8); JOBS, the parallel jobs, follows it unless set
#   CMAKE_ARGS  extra configure options, split at whitespace (an option cannot hold a space)
#   MSVC_DIR    msvc-wine's install (default ~/opt/msvc when it has bin/x64/cl, else .docker/msvc); with no install
#               there, windows-x64 downloads one only when MSVC_ACCEPT_LICENSE=1
set -euo pipefail
cd "$(dirname "$0")/../.."
usage="usage: tools/docker/build.sh linux-x64|linux-arm64|windows-x64|all [--no-test]"
what=${1:?$usage}
test=1
if [ "${2:-}" = --no-test ]; then test=0; elif [ -n "${2:-}" ]; then echo "$usage"; exit 1; fi

SRC_DIR=$(pwd -P)
GIT_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd -P)
CPUS=${CPUS:-8}
JOBS=${JOBS:-$CPUS}
# The containers run as the checkout's owner, also when root starts this: Chromium (the browser tests) refuses to run
# as root, and nothing in the checkout should end up owned by root.
HOST_UID=${HOST_UID:-$(stat -c %u .)}
HOST_GID=${HOST_GID:-$(stat -c %g .)}
if [ -z "${MSVC_DIR:-}" ]; then
    if [ -x "${HOME:-}/opt/msvc/bin/x64/cl" ]; then MSVC_DIR=$HOME/opt/msvc; else MSVC_DIR=$SRC_DIR/.docker/msvc; fi
fi
export SRC_DIR GIT_COMMON_DIR CPUS JOBS HOST_UID HOST_GID MSVC_DIR
if [ "$HOST_UID" = 0 ]; then echo "warning: the checkout belongs to root, so the browser tests will fail (Chromium refuses root)"; fi
mkdir -p .docker/ccache/linux-x64 .docker/ccache/linux-arm64 .docker/wine-home dist
# Started by root, the folders it just made are still root's: hand them to the containers' user (Wine refuses a home
# folder it does not own).
if [ "$(id -u)" = 0 ]; then chown "$HOST_UID:$HOST_GID" .docker .docker/ccache .docker/ccache/* .docker/wine-home dist; fi
# Whitespace-split on purpose; ${cmake_args[@]+...} below keeps an empty array from tripping set -u in old bash.
read -ra cmake_args <<< "${CMAKE_ARGS:-}"

dc() { docker compose "$@"; }
docker compose version >/dev/null 2>&1 || { echo "needs Docker Compose v2 (docker compose)"; exit 1; }

install_tree() { # <platform> <service>
    rm -rf "dist/$1"
    dc run --rm --no-deps "$2" cmake --install "build-$1" --prefix "dist/$1/hailstone"
    echo "installed dist/$1/hailstone"
}

linux() { # <platform>
    if [ "$1" = linux-arm64 ] && [ "$(uname -m)" != aarch64 ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
        # Registering QEMU changes the host's kernel settings (until the next reboot), so it is left to you.
        echo "linux-arm64 on this host needs QEMU registered for arm64; once per boot, run:"
        echo "  docker run --privileged --rm tonistiigi/binfmt:qemu-v10.2.3@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0 --install arm64"
        exit 1
    fi
    dc build "$1"
    dc run --rm --no-deps "$1" tools/docker/linux_build.sh "build-$1" ${cmake_args[@]+"${cmake_args[@]}"}
    if [ $test = 1 ]; then dc run --rm --no-deps -e EQ_GAME_DIR= "$1-test" "build-$1/hailstone_tests"; fi
    install_tree "$1" "$1"
}

windows() {
    dc build linux-x64
    dc build windows-x64
    if [ ! -x "$MSVC_DIR/bin/x64/cl" ]; then
        if [ "${MSVC_ACCEPT_LICENSE:-}" != 1 ]; then
            echo "no MSVC in $MSVC_DIR: set MSVC_DIR to an install, or MSVC_ACCEPT_LICENSE=1 to accept Microsoft's"
            echo "license and download one there (about 3.3 GB)"
            exit 1
        fi
        mkdir -p "$MSVC_DIR"
        dc --profile setup run --rm --no-deps msvc-setup
    fi
    dc run --rm --no-deps windows-x64 tools/docker/msvc_build.sh . build-windows-x64 ${cmake_args[@]+"${cmake_args[@]}"}
    if [ $test = 1 ]; then
        # Microsoft's runtime DLLs next to the tests: Wine's own msvcp140_atomic_wait lacks __std_calloc_crt and the
        # run crashes.
        cp -f "$MSVC_DIR"/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT/*.dll build-windows-x64/
        # Left out here because they fail only under Wine; this script does not run them, so run them on Windows by
        # hand (docs/ci.md "Builds in containers"): the browser tests (Chromium does not run under Wine), "cancelled
        # ends a transfer stalled..." (Wine's WinHTTP never ends the call), allow_url (Wine's WinHTTP reports another
        # address to the check), and "File time" and the story window's links (MSVC's clock_cast from file times needs
        # Windows' icu.dll).
        dc run --rm --no-deps -e EQ_GAME_DIR= \
            -e WINEDLLOVERRIDES="msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait,msvcp140_codecvt_ids,vcruntime140,vcruntime140_1,concrt140=n,b" \
            windows-x64 wine build-windows-x64/hailstone_tests.exe '-sfe=*browser_host_test*' \
            '-tce=*stalled?waiting?for?the?answer*,*allow_url?refuses?an?empty*,File?time:*,Story?window:?a?page?s?href*'
    fi
    install_tree windows-x64 windows-x64
    # The default Windows install also writes zlib's bin/, include/ and lib/ (docs/updater.md "Making a release").
    rm -rf dist/windows-x64/hailstone/bin dist/windows-x64/hailstone/include dist/windows-x64/hailstone/lib
}

case $what in
    linux-x64|linux-arm64) linux "$what" ;;
    windows-x64) windows ;;
    all) linux linux-x64; windows; linux linux-arm64 ;;
    *) echo "$usage"; exit 1 ;;
esac
