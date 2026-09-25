#!/bin/sh
# Tests the installed operation inside GIMP (Flatpak), without a window:
# see gimp-test.py. Install first (README), and close GIMP.
set -e

here=$(cd "$(dirname "$0")" && pwd)
out="$here/output"
mkdir -p "$out"
[ -f "$out/scene.png" ] || python3 "$here/make-scene.py" "$out"

flatpak run --filesystem="$here" --env=DEPTH_BLUR_TEST_OUT="$out" \
  --command=gimp-console-3.2 org.gimp.GIMP --no-interface --no-data \
  --batch-interpreter python-fu-eval \
  -b "exec(open('$here/gimp-test.py').read())" --quit
