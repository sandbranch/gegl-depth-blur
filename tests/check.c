/*
 * Automated checks for depth:blur
 *
 * check.c
 * Copyright 2026 by David
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Runs the operation on small synthetic images and checks the results
 * numerically: edge cases (tiny and thin images, missing or odd aux
 * inputs, NaN and infinite values, range limits of the properties),
 * invariants (a flat image stays flat, light is conserved, what is in
 * focus stays sharp, near covers far) and that the result does not depend
 * on how GEGL splits the work into pieces or threads.
 *
 *   check <depth-blur.so>
 *
 * Prints PASS or FAIL per case and exits with 1 if any case failed.
 */

#include <gegl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FMT "RaGaBaA float"

static gint n_failed   = 0;
static gint n_passed   = 0;
static gint n_messages = 0;   /* warnings and criticals from GLib and GEGL */

static void
log_handler (const gchar    *domain,
             GLogLevelFlags  level,
             const gchar    *message,
             gpointer        data)
{
  (void) data;
  if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING))
    n_messages++;
  g_log_default_handler (domain, level, message, NULL);
}

static void
report (const gchar *name,
        gboolean     ok,
        const gchar *detail)
{
  printf ("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
          detail ? ": " : "", detail ? detail : "");
  fflush (stdout);

  if (ok)
    n_passed++;
  else
    n_failed++;
}

/* images ---------------------------------------------------------------- */

typedef void (*PixelFunc) (gint x, gint y, gfloat *pixel, gpointer data);

static GeglBuffer *
make_buffer (const gchar *format,
             gint         x0,
             gint         y0,
             gint         width,
             gint         height,
             PixelFunc    func,
             gpointer     data)
{
  const Babl *fmt    = babl_format (format);
  gint        nch    = babl_format_get_n_components (fmt);
  GeglBuffer *buffer = gegl_buffer_new (GEGL_RECTANGLE (x0, y0, width, height),
                                        fmt);
  gfloat     *pixels = g_new0 (gfloat, (gsize) width * height * nch);
  gchar      *ffmt;
  gint        x, y;

  /* the function writes up to 4 components, of which the first nch are
   * kept */
  for (y = 0; y < height; y++)
    for (x = 0; x < width; x++)
      {
        gfloat p[4] = { 0.0f, };

        func (x0 + x, y0 + y, p, data);
        memcpy (pixels + ((gsize) y * width + x) * nch, p,
                nch * sizeof (gfloat));
      }

  /* the pixels are made as float: "R'G'B' u8" -> "R'G'B' float" */
  if (g_str_has_suffix (format, " u8") || g_str_has_suffix (format, " u16"))
    {
      gchar *model = g_strndup (format, strrchr (format, ' ') - format);

      ffmt = g_strconcat (model, " float", NULL);
      g_free (model);
    }
  else
    ffmt = g_strdup (format);
  gegl_buffer_set (buffer, NULL, 0, babl_format (ffmt), pixels,
                   GEGL_AUTO_ROWSTRIDE);
  g_free (ffmt);
  g_free (pixels);

  return buffer;
}

static void
px_color (gint x, gint y, gfloat *p, gpointer data)
{
  const gfloat *c = data;

  (void) x; (void) y;
  memcpy (p, c, 4 * sizeof (gfloat));
}

static void
px_value (gint x, gint y, gfloat *p, gpointer data)
{
  (void) x; (void) y;
  p[0] = *(const gfloat *) data;
}

/* a textured, colorful image with some bright spots */
static void
px_texture (gint x, gint y, gfloat *p, gpointer data)
{
  (void) data;
  p[0] = 0.3f + 0.25f * sinf (x * 0.37f) * cosf (y * 0.23f);
  p[1] = 0.4f + 0.3f * sinf (x * 0.11f + y * 0.19f);
  p[2] = ((x / 5 + y / 7) % 2) ? 0.8f : 0.1f;
  p[3] = 1.0f;
  if ((x * 7 + y * 13) % 97 == 0)
    p[0] = p[1] = p[2] = 1.0f;
}

/* a depth map with a bit of everything: a far wall, a slope, a near bar
 * and a disc at the middle depth */
static void
px_depth (gint x, gint y, gfloat *p, gpointer data)
{
  gfloat d = 0.1f;

  (void) data;
  if (y > 40)
    d = 0.1f + 0.8f * CLAMP ((y - 40) / 60.0f, 0.0f, 1.0f);
  if ((x - 60) * (x - 60) + (y - 45) * (y - 45) < 30 * 30)
    d = 0.5f;
  if (x > 15 && x < 30)
    d = 0.95f;
  p[0] = d;
}

/* the function in data, moved by 37, -11 */
typedef struct { PixelFunc func; } Shifted;

static void
px_shifted (gint x, gint y, gfloat *p, gpointer data)
{
  ((const Shifted *) data)->func (x + 37, y - 11, p, NULL);
}

/* the same at four times the size, with a long slope */
static void
px_depth_big (gint x, gint y, gfloat *p, gpointer data)
{
  gfloat d = 0.1f;

  (void) data;
  if (y > 120)
    d = 0.1f + 0.8f * CLAMP ((y - 120) / 200.0f, 0.0f, 1.0f);
  if ((x - 240) * (x - 240) + (y - 140) * (y - 140) < 90 * 90)
    d = 0.5f;
  if (x > 60 && x < 110)
    d = 0.95f;
  p[0] = d;
}

/* running the operation ----------------------------------------------- */

typedef struct
{
  GeglRectangle rect;
  gfloat       *pixels;   /* RaGaBaA float */
} Result;

static void
result_free (Result *r)
{
  g_free (r->pixels);
  r->pixels = NULL;
}

