#!/bin/sh
# Runs the numerical checks (check.sh), then renders the test scene with a
# set of settings into tests/output, using the operation from build/ and
# the GEGL of the Flatpak GIMP (or the system GEGL with GIMP_FLATPAK=0),
# and checks that every render has the size of the scene.
#
#   tests/run.sh                     checks and all cases
#   tests/run.sh name 'props...'     one case, e.g. tests/run.sh try 'radius=30'
set -e

here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out="$here/output"
mkdir -p "$out"

[ -f "$out/scene.png" ] || python3 "$here/make-scene.py" "$out"

# GEGL_PATH replaces GEGL's own list, and the build folder holds files that
# GEGL must not load: use a folder with only the module
mod="$out/modules"
rm -rf "$mod"
mkdir -p "$mod"
cp "$top/build/depth-blur.so" "$mod/"

render () {
  name=$1; shift
  if [ "${GIMP_FLATPAK:-1}" = 1 ] && flatpak info org.gimp.GIMP >/dev/null 2>&1; then
    flatpak run --filesystem="$here" --env=GEGL_PATH="$mod:/app/lib/gegl-0.4" \
      --command=gegl org.gimp.GIMP \
      "$out/scene.png" -o "$out/$name.png" -- \
      depth:blur "$@" aux=[ load path="$out/depth.png" ]
  else
    GEGL_PATH="$mod:$(pkg-config --variable=pluginsdir gegl-0.4)" gegl \
      "$out/scene.png" -o "$out/$name.png" -- \
      depth:blur "$@" aux=[ load path="$out/depth.png" ]
  fi
  echo "$out/$name.png"
}

if [ $# -gt 0 ]; then
  name=$1; shift
  # shellcheck disable=SC2086
  render "$name" $*
  exit
fi

"$here/check.sh"
echo

render focus-subject   focus=0.5  radius=20
render focus-near      focus=0.95 radius=20
render focus-far       focus=0.1  radius=20
render highlights      focus=0.5  radius=20 highlight-factor=0.6
render hexagon         focus=0.5  radius=20 highlight-factor=0.6 shape=polygon blades=6 rotation=15
render ring            focus=0.5  radius=20 highlight-factor=0.6 shape=ring
render sharp-range     focus=0.5  radius=20 sharp-range=0.2
render big             focus=0.5  radius=60

python3 - "$out" <<'EOF'
import sys

from PIL import Image

out = sys.argv[1]
size = Image.open(out + "/scene.png").size
bad = [n for n in ("focus-subject", "focus-near", "focus-far", "highlights",
                   "hexagon", "ring", "sharp-range", "big")
       if Image.open("%s/%s.png" % (out, n)).size != size]
print(("FAIL  renders of another size: " + " ".join(bad)) if bad
      else "PASS  every render has the size of the scene")
sys.exit(1 if bad else 0)
EOF
