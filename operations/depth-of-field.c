/*
 * Depth of field GEGL operation
 *
 * depth-of-field.c
 * Copyright 2026 by David
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Blurs an image the way a lens does, from a depth map on the aux input:
 * the blur grows with the distance in depth from the focus depth, nearer
 * things cover farther ones, and out-of-focus points of light take the
 * shape of the aperture.
 *
 * The model is that of the Focus Blur plug-in for GIMP 2 by Kyoichiro
 * Suda (GPL 2 or later): blur by depth, depth precedence (near over far)
 * and filling the gaps behind near things from what lies behind. This
 * implementation is new: the image is cut into depth slices, each slice
 * is blurred with its own kernel, and the slices are stacked from far to
 * near. See docs/design.md.
 */

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

enum_start (dof_shape)
  enum_value (DOF_SHAPE_CIRCLE,  "circle",  N_("Circle"))
  enum_value (DOF_SHAPE_POLYGON, "polygon", N_("Polygon"))
  enum_value (DOF_SHAPE_RING,    "ring",    N_("Ring"))
enum_end (DofShape)

property_double (focus, _("Focus depth"), 0.5)
  description (_("The value of the depth map that is in focus"))
  value_range (0.0, 1.0)
  ui_digits (3)

property_double (focus_range, _("Depth of field"), 0.0)
  description (_("How far in front of and behind the focus depth "
                 "everything stays sharp, in values of the depth map"))
  value_range (0.0, 1.0)
  ui_digits (3)

property_double (radius, _("Blur radius"), 15.0)
  description (_("Radius of the blur for a difference in depth of 1 from "
                 "the focus depth, such as white against black"))
  value_range (0.0, 100.0)
  ui_gamma (2.0)
  ui_meta ("unit", "pixel-distance")

property_boolean (white_is_near, _("White is near"), TRUE)
  description (_("White in the depth map is nearest to the camera, as in "
                 "the depth maps of most depth estimation tools. Turn off "
                 "when black is near."))

property_enum (shape, _("Bokeh shape"), DofShape, dof_shape,
               DOF_SHAPE_CIRCLE)
  description (_("The shape that out-of-focus points of light take: the "
                 "shape of the aperture of the lens"))

property_int (blades, _("Aperture blades"), 6)
  description (_("Number of sides of the polygon"))
  value_range (3, 12)
  ui_meta ("visible", "shape {polygon}")

property_double (rotation, _("Rotation"), 0.0)
  description (_("Rotation of the polygon"))
  value_range (0.0, 360.0)
  ui_meta ("unit", "degree")
  ui_meta ("direction", "ccw")
  ui_meta ("visible", "shape {polygon}")

property_double (ring_thickness, _("Ring thickness"), 0.3)
  description (_("Thickness of the ring as a part of its radius, as in the "
                 "bokeh of a mirror lens"))
  value_range (0.05, 1.0)
  ui_meta ("visible", "shape {ring}")

property_double (highlight_factor, _("Highlights"), 0.0)
  description (_("How strongly bright points of light stand out as discs "
                 "of light in the blur"))
  value_range (0.0, 1.0)

property_double (highlight_threshold_low, _("Highlight threshold (low)"), 0.8)
  value_range (0.0, 1.0)
  ui_meta ("role", "range-start")
  ui_meta ("unit", "luminance")
  ui_meta ("range-label", _("Highlight threshold"))
  ui_meta ("sensitive", "highlight-factor")

property_double (highlight_threshold_high, _("Highlight threshold (high)"), 1.0)
  value_range (0.0, 1.0)
  ui_meta ("role", "range-end")
  ui_meta ("unit", "luminance")
  ui_meta ("sensitive", "highlight-factor")

#else

#define GEGL_OP_COMPOSER
#define GEGL_OP_NAME     depth_of_field
#define GEGL_OP_C_SOURCE depth-of-field.c

#include "gegl-op.h"
#include <math.h>
#include <string.h>

#define TILE_SIZE  256
#define MAX_SLICES 104
#define N_SUB      8     /* vertical samples per kernel row */
#define EPSILON    1e-6f

