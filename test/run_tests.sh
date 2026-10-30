#!/usr/bin/env bash
# Host tests: protocol + tags (vs. independent Python reference) and the hook blob in a simulated game.
set -euo pipefail
cd "$(dirname "$0")"
python3 ../tools/gen_imports.py >/dev/null
out=$(mktemp -d)
cc -O1 -Wall -Wextra -I../src/core test_core.c ../src/core/diproto.c ../src/core/ditag.c -o "$out/test_core"
python3 -I ref_check.py "$out/test_core"
cc -O1 -pthread -Wall -Wno-unused-function -I../src/core -I../src/hook test_hook.c ../src/core/diproto.c ../src/core/ditag.c -o "$out/test_hook"
"$out/test_hook"
cc -O1 -Wall -Wno-unused-function -Istub -I../src/core -I../src/hook test_got.c ../src/injector/got.c -o "$out/test_got"
"$out/test_got"
c++ -std=c++20 -O1 -Wall -I../src/overlay/common test_fw.cpp -o "$out/test_fw"
"$out/test_fw"
cc -O1 -Wall -Wextra -I../src/core test_led.c -o "$out/test_led"
LED_OUT="$out/led.txt" "$out/test_led"
if command -v node >/dev/null; then node test_led.js ../src/web/index.html "$out/led.txt"; fi
if python3 -c "import PIL" 2>/dev/null; then
  python3 -I test_art.py >"$out/art.txt" || { cat "$out/art.txt"; exit 1; }   # any render -> 160x160 badge that fits its box
  python3 -I ../tools/build_data.py --check-art                                # the art in tools/art is named and readable
fi
echo "all host tests passed"
