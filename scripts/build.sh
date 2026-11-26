#!/usr/bin/env bash
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
API="$MODULE/third_party/schwung"  # Schwung API headers (MIT, see its LICENSE)
case "${1:-arm64}" in
  native) CC="${CC:-gcc}"; OUT="$MODULE/build/native" ;;
  arm64) CC="${CROSS_PREFIX:-aarch64-linux-gnu-}gcc"; OUT="$MODULE/dist/dirt-delay" ;;
  *) echo "Usage: $0 [native|arm64]" >&2; exit 2 ;;
esac
mkdir -p "$OUT" "$MODULE/build/generated"
python3 "$MODULE/scripts/metadata.py" "$MODULE/build/generated/metadata.h"
# The chain host loads audio_fx/<id>/<id>.so, never dsp.so (docs/MODULES.md).
# Do NOT use -ffast-math: the finite guards must survive.
"$CC" -std=c11 -O3 -fno-math-errno -Wall -Wextra -Werror -fPIC -shared -Wl,-z,defs \
  -I"$API" -I"$MODULE/build/generated" "$MODULE/dsp/dirt_delay.c" \
  "$MODULE/dsp/dirt_engine.c" -o "$OUT/dirt-delay.so" -lm
cp "$MODULE/module.json" "$OUT/"
if [[ "${1:-arm64}" == arm64 ]]; then
  python3 "$MODULE/scripts/check_abi.py" "$OUT/dirt-delay.so"
  cp "$MODULE/LICENSE" "$MODULE/THIRD_PARTY_LICENSES.md" "$MODULE/help.json" "$MODULE/README.md" "$OUT/"
  tar -czf "$MODULE/dist/dirt-delay-module.tar.gz" -C "$MODULE/dist" dirt-delay
fi
file "$OUT/dirt-delay.so"
