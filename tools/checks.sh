#!/bin/sh
# The repository's checks, in the groups CI runs as jobs (.github/workflows/ci.yml, docs/ci.md). They read the
# repository and build nothing. A check added here runs in CI and in tools/premerge.sh alike.
#
# Usage: tools/checks.sh [group...]     with no group, all of them
# Groups: core-names layers wiki ui-api
set -eu

cd "$(dirname "$0")/.."

run() {
  printf '== %s\n' "$*"
  "$@"
}

core_names() {
  run python3 tools/check_core_names.py
}

layers() {
  run python3 tools/check_layers.py
  run python3 tools/check_path_strings.py
  run python3 -m unittest discover -s tests -p 'test_debt_checks.py'
}

wiki() {
  run python3 tools/sync_wiki.py --check
  run python3 tools/check_unknowns.py
  run python3 -m unittest discover -s tests -p 'test_check_unknowns.py'
}

ui_api() {
  run python3 tools/migrate_ui_api.py --check
  run python3 tools/migrate_ui_api.py --mock-adapter --check
  run python3 tools/gen_ui_api_docs.py --check
  run python3 -m unittest discover -s tests -p 'test_migrate_ui_api.py'
  run python3 -m unittest discover -s tests -p 'test_gen_ui_api_docs.py'
  run python3 -m unittest discover -s tests -p 'test_make_addon_ui.py'
  run python3 -m unittest discover -s tests -p 'test_make_addon_autoload.py'
  run python3 -m unittest discover -s tests -p 'test_release_tools.py'
  run python3 -m unittest discover -s tests -p 'test_make_update.py'
  run python3 -m unittest discover -s tests -p 'test_perf_track.py'
  run python3 -m unittest discover -s tests -p 'test_symbolize_crash.py'
  run python3 -m unittest discover -s tests -p 'test_live_smoke.py'
}

[ $# -eq 0 ] && set -- core-names layers wiki ui-api
for group in "$@"; do
  case "$group" in
    core-names) core_names ;;
    layers) layers ;;
    wiki) wiki ;;
    ui-api) ui_api ;;
    *) echo "checks.sh: no group '$group' (core-names layers wiki ui-api)" >&2; exit 2 ;;
  esac
done
