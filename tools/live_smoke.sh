#!/bin/bash
# Live smoke tests (docs/ci.md "Live smoke tests"): each tests/live/*.quest script played by playprobe's quest runner
# (tools/quest_probe.h) with a GM character on a test server, and a summary. Exits 1 when any script fails.
#
# Usage: tools/live_smoke.sh HOST[:PORT] USER PASS CHARACTER [script.quest ...]
#   With no scripts, every tests/live/*.quest. EQ_GAME_DIR must be set (playprobe reads the zones' collision).
#   PLAYPROBE: the playprobe to run (default build/playprobe). The logs go to LIVE_SMOKE_LOGS (default
#   build/live-smoke/<time>).
#
# Never commit a server's address, an account or a password: pass them here.
set -uo pipefail

if [ $# -lt 4 ]; then
  sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' >&2
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
for script in "$@"; do
  name=$(basename "$script" .quest)
  timeout 600 "$probe" "$host" "$user" "$pass" --char "$char" --quest "$script" > "$logs/$name.log" 2>&1
  rc=$?
  result=$(grep -o 'QUEST SUMMARY: .*' "$logs/$name.log" | tail -1)
  if [ "$rc" = 0 ]; then
    echo "PASS $name: ${result#QUEST SUMMARY: }"
  else
    failed=1
    echo "FAIL $name: ${result:-no summary (exit $rc)} ($logs/$name.log)"
    grep 'QUEST FAIL' "$logs/$name.log" | sed 's/^/  /'
  fi
done
exit "$failed"
