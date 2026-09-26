# Design

## What exists, and what was missing

Checked in GEGL 0.4.62 (the Flatpak GIMP 3.2.6) and GEGL's source:

- `gegl:focus-blur` (GIMP's Filters > Blur > Focus Blur): blurs around a
  point or shape placed on the canvas; no depth map.
- `gegl:variable-blur`: blur radius from a mask on the aux input.
- `gegl:lens-blur` (operations/common-cxx/lens-blur.cc): a round lens
  kernel with a highlight boost, radius per pixel from a mask. Every pixel
  spreads by its own radius, whatever its depth.

What none of them do, and the Focus Blur plug-in for GIMP 2 (Kyoichiro
Suda, 2010, GPL 2 or later; last maintained at
github.com/JMoerman/gimp-focusblur-plugin, 2019) did:

1. **A focus depth**: blur grows with the distance in depth from it, in
   front and behind.
2. **Depth precedence**: near things cover far things. A blurred
   background must not bleed over a sharp subject in front of it (the
   halo of fake depth of field), while a blurred foreground spreads over
   what is behind it.
3. **Filling behind**: the edge of a blurred near object is partly
   transparent, so what lies behind it must be there to show.
4. **Bokeh shapes** and a highlight boost.

## Model

As in Focus Blur, the blur radius of a pixel is proportional to its
distance in depth from the focus depth:

    radius = blur_radius * max(0, |depth - focus| - sharp_range)

This is the physics of a thin lens when the depth map holds inverse
distance (disparity), which is what many depth estimation tools produce
(MiDaS, for one, predicts relative inverse depth): the
circle of confusion grows linearly with the difference in inverse
distance from the focused plane.

## Algorithm

Focus Blur gathers, for each output pixel, every neighbor whose blur disc
reaches it, sorted into depth bins, and stacks the bins from near to far
until the pixel is covered, then fills what is left from pixels behind it.
That is O(pixels × radius²).

This operation computes the same result by slices:

1. Each pixel goes to one of N depth slices, N = radius + 2, so that the
   blur of neighboring slices differs by at most a pixel. (Splitting a
   pixel between two slices makes a sharp surface partly transparent: the
   first version did that and showed the background through the subject's
   edge.)
2. From far to near, each slice is blurred with its own kernel: a
   normalized blur of the slice's premultiplied color, weighted by the
   highlight boost, and of its coverage. Because a slice is normalized by
   its own coverage, its colors never mix with those of other slices.
3. **Fill behind**: before a slice is laid down, the holes that nearer
   slices left in what is behind are filled by spreading what is behind
   into them, over the slice's blur radius (a normalized blur). What is
   spread is the stack of the farther slices alone, not what earlier
   fills put into the holes: spreading those again with each slice
   reached far beyond the margins of a tile, so the result changed with
   the tiles and threads GEGL splits the image into (by up to 0.13 at
   occlusion edges, seen in `tests/check.c`). A hole is filled fully
   where the blurred coverage of what is behind is one half or more (at
   its edge) and less towards where it ends, so that the fills of
   neighboring slices meet without a step.
4. The slice is laid over what is behind it with its blurred coverage
   (Porter-Duff over). Whatever stays uncovered takes the colors of what
   covers it partly.

Kernels are lists of horizontal spans, one per row (two for a ring),
averaged over 8 lines per row so that small kernels grow smoothly. A span
is summed from row prefix sums in double precision, so the cost per
pixel grows with the radius, not its square. Each slice is processed only
within its bounding box, and pixels that no part of the slice reaches are
skipped after one channel.

The operation works in linear light, premultiplied (`RaGaBaA float`); the
depth map is read as stored (`Y' float`), since it holds values, not
light. The image is processed in tiles of 256 pixels or more, each with
two margins of the blur radius (one for the slice blur, one for filling
behind).

Speed, measured on 24 megapixels (24 threads), after subtracting the time
to load and save the file: about 1.6 s at radius 20 and 4 s at radius 60,
the same as GEGL's `gegl:lens-blur` with a mask.

## Credits and borrowing

- The model (focus depth, depth precedence, filling behind, bokeh shapes,
  highlight boost) is that of Focus Blur by Kyoichiro Suda (GPL 2 or
  later, compatible with this project's GPL 3 or later). No code is taken
  from it; the slicing algorithm here is new.
- The names and meaning of the highlight threshold settings follow
  `gegl:lens-blur` (by Ell, LGPL 3 or later) so that they feel the same.

## GIMP and the aux input

GIMP shows an "Aux Input" layer chooser for any operation with an `aux`
pad, but merges such filters into the layer on OK, both from the filter
dialog and from scripts (`gimp-drawable-append-filter` refuses them). The
source (app/tools/gimpfiltertool.c, master as of 2026-09) has a TODO to
allow them non-destructively once GIMP can serialize a drawable reference.
Until then, this filter behaves like GIMP's own filters with an aux input.