/* Channels of a slice: premultiplied RGBA times membership and highlight
 * weight, the weight times membership, and the membership alone, which
 * becomes the coverage of the slice after blurring. The accumulated image
 * has premultiplied RGBA and coverage.
 */
#define SLICE_CH 6
#define ACC_CH   5

/* A kernel is a list of horizontal spans, one or two per row. For an
 * output pixel x a span covers [x + l, x + r] in pixel units, where pixel
 * j covers [j - 0.5, j + 0.5]; its sum is read from prefix sums as
 * C(x + r) - C(x + l), where C interpolates the prefix sums linearly.
 */
typedef struct
{
  gint   dy;
  gint   il, ir;   /* floor (l + 0.5), floor (r + 0.5) */
  gfloat fl, fr;   /* the fractions l + 0.5 - il, r + 0.5 - ir */
  gfloat sign;
} Span;

typedef struct
{
  Span  *spans;
  gint   n_spans;
  gint   reach;    /* largest horizontal or vertical offset, rounded up */
  gfloat norm;     /* 1 / area */
} Kernel;

static gboolean
polygon_interval (gint     sides,
                  gdouble  rotation,
                  gdouble  radius,
                  gdouble  y,
                  gdouble *l,
                  gdouble *r)
{
  gboolean found = FALSE;
  gint     i;

  for (i = 0; i < sides; i++)
    {
      /* one vertex points up at rotation 0, so an even number of blades
       * has a flat side at the bottom and top */
      gdouble a0 = rotation + G_PI / 2 + 2 * G_PI * i / sides;
      gdouble a1 = rotation + G_PI / 2 + 2 * G_PI * (i + 1) / sides;
      gdouble x0 = radius * cos (a0), y0 = radius * sin (a0);
      gdouble x1 = radius * cos (a1), y1 = radius * sin (a1);
      gdouble x;

      if ((y < y0) == (y < y1))
        continue;

      x = x0 + (y - y0) * (x1 - x0) / (y1 - y0);

      if (! found)
        {
          *l = *r = x;
          found = TRUE;
        }
      else
        {
          *l = MIN (*l, x);
          *r = MAX (*r, x);
        }
    }

  return found && *r > *l;
}

static gboolean
shape_interval (DofShape  shape,
                gint      sides,
                gdouble   rotation,
                gdouble   radius,
                gdouble   y,
                gdouble  *l,
                gdouble  *r)
{
  if (shape == DOF_SHAPE_POLYGON)
    return polygon_interval (sides, rotation, radius, y, l, r);

  if (fabs (y) >= radius)
    return FALSE;

  *r = sqrt (radius * radius - y * y);
  *l = -*r;

  return TRUE;
}

/* Adds the rows of one shape to the kernel. Each row averages the shape
 * over N_SUB lines within the row's height, so that small kernels grow
 * smoothly in both directions.
 */
static gdouble
kernel_add_shape (GArray   *spans,
                  gint     *reach,
                  DofShape  shape,
                  gint      sides,
                  gdouble   rotation,
                  gdouble   radius,
                  gfloat    sign)
{
  gint    ry   = (gint) ceil (radius + 0.5);
  gdouble area = 0.0;
  gint    dy;

  for (dy = -ry; dy <= ry; dy++)
    {
      gdouble width_sum  = 0.0;
      gdouble center_sum = 0.0;
      gdouble width, center, l, r;
      Span    span;
      gint    s;

      for (s = 0; s < N_SUB; s++)
        {
          gdouble y = dy - 0.5 + (s + 0.5) / N_SUB;
          gdouble sl, sr;

          if (shape_interval (shape, sides, rotation, radius, y, &sl, &sr))
            {
              width_sum  += sr - sl;
              center_sum += (sr + sl) / 2 * (sr - sl);
            }
        }

      if (width_sum <= 0.0)
        continue;

      width  = width_sum / N_SUB;
      center = center_sum / width_sum;
      l      = center - width / 2;
      r      = center + width / 2;

      span.dy   = dy;
      span.il   = (gint) floor (l + 0.5);
      span.ir   = (gint) floor (r + 0.5);
      span.fl   = l + 0.5 - span.il;
      span.fr   = r + 0.5 - span.ir;
      span.sign = sign;
      g_array_append_val (spans, span);

      area   += sign * width;
      *reach  = MAX (*reach, MAX (ABS (dy), MAX (ABS (span.il), ABS (span.ir)) + 1));
    }

  return area;
}

