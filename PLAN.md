# Plan

**Where we left off (2026-09-25):** the first version works on the
command line and inside GIMP (headless test). Next: look at it in GIMP's
filter dialog, then real photos with depth maps. The status of all the
GIMP 3 work is in
[gimp-plugin-devtools/STATUS.md](https://github.com/sandbranch/gimp-plugin-devtools/blob/main/STATUS.md).

## 1. First version (done)

- [x] Operation `depth:blur`: focus depth, sharp range, blur
      radius, depth convention, bokeh circle / polygon / ring, highlights
- [x] Depth precedence and filling behind (docs/design.md)
- [x] Synthetic test scene and `tests/run.sh`; headless GIMP test
      `tests/gimp-test.sh`
- [x] Speed on 24 MP: same as `gegl:lens-blur`

## 2. In GIMP

- [ ] Check the dialog: menu entry, the Aux Input chooser, the "visible"
      rules of the shape settings, slider ranges
- [ ] Pick the focus on the canvas: focus x/y properties that read the
      depth map there, as an alternative to the focus depth slider
- [ ] A way to keep it editable despite GIMP merging aux filters: e.g. an
      optional depth map file path property (GIMP can save a string);
      or help GIMP with its TODO (serializing a drawable reference)

## 3. Real photos

- [ ] Photos with depth maps from a depth estimation tool; check edges
      where the depth map is soft or slightly off (hair, leaves), which
      synthetic scenes do not show
- [ ] Separate strength in front of and behind the focus (optional)

## 4. Speed for large radii

- [ ] Blur slices with a large radius at half or quarter size and scale
      them up; radius 100 on 24 MP should stay under a few seconds
- [ ] Small pieces: every piece GEGL asks for is computed with two
      margins of the radius around it, so rendering 1024x1024 in pieces
      of 64 took 7 times as long as at once at radius 20, and 18 times at
      radius 60 (2026-09-26, `ninja -C build bench-pieces`, see
      tests/bench-pieces.c). GIMP's chunk iterator makes the
      pieces smaller for slow filters (down to 4096 pixels). Options: a
      get_cached_region () that rounds the piece up to the 256 pixel
      grid, so that GEGL caches whole tiles (the result no longer
      depends on the pieces, so this is safe); or the whole image.
      Memory is also worth a look: by the buffer sizes in process_tile (),
      about 90 MB per thread at radius 100 (computed, not measured).

## 5. Upstream GEGL

- [ ] Report: gegl_operation_get_source_space () ignores its pad
      argument and always looks at "input" (gegl/operation/
      gegl-operation.c, 0.4.72 and master as of 2026-09); this
      operation works around it for the aux pad.

## 6. Release

- [ ] Before/after examples in the README
- [ ] Offer it to the community (discuss.pixls.us, GIMP forums); ask the
      GEGL developers whether they want depth precedence in
      `gegl:lens-blur` or a new operation upstream
