# gegl-depth-blur

Depth Blur, a filter for GIMP 3 (a GEGL operation): it blurs a photo the
way a lens does, from a depth map, for a shallow depth of field. What is
at the focus depth stays sharp, the blur grows in front of it and behind
it, near things cover far things, and out-of-focus points of light take
the shape of the aperture: a circle, a polygon with 3 to 12 blades, or the
ring of a mirror lens.

It is the successor to the **Focus Blur** plug-in for GIMP 2 by Kyoichiro
Suda, which never reached GIMP 3, and follows its model (see
[docs/design.md](docs/design.md)). It has a new name because GIMP has had
a built-in filter called Focus Blur since 2.10.20, which is a different
filter: it blurs around a point or shape you place, without a depth map.

**Status: first working version.** See [PLAN.md](PLAN.md).

## Using it

1. Put the depth map in the image as a layer: white is near, black is far
   (the convention of most depth estimation tools; the filter has a switch
   for the other way round). It can be painted by hand, made with a
   gradient, or estimated from the photo by a depth estimation tool.
2. Select the photo layer and open Filters > Blur > Depth Blur...
3. Pick the depth map layer as the **Aux Input**.
4. Set the **Focus depth** to the value of the depth map that should be
   sharp, and the **Blur radius**.

| Setting | What it does |
|---|---|
| Focus depth | the depth map value that is in focus |
| Sharp range | how far in front of and behind the focus everything stays sharp |
| Blur radius | the blur for a depth difference of 1 (white against black) |
| White is near | the depth map convention |
| Bokeh shape | circle, polygon (blades, rotation) or ring (thickness) |
| Highlights | how strongly bright points turn into discs of light, above the highlight threshold |

GIMP merges this filter into the layer when you press OK, as it does with
every filter that has an aux input (its own Lens Blur with a mask,
Displace, Bump Map): GIMP cannot yet keep a reference to another layer in
a non-destructive filter. Preview, split view and presets work as usual.
Duplicate the layer first to keep the original.

## Building and installing

Needs meson, ninja, a C compiler and the GEGL development files (0.4.62 or
newer).

    meson setup build -Dmoduledir=$HOME/.local/share/gegl-0.4/plug-ins
    ninja -C build install

For the Flatpak version of GIMP, build inside it with
[gimp-plugin-devtools](https://github.com/sandbranch/gimp-plugin-devtools):

    gimp-build.sh . meson setup build -Dmoduledir=\$GEGL_OPDIR
    gimp-build.sh . ninja -C build install

Restart GIMP after installing.

## Tests

- `tests/run.sh` renders a synthetic scene with a known depth map
  (`tests/make-scene.py`) with a set of settings into `tests/output/`,
  using the operation in `build/`.
- `tests/gimp-test.sh` applies the installed filter inside GIMP (Flatpak,
  no window) with the depth map layer as its aux input.

## License

GPL version 3 or later, see COPYING.