/* Returns NULL for a radius below half a pixel: no blur. */
static Kernel *
kernel_new (DofShape shape,
            gint     sides,
            gdouble  rotation,
            gdouble  thickness,
            gdouble  radius)
{
  Kernel  *kernel;
  GArray  *spans;
  gdouble  area;
  gint     reach = 0;

  if (radius < 0.5)
    return NULL;

  spans = g_array_new (FALSE, FALSE, sizeof (Span));

  if (shape == DOF_SHAPE_RING)
    {
      area = kernel_add_shape (spans, &reach, DOF_SHAPE_CIRCLE, 0, 0.0,
                               radius, 1.0f);
      if (thickness < 1.0 && radius * (1.0 - thickness) >= 0.5)
        area += kernel_add_shape (spans, &reach, DOF_SHAPE_CIRCLE, 0, 0.0,
                                  radius * (1.0 - thickness), -1.0f);
    }
  else
    {
      area = kernel_add_shape (spans, &reach, shape, sides, rotation,
                               radius, 1.0f);
    }

  kernel          = g_new0 (Kernel, 1);
  kernel->n_spans = spans->len;
  kernel->spans   = (Span *) g_array_free (spans, FALSE);
  kernel->reach   = reach;
  kernel->norm    = area > 0.0 ? 1.0 / area : 0.0;

  return kernel;
}

static void
kernel_free (Kernel *kernel)
{
  if (kernel)
    {
      g_free (kernel->spans);
      g_free (kernel);
    }
}

typedef struct
{
  gint x, y, width, height;
} Rect;

static gboolean
rect_intersect (Rect *dest, const Rect *a, const Rect *b)
{
  gint x1 = MAX (a->x, b->x);
  gint y1 = MAX (a->y, b->y);
  gint x2 = MIN (a->x + a->width,  b->x + b->width);
  gint y2 = MIN (a->y + a->height, b->y + b->height);

  dest->x      = x1;
  dest->y      = y1;
  dest->width  = MAX (x2 - x1, 0);
  dest->height = MAX (y2 - y1, 0);

  return dest->width > 0 && dest->height > 0;
}

/* Blurs src (nch channels, sw x sh pixels at offset sx, sy) with the
 * kernel into dst over the rectangle out (dst has out's size). Every
 * pixel of out must lie at least kernel->reach pixels inside src. The
 * last channel is computed first; where it is zero, all channels are.
 */