static GeglNode *
make_graph (GeglNode   **graph,
            GeglBuffer  *input,
            GeglBuffer  *aux)
{
  GeglNode *in, *blur;

  *graph = gegl_node_new ();
  in     = gegl_node_new_child (*graph, "operation", "gegl:buffer-source",
                                "buffer", input, NULL);
  blur   = gegl_node_new_child (*graph, "operation", "depth:blur", NULL);

  gegl_node_connect (in, "output", blur, "input");

  if (aux)
    {
      GeglNode *a = gegl_node_new_child (*graph,
                                         "operation", "gegl:buffer-source",
                                         "buffer", aux, NULL);
      gegl_node_connect (a, "output", blur, "aux");
    }

  return blur;
}

/* Renders the operation over its bounding box, or over rect if given, in
 * pieces of piece x piece pixels (0: at once), each in a graph of its own
 * so that no cache is shared between them.
 */
static Result
render_valist (GeglBuffer          *input,
               GeglBuffer          *aux,
               const GeglRectangle *rect,
               gint                 piece,
               const gchar         *first_property,
               va_list              args)
{
  GeglNode *graph, *blur;
  Result    r;
  gint      x, y;
  va_list   copy;

  blur = make_graph (&graph, input, aux);
  if (first_property)
    {
      va_copy (copy, args);
      gegl_node_set_valist (blur, first_property, copy);
      va_end (copy);
    }
  r.rect = rect ? *rect : gegl_node_get_bounding_box (blur);
  r.pixels = g_new0 (gfloat, (gsize) MAX (r.rect.width, 1) *
                             MAX (r.rect.height, 1) * 4);

  if (piece <= 0)
    {
      gegl_node_blit (blur, 1.0, &r.rect, babl_format (FMT), r.pixels,
                      r.rect.width * 4 * sizeof (gfloat), GEGL_BLIT_DEFAULT);
      g_object_unref (graph);
      return r;
    }

  g_object_unref (graph);

  for (y = r.rect.y; y < r.rect.y + r.rect.height; y += piece)
    for (x = r.rect.x; x < r.rect.x + r.rect.width; x += piece)
      {
        GeglRectangle p = { x, y,
                            MIN (piece, r.rect.x + r.rect.width - x),
                            MIN (piece, r.rect.y + r.rect.height - y) };
        gfloat *dst = r.pixels + ((gsize) (y - r.rect.y) * r.rect.width
                                  + (x - r.rect.x)) * 4;

        blur = make_graph (&graph, input, aux);
        if (first_property)
          {
            va_copy (copy, args);
            gegl_node_set_valist (blur, first_property, copy);
            va_end (copy);
          }
        gegl_node_blit (blur, 1.0, &p, babl_format (FMT), dst,
                        r.rect.width * 4 * sizeof (gfloat), GEGL_BLIT_DEFAULT);
        g_object_unref (graph);
      }

  return r;
}

static Result
render (GeglBuffer *input, GeglBuffer *aux, const gchar *first_property, ...)
{
  va_list args;
  Result  r;

  va_start (args, first_property);
  r = render_valist (input, aux, NULL, 0, first_property, args);
  va_end (args);

  return r;
}

static Result
render_pieces (GeglBuffer          *input,
               GeglBuffer          *aux,
               const GeglRectangle *rect,
               gint                 piece,
               const gchar         *first_property, ...)
{
  va_list args;
  Result  r;

  va_start (args, first_property);
  r = render_valist (input, aux, rect, piece, first_property, args);
  va_end (args);

  return r;
}

static Result
read_buffer (GeglBuffer *buffer)
{
  Result r;

  r.rect   = *gegl_buffer_get_extent (buffer);
  r.pixels = g_new (gfloat, (gsize) r.rect.width * r.rect.height * 4);
  gegl_buffer_get (buffer, &r.rect, 1.0, babl_format (FMT), r.pixels,
                   GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);

  return r;
}

/* with $DEPTH_BLUR_CHECK_SAVE set to a folder, saves a result there as
 * name.png, to look at */
static void
save (const Result *r, const gchar *name)
{
  const gchar *dir = g_getenv ("DEPTH_BLUR_CHECK_SAVE");
  GeglBuffer  *buffer;
  GeglNode    *graph, *src, *sink;
  gchar       *path;

  if (! dir)
    return;

  buffer = gegl_buffer_new (&r->rect, babl_format (FMT));
  gegl_buffer_set (buffer, &r->rect, 0, babl_format (FMT), r->pixels,
                   GEGL_AUTO_ROWSTRIDE);
  path  = g_strdup_printf ("%s/%s.png", dir, name);
  graph = gegl_node_new ();
  src   = gegl_node_new_child (graph, "operation", "gegl:buffer-source",
                               "buffer", buffer, NULL);
  sink  = gegl_node_new_child (graph, "operation", "gegl:png-save",
                               "path", path, NULL);
  gegl_node_link (src, sink);
  gegl_node_process (sink);
  g_object_unref (graph);
  g_object_unref (buffer);
  g_free (path);
}

/* measurements --------------------------------------------------------- */

static gsize
n_values (const Result *r)
{
  return (gsize) r->rect.width * r->rect.height * 4;
}

static gboolean
all_finite (const Result *r)
{
  gsize i;

  for (i = 0; i < n_values (r); i++)
    if (! isfinite (r->pixels[i]))
      return FALSE;

  return TRUE;
}

static gdouble
max_diff (const Result *a, const Result *b)
{
  gdouble m = 0.0;
  gsize   i;

  if (! gegl_rectangle_equal (&a->rect, &b->rect))
    return INFINITY;

  for (i = 0; i < n_values (a); i++)
    {
      gdouble d = fabs ((gdouble) a->pixels[i] - b->pixels[i]);

      if (! (d <= m))   /* also NaN */
        m = isnan (d) ? INFINITY : d;
    }

  return m;
}

