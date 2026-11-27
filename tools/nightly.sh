#!/bin/bash
# The nightly run (docs/ci.md "Nightly run"): what a normal run of hailstone_tests leaves out, on origin/main.
#   1. The sanitizer build (HAILSTONE_SANITIZE) and every hailstone_tests case, with the game folder and the recorded
#      vectors set, so the tests that otherwise pass without checking anything check something.
#   2. The libFuzzer targets (HAILSTONE_LIBFUZZER, Clang), each for --fuzz-seconds, side by side, on corpora kept
#      between nights. A crash's input is kept for the morning.
#   3. With a test server set (HAILSTONE_LIVE_*), the live smoke tests (tools/live_smoke.sh, tests/live/*.quest)
#      played by the sanitizer build's playprobe.
#   3b. The performance scenes (tools/perf_track.py): a Release client's frame times and memory in fixed scenes,
#      compared with the last nights in perf.csv; a rise is reported, not failed.
#   3c. The forced GPU fallbacks (HAILSTONE_GPU_FALLBACKS): the zone viewer under the validation layers once with none
#      and once with each; a validation error fails the night.
#   4. With --publish, after a night with no failure: the Linux and Windows packages, built as a release is
#      (tools/docker/build.sh, tools/make_update.py) with the version label nightly.<date>, the Linux package's tests
#      run on this machine and the Windows package started under Wine (it logs in to the test server), then
#      uploaded to the rolling `nightly` pre-release on GitHub. The updater never offers a pre-release, and the
#      packages carry no signed manifest.
#   5. A summary, printed and, with a webhook set, posted to Discord.
#
# Usage: tools/nightly.sh [--fuzz-seconds N] [--jobs N] [--no-fuzz] [--no-live] [--no-perf] [--no-gpu-fallbacks]
#                         [--no-post] [--package | --publish]
#                         [--ref REF]
#   --fuzz-seconds  seconds per fuzz target (default 1800; the targets run at once, one core each)
#   --jobs          build and test jobs, and the package containers' CPU limit (default 20)
#   --no-live       leave out the live smoke tests (and the logged-in performance scene) even with a test server set
#   --no-perf       leave out the performance scenes
#   --no-gpu-fallbacks  leave out the forced GPU fallbacks
#   --package       build, test and zip the packages into the checkout's release-nightly/, without uploading
#   --publish       build and publish the packages (needs gh signed in with write access, Docker, and MSVC for
#                   tools/docker/build.sh windows-x64)
#   --ref           what to test (default origin/main, fetched first)
#
# Settings, in ${XDG_CONFIG_HOME:-~/.config}/hailstone/nightly.env (sourced; never in the repository):
#   EQ_GAME_DIR=...             the game folder the asset tests read
#   HAILSTONE_VECTORS_DIR=...   the recorded vectors (*.jsonl)
#   HAILSTONE_NIGHTLY_WEBHOOK=  a Discord webhook URL for the summary (optional)
#   HAILSTONE_LIVE_SERVER=HOST[:PORT] HAILSTONE_LIVE_USER= HAILSTONE_LIVE_PASSWORD= HAILSTONE_LIVE_CHARACTER=
#                               a test server and a GM character for the live smoke tests (optional; all four)
#   HAILSTONE_LIVE_CASTER=      a GM cleric on the same account for the live smoke tests that cast (optional)
#
# Everything it writes is under ${XDG_CACHE_HOME:-~/.cache}/hailstone-nightly: the checkout it tests (a detached
# worktree of this repository), its build folders, the fuzz corpora and crash inputs, the performance history
# (perf.csv), a log folder per night, and the packages' symbol files, symbols/<version>/<platform>/ (never uploaded;
# docs/hardening.md "Crash reports").
set -uo pipefail