static void
blur_rect (const gfloat *src,
           gint          nch,
           const Rect   *s,
           gdouble      *prefix,
           const Kernel *kernel,
           const Rect   *out,
           gfloat       *dst)
{
  const gint pw    = s->width + 1;
  const gint reach = kernel->reach;
  gint       y0    = MAX (out->y - reach, s->y);
  gint       y1    = MIN (out->y + out->height + reach, s->y + s->height);
  gint       cx0   = MAX (out->x - s->x - reach, 0);
  gint       cx1   = MIN (out->x + out->width - s->x + reach, s->width);
  gint       x, y, c, i;

  /* prefix sums of the rows that the kernel reaches */
  for (y = y0; y < y1; y++)
    {
      const gfloat *row = src + (gsize) (y - s->y) * s->width * nch;
      gdouble      *p   = prefix + (gsize) (y - s->y) * pw * nch;
      gdouble       sum[SLICE_CH] = { 0.0, };

      /* only the columns that the kernel reads; sums of spans do not
       * depend on where the prefix sums start */
      for (c = 0; c < nch; c++)
        p[cx0 * nch + c] = 0.0;

      for (x = cx0; x < cx1; x++)
        for (c = 0; c < nch; c++)
          {
            sum[c] += row[x * nch + c];
            p[(x + 1) * nch + c] = sum[c];
          }
    }

  for (y = out->y; y < out->y + out->height; y++)
    {
      gfloat *d = dst + (gsize) (y - out->y) * out->width * nch;

      for (x = out->x; x < out->x + out->width; x++, d += nch)
        {
          const gint px   = x - s->x;
          const gint last = nch - 1;
          gdouble    acc;

          /* the last channel alone first */
          acc = 0.0;
          for (i = 0; i < kernel->n_spans; i++)
            {
              const Span   *sp = &kernel->spans[i];
              const gdouble *p  = prefix + (gsize) (y + sp->dy - s->y) * pw * nch;
              const gdouble *pl = p + (px + sp->il) * nch + last;
              const gdouble *pr = p + (px + sp->ir) * nch + last;
              gdouble        cl = pl[0] + sp->fl * (pl[nch] - pl[0]);
              gdouble        cr = pr[0] + sp->fr * (pr[nch] - pr[0]);

              acc += sp->sign * (cr - cl);
            }
          acc *= kernel->norm;

          if (fabs (acc) < EPSILON)
            {
              for (c = 0; c < nch; c++)
                d[c] = 0.0f;
              continue;
            }

          {
            gdouble sum[SLICE_CH] = { 0.0, };

            d[last] = acc;

          for (i = 0; i < kernel->n_spans; i++)
            {
              const Span   *sp = &kernel->spans[i];
              const gdouble *p  = prefix + (gsize) (y + sp->dy - s->y) * pw * nch;
              const gdouble *pl = p + (px + sp->il) * nch;
              const gdouble *pr = p + (px + sp->ir) * nch;

              for (c = 0; c < last; c++)
                {
                  gdouble cl = pl[c] + sp->fl * (pl[nch + c] - pl[c]);
                  gdouble cr = pr[c] + sp->fr * (pr[nch + c] - pr[c]);

                  sum[c] += sp->sign * (cr - cl);
                }
            }

          for (c = 0; c < last; c++)
            d[c] = sum[c] * kernel->norm;
          }
        }
    }
}

typedef struct
{
  gint    n_slices;
  gint    margin;
  Kernel *kernels[MAX_SLICES];   /* blur of each slice */
  Kernel *fill[MAX_SLICES];      /* round kernel for filling behind */
} Setup;

static void
setup_free (Setup *setup)
{
  gint k;

  for (k = 0; k < setup->n_slices; k++)
    {
      kernel_free (setup->kernels[k]);
      kernel_free (setup->fill[k]);
    }
  g_free (setup);
}

/* depth measured from near (0) to far (1) */
static inline gfloat
to_distance (const GeglProperties *o,
             gfloat                value)
{
  value = CLAMP (value, 0.0f, 1.0f);

  return o->white_is_near ? 1.0f - value : value;
}

static gint
get_margin (const GeglProperties *o)
{
  /* the largest kernel is at most radius + 2 pixels from its center */
  return (gint) ceil (o->radius) + 3;
}

static Setup *
setup_new (const GeglProperties *o)
{
  Setup  *setup = g_new0 (Setup, 1);
  gfloat  focus = to_distance (o, o->focus);
  gint    k;

  /* neighboring slices differ by at most a pixel of blur */
  setup->n_slices = CLAMP ((gint) ceil (o->radius) + 2, 2, MAX_SLICES);
  setup->margin   = get_margin (o);

  for (k = 0; k < setup->n_slices; k++)
    {
      gdouble t      = (gdouble) k / (setup->n_slices - 1);
      gdouble radius = o->radius * MAX (fabs (t - focus) - o->focus_range, 0.0);

      setup->kernels[k] = kernel_new (o->shape, o->blades,
                                      o->rotation * G_PI / 180.0,
                                      o->ring_thickness, radius);
      if (radius >= 1.0)
        setup->fill[k] = kernel_new (DOF_SHAPE_CIRCLE, 0, 0.0, 1.0, radius);

      g_assert (! setup->kernels[k] || setup->kernels[k]->reach < setup->margin);
      g_assert (! setup->fill[k]    || setup->fill[k]->reach    < setup->margin);
    }

  return setup;
}