/* largest difference from a constant premultiplied color */
static gdouble
max_diff_color (const Result *r, const gfloat *color)
{
  gdouble m = 0.0;
  gsize   i;

  for (i = 0; i < n_values (r); i++)
    {
      gdouble d = fabs ((gdouble) r->pixels[i] - color[i % 4]);

      if (! (d <= m))
        m = isnan (d) ? INFINITY : d;
    }

  return m;
}

static const gfloat *
pixel_at (const Result *r, gint x, gint y)
{
  return r->pixels + ((gsize) (y - r->rect.y) * r->rect.width
                      + (x - r->rect.x)) * 4;
}

static void
check_diff (const gchar *name, gdouble diff, gdouble tolerance)
{
  gchar *detail = g_strdup_printf ("max difference %.3g (tolerance %.3g)",
                                   diff, tolerance);

  report (name, diff <= tolerance, detail);
  g_free (detail);
}

/* cases ---------------------------------------------------------------- */

static const gfloat RED_HALF[4]  = { 0.4f, 0.05f, 0.1f, 0.5f };   /* premultiplied */
static const gfloat OPAQUE[4]    = { 0.2f, 0.5f, 0.7f, 1.0f };
static const gfloat CLEAR[4]     = { 0.0f, 0.0f, 0.0f, 0.0f };

/* a flat image stays flat, whatever the depth map and settings: every
 * kernel is normalized */
static void
test_flat (GeglBuffer *depth)
{
  GeglBuffer *img = make_buffer (FMT, 0, 0, 120, 100, px_color,
                                 (gpointer) RED_HALF);
  struct { const gchar *name; gint shape; gint blades; gdouble thick;
           gdouble hl; gdouble lo; gdouble hi; } cases[] = {
    { "flat circle",               0, 6,  0.3,  0.0, 0.8, 1.0 },
    { "flat polygon 3",            1, 3,  0.3,  0.0, 0.8, 1.0 },
    { "flat polygon 12",           1, 12, 0.3,  0.0, 0.8, 1.0 },
    { "flat ring thin",            2, 6,  0.05, 0.0, 0.8, 1.0 },
    { "flat ring full",            2, 6,  1.0,  0.0, 0.8, 1.0 },
    { "flat highlights",           0, 6,  0.3,  1.0, 0.0, 0.1 },
    { "flat highlights lo=hi",     0, 6,  0.3,  1.0, 0.2, 0.2 },
    { "flat highlights lo>hi",     0, 6,  0.3,  1.0, 0.9, 0.1 },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      Result r = render (img, depth,
                         "focus", 0.9, "radius", 25.0,
                         "shape", cases[i].shape, "blades", cases[i].blades,
                         "rotation", 17.0,
                         "ring-thickness", cases[i].thick,
                         "highlight-factor", cases[i].hl,
                         "highlight-threshold-low", cases[i].lo,
                         "highlight-threshold-high", cases[i].hi, NULL);

      check_diff (cases[i].name, max_diff_color (&r, RED_HALF), 2e-5);
      result_free (&r);
    }

  g_object_unref (img);
}

/* no blur at radius 0; and none where the depth map is at the focus */
static void
test_sharp (GeglBuffer *img)
{
  Result      orig = read_buffer (img);
  GeglBuffer *at_focus;
  Result      r;
  gfloat      half = 0.5f, one = 1.0f;

  r = render (img, NULL, "radius", 0.0, NULL);
  check_diff ("radius 0 is the identity", max_diff (&r, &orig), 0.0);
  result_free (&r);

  at_focus = make_buffer ("Y' float", 0, 0, 120, 100, px_value, &half);
  r = render (img, at_focus, "focus", 0.5, "radius", 100.0, NULL);
  check_diff ("depth at the focus stays sharp", max_diff (&r, &orig), 1e-5);
  result_free (&r);
  g_object_unref (at_focus);

  at_focus = make_buffer ("Y' float", 0, 0, 120, 100, px_value, &one);
  r = render (img, at_focus, "focus", 1.0, "radius", 100.0,
              "white-is-near", FALSE, NULL);
  check_diff ("focus 1, black is near", max_diff (&r, &orig), 1e-5);
  result_free (&r);

  r = render (img, NULL, "focus", 0.5, "radius", 60.0, "sharp-range", 1.0,
              NULL);
  check_diff ("sharp range 1 keeps everything sharp", max_diff (&r, &orig),
              1e-5);
  result_free (&r);
  g_object_unref (at_focus);

  result_free (&orig);
}

/* the output covers the input exactly, also away from the origin */
static void
test_extent (void)
{
  GeglBuffer   *img   = make_buffer (FMT, 37, -11, 50, 30, px_texture, NULL);
  GeglBuffer   *depth = make_buffer ("Y' float", 37, -11, 50, 30, px_depth,
                                     NULL);
  GeglNode     *graph, *blur;
  GeglRectangle box;
  GeglBuffer   *img0, *depth0;
  Shifted       texture = { px_texture }, depth_map = { px_depth };
  Result        at, zero;
  gchar        *detail;

  blur = make_graph (&graph, img, depth);
  gegl_node_set (blur, "radius", 20.0, NULL);
  box = gegl_node_get_bounding_box (blur);
  detail = g_strdup_printf ("%d,%d %dx%d", box.x, box.y, box.width,
                            box.height);
  report ("bounding box is the input's, at an offset",
          gegl_rectangle_equal (&box, GEGL_RECTANGLE (37, -11, 50, 30)),
          detail);
  g_free (detail);
  g_object_unref (graph);

  /* the same pixels moved to the origin give the same result there */
  img0   = make_buffer (FMT, 0, 0, 50, 30, px_shifted, &texture);
  depth0 = make_buffer ("Y' float", 0, 0, 50, 30, px_shifted, &depth_map);

  at   = render (img, depth, "radius", 20.0, "highlight-factor", 0.5, NULL);
  zero = render (img0, depth0, "radius", 20.0, "highlight-factor", 0.5, NULL);
  detail = g_strdup_printf ("%d,%d %dx%d", zero.rect.x, zero.rect.y,
                            zero.rect.width, zero.rect.height);
  report ("moved to the origin, the bounding box moves",
          gegl_rectangle_equal (&zero.rect, GEGL_RECTANGLE (0, 0, 50, 30)),
          detail);
  g_free (detail);
  zero.rect = at.rect;
  check_diff ("same pixels at an offset and at the origin",
              max_diff (&at, &zero), 0.0);
  result_free (&at);
  result_free (&zero);
  g_object_unref (img0);
  g_object_unref (depth0);
  g_object_unref (img);
  g_object_unref (depth);
}

