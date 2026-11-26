#!/usr/bin/env bash
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
bash "$MODULE/scripts/build.sh" native
OUT="$MODULE/build/native"
"${CC:-gcc}" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  "$MODULE/tests/test_engine.c" "$MODULE/dsp/dirt_engine.c" -o "$OUT/test-engine" -lm
"$OUT/test-engine"
python3 "$MODULE/tests/smoke.py" "$OUT/dirt-delay.so"
if command -v node >/dev/null 2>&1; then
  node "$MODULE/tests/test_ui.mjs"
elif command -v node.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
  SCHWUNG_ROOT="$(wslpath -w "${SCHWUNG_ROOT:-$MODULE/../schwung}")" WSLENV=SCHWUNG_ROOT node.exe "$(wslpath -w "$MODULE/tests/test_ui.mjs")"
else
  echo "Node.js is required for the UI contract test" >&2; exit 1
fi