static void
prepare (GeglOperation *operation)
{
  const Babl *space  = gegl_operation_get_source_space (operation, "input");
  const Babl *format = babl_format_with_space ("RaGaBaA float", space);

  gegl_operation_set_format (operation, "input",  format);
  gegl_operation_set_format (operation, "output", format);
  /* a depth map holds values, not light: read them as they are stored */
  gegl_operation_set_format (operation, "aux",
                             babl_format_with_space ("Y' float",
                               gegl_operation_get_source_space (operation,
                                                                "aux")));
}

static GeglRectangle
get_bounding_box (GeglOperation *operation)
{
  const GeglRectangle *in_rect =
    gegl_operation_source_get_bounding_box (operation, "input");

  if (in_rect)
    return *in_rect;

  return (GeglRectangle) { 0, 0, 0, 0 };
}

/* The output needs the slices blurred within a margin around it (to fill
 * behind near things), and those need the input within a second margin.
 */
static GeglRectangle
get_required_for_output (GeglOperation       *operation,
                         const gchar         *input_pad,
                         const GeglRectangle *roi)
{
  GeglProperties *o      = GEGL_PROPERTIES (operation);
  GeglRectangle   result = *roi;
  gint            m      = 2 * get_margin (o);

  result.x      -= m;
  result.y      -= m;
  result.width  += 2 * m;
  result.height += 2 * m;

  return result;
}

static GeglRectangle
get_invalidated_by_change (GeglOperation       *operation,
                           const gchar         *input_pad,
                           const GeglRectangle *roi)
{
  return get_required_for_output (operation, input_pad, roi);
}