/* tiny and thin images, and radii far larger than the image */
static void
test_small (void)
{
  gint sizes[][2] = { { 1, 1 }, { 1, 57 }, { 57, 1 }, { 2, 3 }, { 7, 300 },
                      { 300, 7 } };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
      gint        w     = sizes[i][0], h = sizes[i][1];
      GeglBuffer *flat  = make_buffer (FMT, 0, 0, w, h, px_color,
                                       (gpointer) OPAQUE);
      GeglBuffer *img   = make_buffer (FMT, 0, 0, w, h, px_texture, NULL);
      GeglBuffer *depth = make_buffer ("Y' float", 0, 0, w, h, px_depth, NULL);
      gchar      *name;
      Result      r;

      name = g_strdup_printf ("%dx%d flat, radius 100", w, h);
      r = render (flat, depth, "radius", 100.0, "focus", 0.0, NULL);
      check_diff (name, max_diff_color (&r, OPAQUE), 2e-5);
      result_free (&r);
      g_free (name);

      name = g_strdup_printf ("%dx%d texture, ring, finite and in range", w, h);
      r = render (img, depth, "radius", 100.0, "focus", 0.3, "shape", 2,
                  "highlight-factor", 1.0, NULL);
      {
        gboolean ok = all_finite (&r) &&
                      r.rect.width == w && r.rect.height == h;
        gsize    j;

        for (j = 0; ok && j < n_values (&r); j++)
          ok = r.pixels[j] >= -1e-5f && r.pixels[j] <= 1.0f + 1e-5f;
        report (name, ok, NULL);
      }
      result_free (&r);
      g_free (name);

      g_object_unref (flat);
      g_object_unref (img);
      g_object_unref (depth);
    }
}

/* the depth map: missing, of another size, or with values out of range */
static void
test_aux (GeglBuffer *img)
{
  Result r, ref;
  gfloat far = 0.0f;

  /* without a depth map everything is at depth 0 (far, white is near) */
  {
    GeglBuffer *zero = make_buffer ("Y' float", 0, 0, 120, 100, px_value,
                                    &far);

    r   = render (img, NULL, "radius", 20.0, NULL);
    ref = render (img, zero, "radius", 20.0, NULL);
    check_diff ("no aux is a depth map of 0", max_diff (&r, &ref), 0.0);
    result_free (&r);
    result_free (&ref);
    g_object_unref (zero);
  }

  /* smaller and larger depth maps, at an offset: no crash, finite values,
   * the size of the input */
  {
    GeglRectangle rects[] = { { 10, 10, 30, 20 }, { -50, -40, 300, 250 },
                              { 500, 500, 10, 10 } };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (rects); i++)
      {
        GeglBuffer *d = make_buffer ("Y' float", rects[i].x, rects[i].y,
                                     rects[i].width, rects[i].height,
                                     px_depth, NULL);
        gchar *name = g_strdup_printf ("aux at %d,%d %dx%d", rects[i].x,
                                       rects[i].y, rects[i].width,
                                       rects[i].height);

        r = render (img, d, "radius", 20.0, NULL);
        report (name, all_finite (&r) && r.rect.width == 120 &&
                      r.rect.height == 100, NULL);
        result_free (&r);
        g_free (name);
        g_object_unref (d);
      }
  }

  /* NaN and infinite depths are no depth at all, not a crash */
  {
    gfloat      bad[] = { NAN, INFINITY, -INFINITY, -3.0f, 7.0f };
    const gchar *what[] = { "NaN", "+inf", "-inf", "-3", "7" };
    guint       i;

    for (i = 0; i < G_N_ELEMENTS (bad); i++)
      {
        GeglBuffer *d    = make_buffer ("Y' float", 0, 0, 120, 100, px_value,
                                        &bad[i]);
        gchar      *name = g_strdup_printf ("depth map of %s", what[i]);

        r = render (img, d, "radius", 20.0, "focus", 0.2, NULL);
        report (name, all_finite (&r), NULL);
        result_free (&r);
        g_free (name);
        g_object_unref (d);
      }
  }

  /* values out of range act as the nearest end of the range */
  {
    gfloat      v[] = { -3.0f, 0.0f, 7.0f, 1.0f };
    GeglBuffer *d[4];
    Result      rr[4];
    guint       i;

    for (i = 0; i < 4; i++)
      {
        d[i]  = make_buffer ("Y' float", 0, 0, 120, 100, px_value, &v[i]);
        rr[i] = render (img, d[i], "radius", 20.0, "focus", 0.2, NULL);
      }
    check_diff ("depth below 0 is 0", max_diff (&rr[0], &rr[1]), 0.0);
    check_diff ("depth above 1 is 1", max_diff (&rr[2], &rr[3]), 0.0);
    for (i = 0; i < 4; i++)
      {
        result_free (&rr[i]);
        g_object_unref (d[i]);
      }
  }
}

/* an aux input alone, with nothing on the input: GEGL still calls
 * process() of a composer then */
