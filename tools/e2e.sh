#!/usr/bin/env bash
# e2e.sh — playwright helper that keeps every artifact inside the repo:
# browsers install into the gitignored .playwright/ (never /tmp or $HOME),
# and the test server uses the throwaway .e2e-config/ dir.
#
#   tools/e2e.sh setup                    one-time: install firefox
#   tools/e2e.sh test                     run the suite (build ./flower first)
#   tools/e2e.sh ff http://127.0.0.1:8080/ interactive inspector
#
# Anything else is passed to `bunx playwright` verbatim.
set -euo pipefail
cd "$(dirname "$0")/.."
export PLAYWRIGHT_BROWSERS_PATH="$PWD/.playwright"

if [ "${1:-}" = "setup" ]; then
  exec bunx playwright install firefox
fi
exec bunx playwright "$@"