static void
process_tile (GeglOperation *operation,
              const Setup   *setup,
              GeglBuffer    *input,
              GeglBuffer    *aux,
              GeglBuffer    *output,
              const Rect    *tile)
{
  GeglProperties *o       = GEGL_PROPERTIES (operation);
  const Babl     *format  = gegl_operation_get_format (operation, "output");
  const Babl     *aux_fmt = gegl_operation_get_format (operation, "aux");
  const gint      m       = setup->margin;
  const gint      ns      = setup->n_slices;
  Rect            work    = { tile->x - m, tile->y - m,
                              tile->width + 2 * m, tile->height + 2 * m };
  Rect            reg     = { work.x - m, work.y - m,
                              work.width + 2 * m, work.height + 2 * m };
  const gsize     n_reg   = (gsize) reg.width * reg.height;
  const gsize     n_work  = (gsize) work.width * work.height;
  gfloat         *in      = g_new (gfloat, n_reg * 4);
  gfloat         *pos     = g_new (gfloat, n_reg);
  guint8         *index   = g_new (guint8, n_reg);
  gint            box[MAX_SLICES][4];
  gfloat         *weight  = g_new (gfloat, n_reg);
  gfloat         *slice   = g_new (gfloat, n_reg * SLICE_CH);
  gdouble        *prefix  = g_new (gdouble, (gsize) (reg.width + 1) * reg.height * SLICE_CH);
  gfloat         *acc     = g_new0 (gfloat, n_work * ACC_CH);
  gfloat         *tmp     = g_new (gfloat, n_work * SLICE_CH);
  gfloat          hl_gain = exp2f (10.0f * o->highlight_factor) - 1.0f;
  gfloat          lo      = o->highlight_threshold_low;
  gfloat          hi      = o->highlight_threshold_high;
  gfloat         *out;
  gsize           i;
  gint            k, x, y, c;

  gegl_buffer_get (input,
                   GEGL_RECTANGLE (reg.x, reg.y, reg.width, reg.height),
                   1.0, format, in, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_CLAMP);

  if (aux)
    gegl_buffer_get (aux,
                     GEGL_RECTANGLE (reg.x, reg.y, reg.width, reg.height),
                     1.0, aux_fmt, pos, GEGL_AUTO_ROWSTRIDE,
                     GEGL_ABYSS_CLAMP);
  else
    memset (pos, 0, n_reg * sizeof (gfloat));

  for (i = 0; i < n_reg; i++)
    {
      const gfloat *p = in + i * 4;

      /* each pixel belongs to the nearest slice: splitting it between two
       * would make a sharp surface partly transparent */
      index[i] = (guint8) lrintf (to_distance (o, pos[i]) * (ns - 1));

      weight[i] = 1.0f;
      if (hl_gain > 0.0f)
        {
          gfloat luma = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
          gfloat s;

          if (hi > lo)
            s = CLAMP ((luma - lo) / (hi - lo), 0.0f, 1.0f);
          else
            s = luma >= lo ? 1.0f : 0.0f;

          weight[i] += hl_gain * s * s * (3.0f - 2.0f * s);
        }
    }

  for (k = 0; k < ns; k++)
    {
      box[k][0] = box[k][1] = G_MAXINT;
      box[k][2] = box[k][3] = G_MININT;
    }

  for (y = 0; y < reg.height; y++)
    for (x = 0; x < reg.width; x++)
      {
        gint *b = box[index[(gsize) y * reg.width + x]];

        b[0] = MIN (b[0], x);
        b[1] = MIN (b[1], y);
        b[2] = MAX (b[2], x);
        b[3] = MAX (b[3], y);
      }

  memset (slice, 0, n_reg * SLICE_CH * sizeof (gfloat));

  /* from far to near, each slice goes over what lies behind it */
  for (k = ns - 1; k >= 0; k--)
    {
      const Kernel *kernel = setup->kernels[k];
      const Kernel *fill   = setup->fill[k];
      gint          bx0 = box[k][0], by0 = box[k][1];
      gint          bx1 = box[k][2], by1 = box[k][3];
      Rect          bbox, area;
      gint          reach;

      if (bx1 < bx0)
        continue;

      reach       = kernel ? kernel->reach : 0;
      bbox.x      = reg.x + bx0 - reach;
      bbox.y      = reg.y + by0 - reach;
      bbox.width  = bx1 - bx0 + 1 + 2 * reach;
      bbox.height = by1 - by0 + 1 + 2 * reach;

      if (! rect_intersect (&area, &bbox, &work))
        continue;

      for (y = by0; y <= by1; y++)
        for (x = bx0; x <= bx1; x++)
          {
            gsize   j = (gsize) y * reg.width + x;
            gfloat *d = slice + j * SLICE_CH;

            if (index[j] == k)
              {
                for (c = 0; c < 4; c++)
                  d[c] = weight[j] * in[j * 4 + c];
                d[4] = weight[j];
                d[5] = 1.0f;
              }
          }

      /* Fill behind: where nearer slices left holes in what lies behind,
       * spread what lies behind into them, so that the blurred edge of
       * this slice shows it rather than nothing.
       */
      if (fill)
        {
          Rect inner = { work.x + fill->reach, work.y + fill->reach,
                         work.width  - 2 * fill->reach,
                         work.height - 2 * fill->reach };
          Rect farea;

          if (rect_intersect (&farea, &area, &inner))
            {
              blur_rect (acc, ACC_CH, &work, prefix, fill, &farea, tmp);

              for (y = 0; y < farea.height; y++)
                for (x = 0; x < farea.width; x++)
                  {
                    gfloat *f = tmp + ((gsize) y * farea.width + x) * ACC_CH;
                    gfloat *a = acc + ((gsize) (farea.y - work.y + y) * work.width
                                       + (farea.x - work.x + x)) * ACC_CH;
                    gfloat  hole = 1.0f - a[4];

                    if (hole > EPSILON && f[4] > EPSILON)
                      {
                        for (c = 0; c < 4; c++)
                          a[c] += hole * f[c] / f[4];
                        a[4] = 1.0f;
                      }
                  }
            }
        }

      if (kernel)
        {
          blur_rect (slice, SLICE_CH, &reg, prefix, kernel, &area, tmp);
        }
      else
        {
          for (y = 0; y < area.height; y++)
            memcpy (tmp + (gsize) y * area.width * SLICE_CH,
                    slice + ((gsize) (area.y - reg.y + y) * reg.width
                             + (area.x - reg.x)) * SLICE_CH,
                    (gsize) area.width * SLICE_CH * sizeof (gfloat));
        }

      for (y = 0; y < area.height; y++)
        for (x = 0; x < area.width; x++)
          {
            gfloat *s = tmp + ((gsize) y * area.width + x) * SLICE_CH;
            gfloat *a = acc + ((gsize) (area.y - work.y + y) * work.width
                               + (area.x - work.x + x)) * ACC_CH;
            gfloat  cover;

            if (s[5] <= EPSILON || s[4] <= EPSILON)
              continue;

            cover = MIN (s[5], 1.0f);

            for (c = 0; c < 4; c++)
              a[c] = cover * s[c] / s[4] + (1.0f - cover) * a[c];
            a[4] = cover + (1.0f - cover) * a[4];
          }

      for (y = by0; y <= by1; y++)
        memset (slice + ((gsize) y * reg.width + bx0) * SLICE_CH, 0,
                (gsize) (bx1 - bx0 + 1) * SLICE_CH * sizeof (gfloat));
    }

  /* whatever is left uncovered takes what covers it partly */
  out = g_new (gfloat, (gsize) tile->width * tile->height * 4);

  for (y = 0; y < tile->height; y++)
    for (x = 0; x < tile->width; x++)
      {
        const gfloat *a = acc + ((gsize) (y + m) * work.width + (x + m)) * ACC_CH;
        gfloat       *d = out + ((gsize) y * tile->width + x) * 4;

        if (a[4] > EPSILON)
          for (c = 0; c < 4; c++)
            d[c] = a[c] / a[4];
        else
          for (c = 0; c < 4; c++)
            d[c] = 0.0f;
      }

  gegl_buffer_set (output,
                   GEGL_RECTANGLE (tile->x, tile->y, tile->width, tile->height),
                   0, format, out, GEGL_AUTO_ROWSTRIDE);

  g_free (out);
  g_free (tmp);
  g_free (acc);
  g_free (prefix);
  g_free (slice);
  g_free (weight);
  g_free (index);
  g_free (pos);
  g_free (in);
}

