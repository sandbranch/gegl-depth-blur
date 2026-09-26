# Runs inside GIMP 3: applies depth:blur with the depth map layer
# on its aux input, the way it is used in GIMP, and exports the result.
# Run with tests/gimp-test.sh, which compares the result with the gegl
# command line (keep the settings here and there the same).
#
# GIMP (3.2, and master as of 2026-09) merges every filter with an aux
# input into the layer: it cannot yet save a layer reference inside a
# non-destructive filter (see the TODO in app/tools/gimpfiltertool.c).
import os

import gi
gi.require_version('Gimp', '3.0')
from gi.repository import Gimp, Gio

out = os.environ['DEPTH_BLUR_TEST_OUT']

image = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE,
                       Gio.File.new_for_path(os.path.join(out, 'scene.png')))
photo = image.get_layers()[0]
depth = Gimp.file_load_layer(Gimp.RunMode.NONINTERACTIVE, image,
                             Gio.File.new_for_path(os.path.join(out, 'depth.png')))
image.insert_layer(depth, None, 0)
depth.set_visible(False)

f = Gimp.DrawableFilter.new(photo, 'depth:blur', 'Depth Blur')
config = f.get_config()
config.set_property('focus', 0.95)
config.set_property('radius', 25.0)
config.set_property('highlight-factor', 0.6)
try:
    config.set_property('shape', 'polygon')
except TypeError:
    print('shape: not a choice, trying the number')
    config.set_property('shape', 1)
f.set_aux_input('aux', depth)
f.update()
print('shape in config:', config.get_property('shape'))
photo.merge_filter(f)

Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, image,
               Gio.File.new_for_path(os.path.join(out, 'gimp-result.png')), None)
print('saved', os.path.join(out, 'gimp-result.png'))
image.delete()
