#!/usr/bin/env bash
# Build everything: hook blob -> ShellUI renderer -> injector payload, then the data folder.
#   PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
if ! python3 -c "import elftools" 2>/dev/null; then
  echo "Missing pyelftools: python3 -m pip install pyelftools" >&2
  exit 1
fi
command -v xxd >/dev/null || { echo "Missing xxd (apt install xxd)" >&2; exit 1; }
python3 tools/gen_imports.py
# one id for this build: shown on the TV picker, the phone page and in the log, so a payload and
# a web folder from different builds can never be mixed up unnoticed
export DIMOD_VERSION="0.6"
export DIMOD_BUILD="${DIMOD_BUILD:-$(date -u +%y%m%d.%H%M)}"
printf '#pragma once\n#define DIMOD_VERSION "%s"\n#define DIMOD_BUILD "%s"\n' "$DIMOD_VERSION" "$DIMOD_BUILD" > src/injector/build_id.h.new
cmp -s src/injector/build_id.h.new src/injector/build_id.h 2>/dev/null && rm src/injector/build_id.h.new || mv src/injector/build_id.h.new src/injector/build_id.h
make -C src/hook
make -C src/overlay
make -C src/injector
if python3 -c "import PIL" 2>/dev/null; then
  python3 tools/build_data.py
else
  echo "Pillow not installed: skipping data/dimod (pip install Pillow)" >&2
fi
echo "Built build/diinject.elf  (v$DIMOD_VERSION build $DIMOD_BUILD)"
