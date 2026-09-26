/*
 * How long depth:blur takes when GEGL asks for the image in pieces
 *
 * bench-pieces.c
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
 * Renders a flat 1024x1024 image at once and in pieces of 256 and 64
 * pixels, each piece in a graph of its own (no cache), as GIMP's chunks
 * would. Run it with GEGL_PATH set to a folder with only the module and
 * GEGL's own folder (see tests/run.sh):
 *
 *   ninja -C build bench-pieces
 *   GEGL_PATH=... build/bench-pieces
 */

#include <gegl.h>
#include <stdio.h>

#define SIZE 1024

int
main (int    argc,
      char **argv)
{
  static gfloat pixels[SIZE * SIZE * 4];
  gdouble       radii[]  = { 20.0, 60.0 };
  gint          pieces[] = { SIZE, 256, 64 };
  gfloat        color[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
  gfloat        depth    = 0.9f;
  GeglBuffer   *in, *aux;
  guint         k, p;

  gegl_init (&argc, &argv);

  if (! gegl_has_operation ("depth:blur"))
    {
      fprintf (stderr, "depth:blur is not in GEGL_PATH\n");
      return 2;
    }

  in  = gegl_buffer_new (GEGL_RECTANGLE (0, 0, SIZE, SIZE),
                         babl_format ("RGBA float"));
  aux = gegl_buffer_new (GEGL_RECTANGLE (0, 0, SIZE, SIZE),
                         babl_format ("Y' float"));
  gegl_buffer_set_color_from_pixel (in, NULL, color,
                                    babl_format ("RGBA float"));
  gegl_buffer_set_color_from_pixel (aux, NULL, &depth,
                                    babl_format ("Y' float"));

  for (k = 0; k < G_N_ELEMENTS (radii); k++)
    for (p = 0; p < G_N_ELEMENTS (pieces); p++)
      {
        gint   step = pieces[p];
        gint64 t0   = g_get_monotonic_time ();
        gint   x, y;

        for (y = 0; y < SIZE; y += step)
          for (x = 0; x < SIZE; x += step)
            {
              GeglNode *graph = gegl_node_new ();
              GeglNode *src   = gegl_node_new_child (graph,
                                  "operation", "gegl:buffer-source",
                                  "buffer", in, NULL);
              GeglNode *dsrc  = gegl_node_new_child (graph,
                                  "operation", "gegl:buffer-source",
                                  "buffer", aux, NULL);
              GeglNode *blur  = gegl_node_new_child (graph,
                                  "operation", "depth:blur",
                                  "radius", radii[k], "focus", 0.0, NULL);

              gegl_node_connect (src, "output", blur, "input");
              gegl_node_connect (dsrc, "output", blur, "aux");
              gegl_node_blit (blur, 1.0, GEGL_RECTANGLE (x, y, step, step),
                              babl_format ("RaGaBaA float"), pixels,
                              GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
              g_object_unref (graph);
            }

        printf ("radius %3g, pieces of %4d: %.2f s\n", radii[k], step,
                (g_get_monotonic_time () - t0) / 1e6);
      }

  g_object_unref (in);
  g_object_unref (aux);
  gegl_exit ();

  return 0;
}
