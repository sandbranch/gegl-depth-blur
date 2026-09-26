#!/bin/sh
# Tests the operation from build/ inside GIMP (Flatpak), without a window:
# see gimp-test.py. GIMP runs with a profile of its own in tests/output and
# loads the operation from build/, so nothing needs to be installed, and
# the user's GIMP and its settings are not touched. Then checks that GIMP's
# result is the same as the one from the gegl command line.
set -e

here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out="$here/output"
mkdir -p "$out"
[ -f "$out/scene.png" ] || python3 "$here/make-scene.py" "$out"

# a folder with only the module (see run.sh), and a fresh profile
mod="$out/gimp-modules"
rm -rf "$mod" "$out/gimp-profile"
mkdir -p "$mod" "$out/gimp-profile"
cp "$top/build/depth-blur.so" "$mod/"

props="focus=0.95 radius=25 highlight-factor=0.6 shape=polygon"

rm -f "$out/gimp-result.png"
flatpak run --filesystem="$here" \
  --env=DEPTH_BLUR_TEST_OUT="$out" \
  --env=GIMP3_DIRECTORY="$out/gimp-profile" \
  --env=GEGL_PATH="$mod:/app/lib/gegl-0.4" \
  --command=gimp-console-3.2 org.gimp.GIMP --no-interface --no-data --no-fonts \
  --batch-interpreter python-fu-eval \
  -b "exec(open('$here/gimp-test.py').read())" --quit

# shellcheck disable=SC2086
"$here/run.sh" gimp-cli $props >/dev/null

python3 - "$out/gimp-result.png" "$out/gimp-cli.png" "$out/scene.png" <<'EOF'
import sys

import numpy as np
from PIL import Image

gimp, cli, scene = (np.asarray(Image.open(f).convert("RGB")).astype(int)
                    for f in sys.argv[1:])
ok = True

def check(name, good, detail=""):
    global ok
    print(("PASS" if good else "FAIL") + "  " + name + (": " + detail if detail else ""))
    ok = ok and good

check("GIMP result has the size of the image", gimp.shape == scene.shape,
      "%s, image %s" % (gimp.shape, scene.shape))
if gimp.shape == cli.shape:
    d = np.abs(gimp - cli)
    # both are 8-bit; allow a level of rounding
    check("GIMP and the gegl command line agree", d.max() <= 2 and d.mean() < 0.1,
          "max difference %d, mean %.3f (of 255)" % (d.max(), d.mean()))
changed = np.abs(gimp - scene).max(axis=-1) > 8
check("GIMP result is blurred", changed.mean() > 0.3,
      "%.0f%% of pixels changed" % (100 * changed.mean()))
sys.exit(0 if ok else 1)
EOF
