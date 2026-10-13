#!/bin/sh
# usage: tests/run.sh path/to/exbin   (runs from the repository root)
BIN="${1:-build/exbin}"
cd "$(dirname "$0")/.." || exit 1
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN" ;; esac
export EXBIN_PATH="$PWD/lib"
fail=0
for t in tests/*.exb; do
  out=$(timeout 300 "$BIN" "$t" 2>&1)
  status=$?
  if [ $status -eq 0 ]; then
    printf 'PASS  %-24s %s\n' "$t" "$(printf '%s' "$out" | tail -1)"
  else
    printf 'FAIL  %s (exit %d)\n%s\n' "$t" "$status" "$out"
    fail=1
  fi
done
exit $fail