repo=$(cd "$(dirname "$0")/.." && git rev-parse --show-toplevel)
fuzz_seconds=1800
jobs=20
fuzz=1
live=1
perf=1
gpu_fallbacks=1
post=1
publish=0
ref=origin/main
while [ $# -gt 0 ]; do
  case "$1" in
    --fuzz-seconds) fuzz_seconds=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --no-fuzz) fuzz=0; shift ;;
    --no-live) live=0; shift ;;
    --no-perf) perf=0; shift ;;
    --no-gpu-fallbacks) gpu_fallbacks=0; shift ;;
    --no-post) post=0; shift ;;
    --package) publish=1; shift ;;
    --publish) publish=2; shift ;;
    --ref) ref=$2; shift 2 ;;
    *) echo "nightly: unknown option $1" >&2; exit 2 ;;
  esac
done

config=${XDG_CONFIG_HOME:-$HOME/.config}/hailstone/nightly.env
# shellcheck disable=SC1090
[ -f "$config" ] && . "$config"
# The tests and live_smoke.sh read these from the environment; the webhook and the test account stay unexported.
[ -n "${EQ_GAME_DIR:-}" ] && export EQ_GAME_DIR
[ -n "${HAILSTONE_VECTORS_DIR:-}" ] && export HAILSTONE_VECTORS_DIR

cache=${XDG_CACHE_HOME:-$HOME/.cache}/hailstone-nightly
checkout=$cache/checkout
night=$(date +%Y%m%d-%H%M%S)
logs=$cache/logs/$night
mkdir -p "$logs" "$cache/corpus" "$cache/crashes"

exec 9>"$cache/lock"
if ! flock -n 9; then
  echo "nightly: another run holds $cache/lock" >&2
  exit 1
fi

summary=()
# The tests run without the desktop's display: CEF's helper in the browser tests can fail to draw on a busy desktop
# session's, and headless Chromium is what they get elsewhere. The Windows package's start keeps the display.
headless() { env -u DISPLAY -u WAYLAND_DISPLAY "$@"; }
failed=0
note() { summary+=("$1"); echo "nightly: $1"; }
fail() { note "FAIL $1"; failed=1; }

# The checkout: a detached worktree at the ref, cleaned of everything but its build folders.
git -C "$repo" fetch -q origin || note "fetch failed; testing the last fetched $ref"
if [ ! -e "$checkout/.git" ]; then
  git -C "$repo" worktree add -q --detach "$checkout" "$ref" || { echo "nightly: cannot make $checkout" >&2; exit 1; }
fi
git -C "$checkout" checkout -q --detach -f "$ref" || { echo "nightly: cannot check out $ref" >&2; exit 1; }
git -C "$checkout" clean -q -ffdx -e /build-asan -e /build-fuzz -e /build-perf -e /build-linux-x64 \
  -e /build-windows-x64 -e /.docker -e /plugins/nms -e /plugins/projecttriune
# The server plugins' clones join the build when the main checkout has them (AGENTS.md, server plugins). Each night
# relinks them to where they really are, so a link made from a worktree since removed does not dangle.
for p in nms projecttriune; do
  if [ -d "$repo/plugins/$p" ]; then
    ln -sfn "$(cd "$repo/plugins/$p" && pwd -P)" "$checkout/plugins/$p"
  elif [ -L "$checkout/plugins/$p" ]; then
    rm "$checkout/plugins/$p"
  fi
done
commit=$(git -C "$checkout" rev-parse --short HEAD)
note "testing $ref at $commit"
[ -n "${EQ_GAME_DIR:-}" ] || note "EQ_GAME_DIR unset: the game folder tests check nothing"
[ -n "${HAILSTONE_VECTORS_DIR:-}" ] || note "HAILSTONE_VECTORS_DIR unset: the vector tests check nothing"

cd "$checkout" || exit 1