static gboolean
process (GeglOperation       *operation,
         GeglBuffer          *input,
         GeglBuffer          *aux,
         GeglBuffer          *output,
         const GeglRectangle *roi,
         gint                 level)
{
  GeglProperties *o     = GEGL_PROPERTIES (operation);
  Setup          *setup = setup_new (o);
  gint            size  = MAX (TILE_SIZE, 4 * setup->margin);
  gint            x, y;

  for (y = roi->y; y < roi->y + roi->height; y += size)
    for (x = roi->x; x < roi->x + roi->width; x += size)
      {
        Rect tile = { x, y,
                      MIN (size, roi->x + roi->width  - x),
                      MIN (size, roi->y + roi->height - y) };

        process_tile (operation, setup, input, aux, output, &tile);
      }

  setup_free (setup);

  return TRUE;
}

static gboolean
operation_process (GeglOperation        *operation,
                   GeglOperationContext *context,
                   const gchar          *output_prop,
                   const GeglRectangle  *roi,
                   gint                  level)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);

  if (o->radius < 0.5)
    {
      gegl_operation_context_set_object (
        context, "output",
        gegl_operation_context_get_object (context, "input"));

      return TRUE;
    }

  return GEGL_OPERATION_CLASS (gegl_op_parent_class)->process (
    operation, context, output_prop, roi, level);
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GeglOperationClass         *operation_class = GEGL_OPERATION_CLASS (klass);
  GeglOperationComposerClass *composer_class  = GEGL_OPERATION_COMPOSER_CLASS (klass);

  operation_class->prepare                   = prepare;
  operation_class->get_bounding_box          = get_bounding_box;
  operation_class->get_required_for_output   = get_required_for_output;
  operation_class->get_invalidated_by_change = get_invalidated_by_change;
  operation_class->process                   = operation_process;
  composer_class->process                    = process;

  gegl_operation_class_set_keys (operation_class,
    "name",            "dof:depth-of-field",
    "title",           _("Depth of Field"),
    "categories",      "blur",
    "description",     _("Blur an image the way a lens does, by a depth map "
                         "on the aux input: things away from the focus "
                         "depth blur, near things cover far things, and "
                         "points of light take the shape of the aperture"),
    "gimp:menu-path",  "<Image>/Filters/Blur",
    "gimp:menu-label", _("Depth of Field..."),
    NULL);
}

#endif
