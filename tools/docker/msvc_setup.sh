#!/bin/bash
# Downloads MSVC 2022 and the Windows SDK into MSVC_DIR and installs msvc-wine's wrappers there (compose.yaml
# msvc-setup). tools/docker/build.sh runs it only with MSVC_ACCEPT_LICENSE=1, which accepts Microsoft's license.
# msvc-wine is pinned: the commit the install in docs/ci.md "MSVC under Wine" was made with. Change it on purpose.
# So are the toolset and the SDK, to those of the install the releases are built with (MSVC 14.44.35207, Visual
# Studio 17.14; Windows SDK 10.0.26100.0): without them vsdownload.py takes whatever Microsoft's manifest names
# newest. The manifest still chooses the servicing build within those (docs/updater.md "Making a release"), so the
# toolset installed is checked against the release one afterwards.
set -euo pipefail
msvc_wine_commit=514f8ea34842cd6d831804d0e9658d3a32870ae1
msvc_version=17.14
msvc_toolset=14.44.35207
sdk_version=10.0.26100
d=${MSVC_DIR:?set MSVC_DIR}
src=.docker/msvc-wine-src
if [ -x "$d/bin/x64/cl" ]; then echo "MSVC is already installed in $d"; exit 0; fi
[ -d "$src/.git" ] || git clone -q https://github.com/mstorsjo/msvc-wine "$src"
git -C "$src" checkout -q "$msvc_wine_commit"
python3 "$src/vsdownload.py" --major 17 --msvc-version "$msvc_version" --sdk-version "$sdk_version" --accept-license \
    --architecture x64 --with-asan no --with-atl no --dest "$d"
"$src/install.sh" "$d"
if [ ! -d "$d/VC/Tools/MSVC/$msvc_toolset" ]; then
    echo "warning: installed MSVC $(ls "$d/VC/Tools/MSVC" | tr '\n' ' '), not $msvc_toolset, the toolset releases are built with"
fi