# 1. The sanitizer build and the tests.
if { [ -f build-asan/CMakeCache.txt ] ||
     cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAILSTONE_SANITIZE=ON; } \
     > "$logs/asan-configure.log" 2>&1 &&
   nice cmake --build build-asan -j "$jobs" > "$logs/asan-build.log" 2>&1; then
  if headless nice ctest --test-dir build-asan -j "$jobs" --output-on-failure > "$logs/asan-tests.log" 2>&1; then
    note "sanitizer tests: $(grep -o '[0-9]*% tests passed.*' "$logs/asan-tests.log" | tail -1)"
  else
    fail "sanitizer tests: $(grep -o '[0-9]*% tests passed.*' "$logs/asan-tests.log" | tail -1) (asan-tests.log)"
  fi
else
  fail "sanitizer build (asan-configure.log, asan-build.log)"
fi

# 2. The fuzz targets, side by side.
if [ "$fuzz" = 1 ]; then
  if { [ -f build-fuzz/CMakeCache.txt ] ||
       cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAILSTONE_LIBFUZZER=ON \
         -DHAILSTONE_BUILD_TESTS=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++; } \
       > "$logs/fuzz-configure.log" 2>&1; then
    targets=$(ninja -C build-fuzz -t targets all 2>/dev/null | grep -o '^fuzz_[a-z_]*:' | tr -d : | sort -u)
    # shellcheck disable=SC2086
    if nice cmake --build build-fuzz -j "$jobs" --target $targets > "$logs/fuzz-build.log" 2>&1; then
      pids=()
      for t in $targets; do
        mkdir -p "$cache/corpus/$t" "$cache/crashes/$t"
        nice build-fuzz/"$t" "$cache/corpus/$t" -max_total_time="$fuzz_seconds" -print_final_stats=1 \
          -artifact_prefix="$cache/crashes/$t/$night-" > "$logs/$t.log" 2>&1 &
        pids+=("$!:$t")
      done
      for entry in "${pids[@]}"; do
        pid=${entry%%:*}; t=${entry#*:}
        if wait "$pid"; then
          note "$t: $(grep -o 'stat::number_of_executed_units: *[0-9]*' "$logs/$t.log" | grep -o '[0-9]*$') runs, no crash"
        else
          fail "$t: crashed; its input is kept in crashes/$t ($t.log)"
        fi
      done
    else
      fail "fuzz build (fuzz-build.log)"
    fi
  else
    fail "fuzz configure (fuzz-configure.log)"
  fi
fi

# 3. The live smoke tests. The server's address and the account stay out of the summary, which may be posted.
if [ "$live" = 1 ] && [ -n "${HAILSTONE_LIVE_SERVER:-}" ]; then
  if [ -z "${HAILSTONE_LIVE_USER:-}" ] || [ -z "${HAILSTONE_LIVE_PASSWORD:-}" ] ||
     [ -z "${HAILSTONE_LIVE_CHARACTER:-}" ]; then
    fail "live smoke tests: HAILSTONE_LIVE_SERVER is set without a user, password and character"
  elif [ ! -x build-asan/playprobe ]; then
    fail "live smoke tests: no playprobe (the sanitizer build failed)"
  else
    PLAYPROBE=build-asan/playprobe LIVE_SMOKE_LOGS="$logs/live" LIVE_SMOKE_CASTER="${HAILSTONE_LIVE_CASTER:-}" \
      tools/live_smoke.sh "$HAILSTONE_LIVE_SERVER" \
      "$HAILSTONE_LIVE_USER" "$HAILSTONE_LIVE_PASSWORD" "$HAILSTONE_LIVE_CHARACTER" > "$logs/live.log" 2>&1
    passed=$(grep -c '^PASS ' "$logs/live.log")
    # A sanitizer report UBSan does not stop on counts as a failure too.
    failures=$( { grep -o '^FAIL [^:]*' "$logs/live.log" | sed 's/^FAIL //'
                  grep -l 'ERROR: [A-Za-z]*Sanitizer\|runtime error:' "$logs"/live/*.log 2>/dev/null |
                    xargs -r -n1 basename | sed 's/\.log$/ (sanitizer)/'; } | sort -u | paste -sd, - | sed 's/,/, /g')
    if [ -z "$failures" ] && [ "$passed" -gt 0 ]; then
      retried=$(grep -c '^PASS .*second try' "$logs/live.log")
      skipped=$(grep -c '^SKIP ' "$logs/live.log")
      note "live smoke tests: $passed passed$([ "$retried" -gt 0 ] && echo " ($retried at a second try: no world at first)")$([ "$skipped" -gt 0 ] && echo ", $skipped skipped (no HAILSTONE_LIVE_CASTER)")"
    else
      fail "live smoke tests: ${failures:-none ran} (live.log, live/)"
    fi
  fi
fi

# 3b. Performance (docs/ci.md "Nightly performance"): a Release client's --bench in the zone viewer and, with a test
# server, logged in; each scene against its last nights. A rise is reported, not failed: frame times are noisy.
if [ "$perf" = 1 ] && [ -n "${EQ_GAME_DIR:-}" ]; then
  if { [ -f build-perf/CMakeCache.txt ] || cmake -S . -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=Release; } \
       > "$logs/perf-configure.log" 2>&1 &&
     nice cmake --build build-perf -j "$jobs" --target hailstone > "$logs/perf-build.log" 2>&1; then
    perf_live=()
    [ "$live" = 1 ] && [ -n "${HAILSTONE_LIVE_SERVER:-}" ] && [ -n "${HAILSTONE_LIVE_CHARACTER:-}" ] &&
      perf_live=(--live "$HAILSTONE_LIVE_SERVER")
    python3 tools/perf_track.py --hailstone build-perf/hailstone/hailstone --csv "$cache/perf.csv" \
      --commit "$commit" --logs "$logs/perf" "${perf_live[@]}" > "$logs/perf.log" 2>&1
    rc=$?
    while IFS= read -r line; do note "$line"; done < <(grep -E '^(perf |REGRESSION )' "$logs/perf.log")
    [ "$rc" = 2 ] && fail "performance: no scene gave a result (perf.log, perf/)"
  else
    fail "performance build (perf-configure.log, perf-build.log)"
  fi
fi

# 3c. The forced GPU fallbacks (docs/ci.md "Nightly GPU fallbacks"): the paths a weaker GPU takes, drawn on this one
# under the validation layers by the Release viewer, first with none forced so a fallback's own errors stand out.
# A validation error, or a run that does not finish, fails the night; warnings are counted only.
if [ "$gpu_fallbacks" = 1 ] && [ -n "${EQ_GAME_DIR:-}" ]; then
  if { [ -f build-perf/CMakeCache.txt ] || cmake -S . -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=Release; } \
       > "$logs/fallbacks-configure.log" 2>&1 &&
     nice cmake --build build-perf -j "$jobs" --target hailstone > "$logs/fallbacks-build.log" 2>&1; then
    mkdir -p "$logs/fallbacks"
    drawn=0 warnings=0 bad=() unforced=()
    for word in none depth-stencil depth-d24 non-coherent shadow-d16 no-bc no-mip-blit no-update-after-bind \
                no-independent-blend; do
      log=$logs/fallbacks/$word.log
      forced=()
      [ "$word" != none ] && forced=(HAILSTONE_GPU_FALLBACKS="$word")
      headless env "${forced[@]}" SDL_VIDEO_DRIVER=offscreen timeout 300 build-perf/hailstone/hailstone \
        --zone qeytoqrg --time 12 --lighting modern --frames 300 --no-audio --validation \
        --game-dir "$EQ_GAME_DIR" > "$log" 2>&1
      rc=$?
      if grep -q 'validation requested but' "$log"; then
        note "GPU fallbacks: not checked (no Vulkan validation layer installed)"; bad=(); drawn=-1; break
      fi
      errors=$(grep -c '^\[error\] vulkan:' "$log")
      warnings=$((warnings + $(grep -c '^\[warn\] vulkan:' "$log")))
      grep -q 'cannot be forced' "$log" && unforced+=("$word")
      if [ "$rc" != 0 ] || [ "$errors" -gt 0 ]; then
        bad+=("$word ($([ "$rc" != 0 ] && echo "exit $rc, ")$errors validation errors)")
      else
        drawn=$((drawn + 1))
      fi
    done
    if [ "${#bad[@]}" -gt 0 ]; then
      fail "GPU fallbacks: $(printf '%s, ' "${bad[@]}" | sed 's/, $//') (fallbacks/)"
    elif [ "$drawn" -ge 0 ]; then
      note "GPU fallbacks: $drawn runs without a validation error, $warnings validation warnings$(
        [ "${#unforced[@]}" -gt 0 ] && echo "; not forceable here: ${unforced[*]}")"
    fi
  else
    fail "GPU fallbacks build (fallbacks-configure.log, fallbacks-build.log)"
  fi
fi

# 4. The packages, after a night with no failure.
published=""
package_build() {
  # Windows at no more than 8 jobs, build.sh's default and the release builds': at 20, MSVC under Wine failed most
  # large builds (below); at 8 a clean build passed.
  local cpus=$jobs
  [ "$1" = windows-x64 ] && [ "$cpus" -gt 8 ] && cpus=8
  CPUS=$cpus SYMBOLS_DIR="$cache/symbols" CMAKE_ARGS="-DHAILSTONE_BROWSER=ON -DHAILSTONE_NIGHTLY_LABEL=$2" \
    tools/docker/build.sh "$1" --no-test > "$3" 2>&1
}

# The Windows package started under Wine: it logs in to the live smoke tests' server, enters the world and stops
# after a minute of frames. A copy runs, so nothing it writes reaches the zip; its data folder is the night's logs.
# It needs Wine, the desktop's display (Vulkan presents to a window) and the test server; without them it is noted
# and left out.
windows_start() {
  if [ -z "${HAILSTONE_LIVE_SERVER:-}" ] || [ -z "${HAILSTONE_LIVE_USER:-}" ] ||
     [ -z "${HAILSTONE_LIVE_PASSWORD:-}" ] || [ -z "${HAILSTONE_LIVE_CHARACTER:-}" ] || [ -z "${EQ_GAME_DIR:-}" ]; then
    note "Windows package: not started (no test server or game folder set)"; return 0
  fi
  if ! command -v wine > /dev/null || ! command -v kwin_wayland > /dev/null; then
    note "Windows package: not started (no Wine or no kwin_wayland)"; return 0
  fi
  local run=$cache/windows-run host port=() rc
  rsync -a --delete dist/windows-x64/hailstone/ "$run/" || { fail "Windows package: copying it"; return 1; }
  mkdir -p "$logs/windows-data"
  host=${HAILSTONE_LIVE_SERVER%%:*}
  [ "$host" != "$HAILSTONE_LIVE_SERVER" ] && port=(--port "${HAILSTONE_LIVE_SERVER#*:}")
  export WINEPREFIX=$cache/wine-prefix WINEDEBUG=-all
  [ -d "$WINEPREFIX" ] || wineboot -i > /dev/null 2>&1
  # It runs on a virtual screen of its own (KWin's virtual backend, with Xwayland for Wine), not the desktop's: with
  # the monitor switched off for the night the desktop has no outputs, and SDL under Wine then finds no display.
  # KWin starts one program with no arguments, so the command goes in a script, which also keeps the exit code.
  {
    printf '#!/bin/bash\ncd %q || exit 1\n' "$run"
    printf 'HAILSTONE_DATA_DIR=%q wine hailstone.exe' "$(winepath -w "$logs/windows-data")"
    printf ' %q' --game-dir "$(winepath -w "$EQ_GAME_DIR")" --host "$host" "${port[@]}" \
      --user "$HAILSTONE_LIVE_USER" --password "$HAILSTONE_LIVE_PASSWORD" --character "$HAILSTONE_LIVE_CHARACTER" \
      --no-audio --max-fps 60 --frames 3600 --screenshot "$(winepath -w "$logs/windows.png")"
    printf '\necho $? > %q\n' "$cache/windows-start.rc"
  } > "$cache/windows-start.sh"
  chmod 700 "$cache/windows-start.sh"
  rm -f "$cache/windows-start.rc"
  # winepath above started a wineserver with the desktop's display; one still running when the game starts is joined,
  # and the game then looks for the desktop's display, not KWin's. Wait for it to end.
  wineserver -w
  env -u DISPLAY -u WAYLAND_DISPLAY timeout 300 kwin_wayland --virtual --xwayland --width 1280 --height 720 \
    --no-lockscreen --no-global-shortcuts --exit-with-session "$cache/windows-start.sh" > "$logs/windows.log" 2>&1
  rc=$?
  [ "$rc" = 0 ] && rc=$(cat "$cache/windows-start.rc" 2> /dev/null || echo "none")
  rm -f "$cache/windows-start.sh" "$cache/windows-start.rc"
  unset WINEPREFIX WINEDEBUG
  if [ "$rc" = 0 ] && grep -q 'chat: You have entered' "$logs/windows.log" && [ -s "$logs/windows.png" ]; then
    note "Windows package: started under Wine, logged in and entered the world"
  else
    fail "Windows package: did not reach the world under Wine (exit $rc; windows.log)"
    return 1
  fi
}

publish_packages() {
  local base label version notes slug full asset
  base=$(sed -n 's/^project(hailstone VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
  label="nightly.$(date +%Y%m%d)"
  version="$base-$label"
  # The packages as a release's (docs/updater.md "Making a release"), with tests run below rather than in the
  # containers: Linux's container cannot run the browser tests, Windows' Wine fails some (docs/ci.md).
  # A failed build is tried once more: MSVC under Wine now and then stops a large build with an internal compiler error
  # in its file reading (p0io.c) and a linker that cannot open an object, at a different file each time, and the same
  # build carried on passes. Ninja resumes where it stopped, so the second try is short.
  for platform in linux-x64 windows-x64; do
    if ! package_build "$platform" "$label" "$logs/package-$platform.log"; then
      package_build "$platform" "$label" "$logs/package-$platform-retry.log" ||
        { fail "the $platform package, twice (package-$platform.log, package-$platform-retry.log)"; return; }
      note "the $platform package passed at the second try (package-$platform.log)"
    fi
  done
  # The symbol files of nightlies older than 90 days go: nobody still runs those.
  find "$cache/symbols" -mindepth 1 -maxdepth 1 -type d -mtime +90 -exec rm -rf {} + 2>/dev/null
  # In 8 parts at once, as ctest runs them (HAILSTONE_TEST_SHARD, docs/ci.md "The tests"). The cases that start the
  # real CEF helper are left out: they time out on some desktops, the release builds' too (docs/ci.md "Nightly
  # builds").
  local k pids=() bad=0
  for k in 1 2 3 4 5 6 7 8; do
    HAILSTONE_TEST_SHARD=$k/8 headless nice build-linux-x64/hailstone_tests \
      "-tce=browser host: the CEF helper*,browser host: CEF's other processes*" \
      > "$logs/package-tests-$k.log" 2>&1 &
    pids+=("$!")
  done
  for k in "${pids[@]}"; do wait "$k" || bad=1; done
  cat "$logs"/package-tests-*.log > "$logs/package-tests.log"
  local cases
  # Each part's report counts the whole list, so the total is one part's and the failures are summed.
  cases=$(grep -ho 'test cases: *[0-9]* | *[0-9]* passed | *[0-9]* failed' "$logs"/package-tests-*.log |
          awk -F'|' '{gsub(/[^0-9]/,"",$1); gsub(/[^0-9]/,"",$3); n=$1; f+=$3} END {print n " cases, " f " failed"}')
  if [ "$bad" = 0 ]; then
    note "Linux package tests: $cases"
  else
    fail "Linux package tests: $cases (package-tests.log)"
    return
  fi
  windows_start || return
  rm -rf release-nightly
  for platform in linux-x64 windows-x64; do
    python3 tools/make_update.py --version "$version" --platform "$platform" --dist "dist/$platform/hailstone" \
      --out release-nightly >> "$logs/package-zip.log" 2>&1 || { fail "zipping $platform (package-zip.log)"; return; }
  done
  # No manifest: nothing should install a nightly as an update.
  rm -f release-nightly/hailstone-update.txt
  (cd release-nightly && sha256sum -- *.zip > SHA256SUMS)
  if [ "$publish" = 1 ]; then
    note "packaged $version, not uploaded"
    return
  fi

  slug=$(gh repo view --json nameWithOwner --jq .nameWithOwner) || { fail "gh cannot see the repository"; return; }
  full=$(git rev-parse HEAD)
  notes=$logs/release-notes.md
  {
    echo "Built from $full on $(date -u +%Y-%m-%d) after the night's checks passed. Untested in live play."
    echo
    echo "A pre-release: the updater never offers it, and a nightly is offered the next release as usual."
    echo "The version is $version; \`SHA256SUMS\` has the downloads' checksums."
    echo
    echo "The night's checks:"
    printf -- '- %s\n' "${summary[@]}"
  } > "$notes"
  if gh release view nightly > /dev/null 2>&1; then
    # The one ref this moves, and never a branch: a plain git fetch leaves a clone's old copy of the tag alone.
    gh api -X PATCH "repos/$slug/git/refs/tags/nightly" -f sha="$full" -F force=true > /dev/null ||
      { fail "moving the nightly tag"; return; }
  else
    gh release create nightly --prerelease --title "Nightly" --target "$full" --notes-file "$notes" > /dev/null ||
      { fail "creating the nightly release"; return; }
  fi
  gh release upload nightly release-nightly/* --clobber > /dev/null || { fail "uploading the packages"; return; }
  # The previous night's downloads, now replaced (their names carry the date).
  for asset in $(gh release view nightly --json assets --jq '.assets[].name'); do
    [ -e "release-nightly/$asset" ] || gh release delete-asset nightly "$asset" -y > /dev/null
  done
  gh release edit nightly --prerelease --notes-file "$notes" > /dev/null
  published="https://github.com/$slug/releases/tag/nightly"
  note "published $version"
}
if [ "$publish" != 0 ]; then
  if [ "$failed" = 0 ]; then publish_packages; else note "not published: the night had a failure"; fi
fi

# 5. The summary.
status="passed"
[ "$failed" = 1 ] && status="FAILED"
# The posted text names no local path (it would carry the user's name); the log folder is added locally only.
text="Hailstone nightly $status on $commit"$'\n'"$(printf -- '- %s\n' "${summary[@]}")"
[ -n "$published" ] && text+=$'\n'"Downloads: $published"
printf '%s\n' "$text" > "$logs/summary.txt"
printf '%s\nLogs: %s\n' "$text" "$logs"
if [ "$post" = 1 ] && [ -n "${HAILSTONE_NIGHTLY_WEBHOOK:-}" ]; then
  python3 -c 'import json,sys; print(json.dumps({"content": sys.stdin.read()[:1900]}))' < "$logs/summary.txt" |
    curl -fsS -H 'Content-Type: application/json' --data-binary @- "$HAILSTONE_NIGHTLY_WEBHOOK" > /dev/null ||
    echo "nightly: posting the summary failed" >&2
fi
exit "$failed"
