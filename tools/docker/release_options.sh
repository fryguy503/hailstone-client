# Sourced by linux_build.sh and msvc_build.sh: release_options <source dir> sets the array `release` to the configure
# options every release build starts from (docs/updater.md "Making a release"). They come before the caller's own, so
# CMAKE_ARGS can still change any of them on purpose; what it does not name is reset here rather than kept from an
# earlier configure of the same build folder, where a cached value (HAILSTONE_UPDATE_KEY above all, read from
# update-key.pub only at a folder's first configure) would otherwise stay. HAILSTONE_SEPARATE_SYMBOLS keeps the
# binaries' debug information in the build folder's symbols/, out of the install (docs/hardening.md "Crash reports").
release_options() {
    local pub="$1/update-key.pub" key
    key=$(grep -m1 -E '^[0-9a-fA-F]{64}$' "$pub" 2>/dev/null || true)
    if [ -z "$key" ]; then
        echo "$pub: no release key (64 hex digits on a line of its own)" >&2
        exit 1
    fi
    release=(-DCMAKE_BUILD_TYPE=Release "-DHAILSTONE_UPDATE_KEY=$key" -DHAILSTONE_SANITIZE=OFF -DHAILSTONE_LIBFUZZER=OFF
             -DHAILSTONE_ALLOW_IN_EQ_FOLDER=OFF -DHAILSTONE_SEPARATE_SYMBOLS=ON -UHAILSTONE_UPDATE_FEED
             -UHAILSTONE_ADDON_INDEX -UHAILSTONE_PROJECT_PAGE -UHAILSTONE_NIGHTLY_LABEL)
}
