#!/bin/bash
# Live smoke tests (docs/ci.md "Live smoke tests"): each tests/live/*.quest script played by playprobe's quest runner
# (tools/quest_probe.h) with a GM character on a test server, and a summary. Exits 1 when any script fails.
#
# Usage: tools/live_smoke.sh HOST[:PORT] USER PASS CHARACTER [script.quest ...]
#   With no scripts, every tests/live/*.quest. EQ_GAME_DIR must be set (playprobe reads the zones' collision).
#   PLAYPROBE: the playprobe to run (default build/playprobe). The logs go to LIVE_SMOKE_LOGS (default
#   build/live-smoke/<time>). A script with a "# live-smoke: caster" line is played as LIVE_SMOKE_CASTER instead, a
#   GM cleric on the same account, and skipped (a SKIP line, not a failure) when that is unset. LIVE_SMOKE_PAUSE: the
#   seconds between scripts (default 20).
#
# Never commit a server's address, an account or a password: pass them here.
set -uo pipefail

if [ $# -lt 4 ]; then
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi
host=$1 user=$2 pass=$3 char=$4
shift 4

root=$(cd "$(dirname "$0")/.." && pwd)
probe=${PLAYPROBE:-$root/build/playprobe}
logs=${LIVE_SMOKE_LOGS:-$root/build/live-smoke/$(date +%Y%m%d-%H%M%S)}
[ -x "$probe" ] || { echo "live_smoke: no playprobe at $probe (cmake --build build --target playprobe)" >&2; exit 2; }
[ -n "${EQ_GAME_DIR:-}" ] || { echo "live_smoke: set EQ_GAME_DIR to the game folder" >&2; exit 2; }
[ $# -gt 0 ] || set -- "$root"/tests/live/*.quest
mkdir -p "$logs"

failed=0
played=0
for script in "$@"; do
  name=$(basename "$script" .quest)
  who=$char
  if grep -q '^# live-smoke: caster' "$script"; then
    if [ -z "${LIVE_SMOKE_CASTER:-}" ]; then
      echo "SKIP $name: needs a caster character (LIVE_SMOKE_CASTER)"
      continue
    fi
    who=$LIVE_SMOKE_CASTER
  fi
  # A pause between scripts, so the last script's zone has shut down and saved before the next login (the zone-ins
  # once closed after a camp were another login of the account: docs/todo.md "A login right after camping out").
  [ "$played" -gt 0 ] && sleep "${LIVE_SMOKE_PAUSE:-20}"
  played=$((played + 1))
  timeout 600 "$probe" "$host" "$user" "$pass" --char "$who" --seconds 480 --quest "$script" > "$logs/$name.log" 2>&1
  rc=$?
  result=$(grep -o 'QUEST SUMMARY: .*' "$logs/$name.log" | tail -1)
  again=""
  # A script that ran no step never reached the world: the server refused the login or the zone ("That zone is not
  # available" right after the last script's logout, while its zone shuts down). Once more after a pause.
  if [ "$rc" != 0 ] && { [ -z "$result" ] || [[ "$result" == *" 0 of "* ]]; }; then
    mv "$logs/$name.log" "$logs/$name-first.log"
    sleep 30
    timeout 600 "$probe" "$host" "$user" "$pass" --char "$who" --seconds 480 --quest "$script" > "$logs/$name.log" 2>&1
    rc=$?
    result=$(grep -o 'QUEST SUMMARY: .*' "$logs/$name.log" | tail -1)
    again=" (second try: the first never reached the world, $name-first.log)"
  fi
  if [ "$rc" = 0 ]; then
    echo "PASS $name: ${result#QUEST SUMMARY: }$again"
  else
    failed=1
    echo "FAIL $name: ${result:-no summary (exit $rc)} ($logs/$name.log)$again"
    grep 'QUEST FAIL' "$logs/$name.log" | sed 's/^/  /'
  fi
done
exit "$failed"