static void
test_no_input (GeglBuffer *depth)
{
  GeglNode *graph = gegl_node_new ();
  GeglNode *aux   = gegl_node_new_child (graph,
                                         "operation", "gegl:buffer-source",
                                         "buffer", depth, NULL);
  GeglNode *blur  = gegl_node_new_child (graph, "operation", "depth:blur",
                                         "radius", 20.0, NULL);
  gfloat   *pixels = g_new0 (gfloat, 50 * 40 * 4);
  gint      before = n_messages;
  Result    r;

  gegl_node_connect (aux, "output", blur, "aux");
  gegl_node_blit (blur, 1.0, GEGL_RECTANGLE (5, 5, 50, 40), babl_format (FMT),
                  pixels, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
  r.rect   = *GEGL_RECTANGLE (5, 5, 50, 40);
  r.pixels = pixels;
  report ("no input, only aux: no warnings, empty result",
          n_messages == before && max_diff_color (&r, CLEAR) == 0.0, NULL);
  result_free (&r);
  g_object_unref (graph);
}

/* NaN or infinity in the image may spoil the pixels it blurs into, but no
 * more than that */
static void
test_bad_input (void)
{
  gfloat bad[] = { NAN, INFINITY };
  const gchar *what[] = { "NaN", "+inf" };
  guint  i;

  for (i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      GeglBuffer *img = make_buffer (FMT, 0, 0, 200, 60, px_color,
                                     (gpointer) OPAQUE);
      gfloat      p[4] = { bad[i], 0.5f, 0.5f, 1.0f };
      gchar      *name;
      Result      r;
      gint        x, y, n_bad = 0, n_far = 0;

      gegl_buffer_set (img, GEGL_RECTANGLE (20, 30, 1, 1), 0,
                       babl_format (FMT), p, GEGL_AUTO_ROWSTRIDE);
      r = render (img, NULL, "radius", 10.0, NULL);

      for (y = 0; y < 60; y++)
        for (x = 0; x < 200; x++)
          {
            const gfloat *q = pixel_at (&r, x, y);

            if (! (isfinite (q[0]) && isfinite (q[1]) && isfinite (q[2]) &&
                   isfinite (q[3])))
              {
                n_bad++;
                if (abs (x - 20) > 12 || abs (y - 30) > 12)
                  n_far++;
              }
          }

      name = g_strdup_printf ("a %s pixel spoils only its neighborhood",
                              what[i]);
      {
        gchar *detail = g_strdup_printf ("%d bad pixels, %d beyond the "
                                         "blur radius", n_bad, n_far);
        report (name, n_far == 0, detail);
        g_free (detail);
      }
      g_free (name);
      result_free (&r);
      g_object_unref (img);
    }
}

/* transparency: premultiplied colors blur without dark or bright fringes */
static void
test_alpha (GeglBuffer *depth)
{
  GeglBuffer *clear = make_buffer (FMT, 0, 0, 120, 100, px_color,
                                   (gpointer) CLEAR);
  Result      r;

  r = render (clear, depth, "radius", 30.0, "highlight-factor", 1.0, NULL);
  check_diff ("transparent stays transparent", max_diff_color (&r, CLEAR),
              0.0);
  result_free (&r);
  g_object_unref (clear);

  /* an opaque square on a transparent image: the color of what is
   * visible stays that of the square (no darkening from the transparent
   * pixels), and the alpha adds up to the area of the square */
  {
    GeglBuffer *img = make_buffer (FMT, 0, 0, 120, 100, px_color,
                                   (gpointer) CLEAR);
    gfloat     *sq  = g_new (gfloat, 20 * 20 * 4);
    gdouble     worst = 0.0, alpha = 0.0;
    gsize       j;
    gchar      *detail;

    for (j = 0; j < 20 * 20; j++)
      memcpy (sq + j * 4, OPAQUE, sizeof OPAQUE);
    gegl_buffer_set (img, GEGL_RECTANGLE (50, 40, 20, 20), 0,
                     babl_format (FMT), sq, GEGL_AUTO_ROWSTRIDE);
    g_free (sq);

    r = render (img, NULL, "radius", 10.0, NULL);
    for (j = 0; j < (gsize) r.rect.width * r.rect.height; j++)
      {
        const gfloat *q = r.pixels + j * 4;
        gint          c;

        alpha += q[3];
        if (q[3] > 1e-3f)
          for (c = 0; c < 3; c++)
            worst = MAX (worst, fabs (q[c] / q[3] - OPAQUE[c] / OPAQUE[3]));
      }
    check_diff ("blurred edge keeps its color", worst, 1e-4);
    detail = g_strdup_printf ("alpha sum %.2f, square 400", alpha);
    report ("blurred alpha adds up", fabs (alpha - 400.0) < 0.5, detail);
    g_free (detail);
    result_free (&r);
    g_object_unref (img);
  }
}

/* one point of light on black, far away: it becomes a disc of the size
 * and shape of the kernel, with the same total light */
static void
test_bokeh (void)
{
  const gfloat black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  struct { const gchar *name; gint shape; gint blades; gdouble thick;
           gdouble area; } cases[] = {
    /* radius 20: the circle's area, the hexagon's (3/2 sqrt 3 r^2), the
     * ring's with thickness 0.3 (r^2 - (0.7 r)^2) pi */
    { "circle",  0, 6, 0.3, G_PI * 400 },
    { "hexagon", 1, 6, 0.3, 1.5 * sqrt (3.0) * 400 },
    { "ring",    2, 6, 0.3, G_PI * (400 - 0.49 * 400) },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      GeglBuffer *img = make_buffer (FMT, 0, 0, 101, 101, px_color,
                                     (gpointer) black);
      gfloat      white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
      gdouble     sum = 0.0;
      gint        lit = 0;
      gsize       j;
      Result      r;
      gchar      *name, *detail;

      gegl_buffer_set (img, GEGL_RECTANGLE (50, 50, 1, 1), 0,
                       babl_format (FMT), white, GEGL_AUTO_ROWSTRIDE);
      /* the depth map is missing: all far, focus near, so radius 20 */
      r = render (img, NULL, "focus", 1.0, "radius", 20.0,
                  "shape", cases[i].shape, "blades", cases[i].blades,
                  "ring-thickness", cases[i].thick, NULL);
      for (j = 0; j < (gsize) r.rect.width * r.rect.height; j++)
        {
          sum += r.pixels[j * 4];
          if (r.pixels[j * 4] > 0.5 / cases[i].area)
            lit++;
        }

      name   = g_strdup_printf ("bokeh %s conserves light", cases[i].name);
      detail = g_strdup_printf ("sum %.5f", sum);
      report (name, fabs (sum - 1.0) < 1e-3, detail);
      g_free (name);
      g_free (detail);

      name   = g_strdup_printf ("bokeh %s has the area of its shape",
                                cases[i].name);
      detail = g_strdup_printf ("%d pixels lit, expected %.0f", lit,
                                cases[i].area);
      report (name, fabs (lit - cases[i].area) < 0.05 * cases[i].area,
              detail);
      g_free (name);
      g_free (detail);

      /* the ring is dark inside, the others are not */
      {
        gfloat center = pixel_at (&r, 50, 50)[0];

        name = g_strdup_printf ("bokeh %s center %s", cases[i].name,
                                cases[i].shape == 2 ? "is dark" : "is lit");
        report (name, cases[i].shape == 2 ? center < 1e-6f
                                          : center > 0.5 / cases[i].area,
                NULL);
        g_free (name);
      }

      result_free (&r);
      g_object_unref (img);
    }
}

/* A triangle points up at rotation 0 and turns counterclockwise on the
 * screen (the dial in GIMP shows the angle counterclockwise): its tip,
 * the farthest lit pixel from the point of light, is up at 0 and at the
 * left at 90 degrees. */
static void
test_rotation (void)
{
  const gfloat black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  gfloat       white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
  gdouble      angles[] = { 0.0, 90.0 };
  const gchar *want[]   = { "up", "left" };
  guint        i;

  for (i = 0; i < G_N_ELEMENTS (angles); i++)
    {
      GeglBuffer *img = make_buffer (FMT, 0, 0, 101, 101, px_color,
                                     (gpointer) black);
      Result      r;
      gint        x, y, up = 0, down = 0, left = 0, right = 0;
      const gchar *tip;
      gchar      *name;

      gegl_buffer_set (img, GEGL_RECTANGLE (50, 50, 1, 1), 0,
                       babl_format (FMT), white, GEGL_AUTO_ROWSTRIDE);
      r = render (img, NULL, "focus", 1.0, "radius", 30.0, "shape", 1,
                  "blades", 3, "rotation", angles[i], NULL);
      for (y = 0; y < 101; y++)
        for (x = 0; x < 101; x++)
          if (pixel_at (&r, x, y)[0] > 1e-5f)
            {
              up    = MAX (up,    50 - y);
              down  = MAX (down,  y - 50);
              left  = MAX (left,  50 - x);
              right = MAX (right, x - 50);
            }
      tip = up    > down + 5 && up    > MAX (left, right) ? "up"    :
            down  > up + 5   && down  > MAX (left, right) ? "down"  :
            left  > right + 5 && left > MAX (up, down)    ? "left"  :
            right > left + 5 && right > MAX (up, down)    ? "right" : "?";
      name = g_strdup_printf ("triangle at %g degrees points %s", angles[i],
                              want[i]);
      {
        gchar *detail = g_strdup_printf ("points %s (reach up %d down %d "
                                         "left %d right %d)", tip, up, down,
                                         left, right);
        report (name, strcmp (tip, want[i]) == 0, detail);
        g_free (detail);
      }
      g_free (name);
      result_free (&r);
      g_object_unref (img);
    }
}

/* Depth precedence: a sharp near subject is not bled over by the blurred
 * background, while a blurred near subject spreads over the sharp
 * background.
 */
static void
px_half_depth (gint x, gint y, gfloat *p, gpointer data)
{
  (void) y; (void) data;
  p[0] = x < 60 ? 1.0f : 0.0f;   /* left near, right far */
}

static void
px_half_color (gint x, gint y, gfloat *p, gpointer data)
{
  static const gfloat left[4]  = { 0.9f, 0.1f, 0.1f, 1.0f };
  static const gfloat right[4] = { 0.1f, 0.1f, 0.9f, 1.0f };

  (void) y; (void) data;
  memcpy (p, x < 60 ? left : right, sizeof left);
}

static void
test_precedence (void)
{
  GeglBuffer *img   = make_buffer (FMT, 0, 0, 120, 60, px_half_color, NULL);
  GeglBuffer *depth = make_buffer ("Y' float", 0, 0, 120, 60, px_half_depth,
                                   NULL);
  Result      r;
  gdouble     worst = 0.0;
  gint        x, y, c;

  /* focus on the near side: its pixels must stay exactly red, right up to
   * the edge */
  r = render (img, depth, "focus", 1.0, "radius", 20.0, NULL);
  for (y = 0; y < 60; y++)
    for (x = 0; x < 60; x++)
      for (c = 0; c < 4; c++)
        {
          gfloat want[4] = { 0.9f, 0.1f, 0.1f, 1.0f };

          worst = MAX (worst, fabs (pixel_at (&r, x, y)[c] - want[c]));
        }
  check_diff ("sharp near subject: no halo from behind", worst, 1e-5);
  result_free (&r);

  /* focus on the far side: the near side spreads over it, about radius
   * pixels far, and not farther */
  r = render (img, depth, "focus", 0.0, "radius", 20.0, NULL);
  {
    const gfloat *at_edge = pixel_at (&r, 62, 30);
    const gfloat *beyond  = pixel_at (&r, 85, 30);

    report ("blurred near subject spreads over the background",
            at_edge[0] > 0.2f, NULL);
    report ("and not beyond its radius",
            fabs (beyond[0] - 0.1f) < 1e-5 && fabs (beyond[2] - 0.9f) < 1e-5,
            NULL);
  }
  result_free (&r);

  g_object_unref (img);
  g_object_unref (depth);
}

/* the result does not depend on how the work is split */
static void
test_pieces (GeglBuffer *img, GeglBuffer *depth)
{
  gint        pieces[] = { 64, 37, 5 };   /* 5: too slow at radius 100 */
  guint       i;
  Result      whole;
  gdouble     radii[] = { 8.0, 40.0, 100.0 };
  guint       k;

  for (k = 0; k < G_N_ELEMENTS (radii); k++)
    {
      whole = render (img, depth, "radius", radii[k], "focus", 0.5,
                      "highlight-factor", 0.5, NULL);

      for (i = 0; i < G_N_ELEMENTS (pieces); i++)
        {
          gchar *name;
          Result r;

          if (pieces[i] < 16 && radii[k] > 50.0)
            continue;

          r = render_pieces (img, depth, NULL, pieces[i],
                             "radius", radii[k], "focus", 0.5,
                             "highlight-factor", 0.5, NULL);
          name = g_strdup_printf ("radius %g: in pieces of %d same as "
                                  "whole", radii[k], pieces[i]);

          check_diff (name, max_diff (&whole, &r), 1e-5);
          g_free (name);
          result_free (&r);
        }

      result_free (&whole);
    }

  /* a part renders as that part of the whole */
  {
    GeglRectangle part = { 33, 21, 41, 17 };
    Result        r;
    gdouble       worst = 0.0;
    gint          x, y, c;

    whole = render (img, depth, "radius", 30.0, "shape", 1, NULL);
    r     = render_pieces (img, depth, &part, 0,
                           "radius", 30.0, "shape", 1, NULL);
    for (y = part.y; y < part.y + part.height; y++)
      for (x = part.x; x < part.x + part.width; x++)
        for (c = 0; c < 4; c++)
          worst = MAX (worst, fabs (pixel_at (&r, x, y)[c] -
                                    pixel_at (&whole, x, y)[c]));
    check_diff ("a part renders as that part of the whole", worst, 1e-5);
    result_free (&r);
    result_free (&whole);
  }

  /* the same with one thread and with several, on an image large enough
   * that GEGL splits it between threads */
  {
    GeglBuffer *big   = make_buffer (FMT, 0, 0, 480, 320, px_texture, NULL);
    GeglBuffer *bigd  = make_buffer ("Y' float", 0, 0, 480, 320, px_depth_big,
                                     NULL);
    Result      one, many, again;

    g_object_set (gegl_config (), "threads", 1, NULL);
    one = render (big, bigd, "radius", 40.0, NULL);
    g_object_set (gegl_config (), "threads", 8, NULL);
    many  = render (big, bigd, "radius", 40.0, NULL);
    again = render (big, bigd, "radius", 40.0, NULL);
    check_diff ("one thread and eight threads agree", max_diff (&one, &many),
                1e-5);
    save (&one, "threads-1");
    save (&many, "threads-8");
    check_diff ("two runs are identical", max_diff (&many, &again), 0.0);
    g_object_unref (big);
    g_object_unref (bigd);
    result_free (&one);
    result_free (&many);
    result_free (&again);
  }
}

/* other input formats are converted and give the same result */
static void
px_gray (gint x, gint y, gfloat *p, gpointer data)
{
  gfloat c[4];

  px_texture (x, y, c, data);
  p[0] = c[1];
}

static void
px_gray_alpha (gint x, gint y, gfloat *p, gpointer data)
{
  px_gray (x, y, p, data);
  p[1] = (x % 3) ? 1.0f : 0.25f;
}

static void
test_formats (GeglBuffer *depth)
{
  struct { const gchar *format; PixelFunc func; } cases[] = {
    { "Y float",    px_gray },
    { "YA float",   px_gray_alpha },
    { "R'G'B' u8",  px_texture },
    { "R'G'B'A u16", px_texture },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      GeglBuffer *img  = make_buffer (cases[i].format, 0, 0, 120, 100,
                                      cases[i].func, NULL);
      GeglBuffer *conv = gegl_buffer_new (gegl_buffer_get_extent (img),
                                          babl_format (FMT));
      Result      a, b;
      gchar      *name;

      gegl_buffer_copy (img, NULL, GEGL_ABYSS_NONE, conv, NULL);
      a = render (img,  depth, "radius", 15.0, NULL);
      b = render (conv, depth, "radius", 15.0, NULL);
      name = g_strdup_printf ("input in %s", cases[i].format);
      check_diff (name, all_finite (&a) ? max_diff (&a, &b) : INFINITY, 1e-5);
      g_free (name);
      result_free (&a);
      result_free (&b);
      g_object_unref (img);
      g_object_unref (conv);
    }
}

/* The blur is linear, so it gives the same light in any RGB space; the
 * highlights depend on the luminance, which must be that of the space the
 * operation works in (the space of the input). The depth map is read as
 * stored, in its own space, whatever the space of the input. */
static void
test_space (GeglBuffer *depth)
{
  const gchar *spaces[] = { "Rec2020", "ProPhoto", "Adobish" };
  GeglBuffer  *img = make_buffer (FMT, 0, 0, 120, 100, px_texture, NULL);
  Result       ref;
  guint        i;

  ref = render (img, depth, "radius", 15.0, "highlight-factor", 1.0,
                "highlight-threshold-low", 0.3,
                "highlight-threshold-high", 0.6, NULL);

  for (i = 0; i < G_N_ELEMENTS (spaces); i++)
    {
      const Babl *space = babl_space (spaces[i]);
      const Babl *fmt   = babl_format_with_space (FMT, space);
      GeglBuffer *conv  = gegl_buffer_new (gegl_buffer_get_extent (img), fmt);
      Result      r;
      gchar      *name;

      gegl_buffer_copy (img, NULL, GEGL_ABYSS_NONE, conv, NULL);
      r = render (conv, depth, "radius", 15.0, "highlight-factor", 1.0,
                  "highlight-threshold-low", 0.3,
                  "highlight-threshold-high", 0.6, NULL);
      name = g_strdup_printf ("image in %s, depth map in sRGB: same "
                              "result", spaces[i]);
      check_diff (name, max_diff (&ref, &r), 1e-4);
      g_free (name);
      result_free (&r);
      g_object_unref (conv);

      /* the same numbers in a depth map in another space */
      {
        const Babl *yfmt = babl_format_with_space ("Y' float", space);
        GeglBuffer *d    = gegl_buffer_new (gegl_buffer_get_extent (depth),
                                            yfmt);
        gfloat     *v    = g_new (gfloat, 120 * 100);

        gegl_buffer_get (depth, NULL, 1.0, babl_format ("Y' float"), v,
                         GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
        gegl_buffer_set (d, NULL, 0, yfmt, v, GEGL_AUTO_ROWSTRIDE);
        r = render (img, d, "radius", 15.0, "highlight-factor", 1.0,
                    "highlight-threshold-low", 0.3,
                    "highlight-threshold-high", 0.6, NULL);
        name = g_strdup_printf ("depth map in %s is read as stored",
                                spaces[i]);
        check_diff (name, max_diff (&ref, &r), 0.0);
        g_free (name);
        result_free (&r);
        g_free (v);
        g_object_unref (d);
      }
    }

  result_free (&ref);
  g_object_unref (img);
}

/* every property at the ends of its range */
static void
test_ranges (GeglBuffer *img, GeglBuffer *depth)
{
  const gchar *doubles[] = { "focus", "sharp-range", "radius", "rotation",
                             "ring-thickness", "highlight-factor",
                             "highlight-threshold-low",
                             "highlight-threshold-high" };
  guint i, s;

  for (i = 0; i < G_N_ELEMENTS (doubles); i++)
    for (s = 0; s < 3; s++)
      {
        GeglNode   *graph, *blur;
        GParamSpec *spec;
        gdouble     value;
        Result      r;
        gchar      *name;

        blur = make_graph (&graph, img, depth);
        spec = gegl_node_find_property (blur, doubles[i]);
        value = G_PARAM_SPEC_DOUBLE (spec)->minimum;
        if (s == 1)
          value = G_PARAM_SPEC_DOUBLE (spec)->maximum;
        g_object_unref (graph);

        /* s == 2: at the end of the range with everything else at the
         * other end */
        if (s < 2)
          r = render (img, depth, "shape", 2, "radius", 30.0,
                      "highlight-factor", 0.7, doubles[i], value, NULL);
        else
          r = render (img, depth, "shape", 1, "blades", 3, "radius", 100.0,
                      "focus", 0.0, "ring-thickness", 0.05,
                      "highlight-factor", 1.0, doubles[i], value, NULL);

        name = g_strdup_printf ("%s = %g%s", doubles[i], value,
                                s == 2 ? " (with the others at extremes)" : "");
        report (name, all_finite (&r) && r.rect.width == 120 &&
                      r.rect.height == 100, NULL);
        g_free (name);
        result_free (&r);
      }

  for (s = 3; s <= 12; s++)
    {
      Result r   = render (img, depth, "shape", 1, "blades", s,
                           "radius", 12.0, NULL);
      gchar *name = g_strdup_printf ("blades = %u", s);

      report (name, all_finite (&r), NULL);
      g_free (name);
      result_free (&r);
    }
}

int
main (int    argc,
      char **argv)
{
  GeglBuffer *img, *depth;
  gchar      *dir, *link, *target;

  if (argc != 2)
    {
      fprintf (stderr, "usage: %s <depth-blur.so>\n", argv[0]);
      return 2;
    }

  /* GEGL loads every file in a module folder, and a build folder holds
   * files that it must not load: load the module from a folder of its own.
   * GEGL_PATH replaces GEGL's own list of folders, so that an installed
   * copy of the operation (in the user's folder) is not loaded instead. */
  dir    = g_dir_make_tmp ("depth-blur-check-XXXXXX", NULL);
  link   = g_build_filename (dir ? dir : ".", "depth-blur.so", NULL);
  target = g_canonicalize_filename (argv[1], NULL);
  if (! dir || symlink (target, link) != 0)
    {
      fprintf (stderr, "cannot link %s into a temporary folder\n", argv[1]);
      return 2;
    }

  {
    gchar *path = g_strconcat (dir, G_SEARCHPATH_SEPARATOR_S,
                               GEGL_PLUGINSDIR, NULL);

    g_setenv ("GEGL_PATH", path, TRUE);
    g_free (path);
  }

  gegl_init (&argc, &argv);
  g_log_set_default_handler (log_handler, NULL);

  if (! gegl_has_operation ("depth:blur"))
    {
      fprintf (stderr, "depth:blur did not load from %s\n", argv[1]);
      return 2;
    }

  img   = make_buffer (FMT, 0, 0, 120, 100, px_texture, NULL);
  depth = make_buffer ("Y' float", 0, 0, 120, 100, px_depth, NULL);

  test_flat (depth);
  test_sharp (img);
  test_extent ();
  test_small ();
  test_aux (img);
  test_no_input (depth);
  test_bad_input ();
  test_alpha (depth);
  test_bokeh ();
  test_rotation ();
  test_precedence ();
  test_pieces (img, depth);
  test_formats (depth);
  test_space (depth);
  test_ranges (img, depth);

  report ("no warnings or criticals from GLib or GEGL", n_messages == 0,
          NULL);

  g_object_unref (img);
  g_object_unref (depth);
  gegl_exit ();

  /* GEGL loads the module again when it is used after a pause: remove it
   * only at the end */
  unlink (link);
  g_rmdir (dir);
  g_free (target);
  g_free (link);
  g_free (dir);

  printf ("\n%d passed, %d failed\n", n_passed, n_failed);

  return n_failed ? 1 : 0;
}
