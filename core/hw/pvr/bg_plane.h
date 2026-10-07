#ifndef BG_PLANE_H
#define BG_PLANE_H

/* The background plane.
 *
 * The chip is given three vertices for it, and what it draws is not the
 * triangle they make but the plane through them, over the whole screen:
 * every colour and texture coordinate is what the three vertices say it
 * is where they are, and goes on at the same rate everywhere else. Most
 * games put the three in three corners of the screen, and then the plane
 * is the picture stretched over the screen; a game that puts them
 * elsewhere, or the other way round, gets what it asked for.
 *
 * bg_plane_init() takes the three positions and says whether they are a
 * plane at all (not all on one line); bg_plane_at() gives the value at a
 * point of something whose values at the three vertices are known. */

typedef struct bg_plane
{
   float x0, y0;        /* the first vertex */
   float e1x, e1y;      /* from it to the second */
   float e2x, e2y;      /* and to the third */
   float inv_det;
} bg_plane;

static int bg_plane_init(bg_plane *p, float x0, float y0, float x1, float y1, float x2, float y2)
{
   float det;

   p->x0  = x0;
   p->y0  = y0;
   p->e1x = x1 - x0;
   p->e1y = y1 - y0;
   p->e2x = x2 - x0;
   p->e2y = y2 - y0;
   det    = p->e1x * p->e2y - p->e2x * p->e1y;
   /* twice the triangle's area, in pixels. Nothing, or not a number, or
    * not finite: no plane. */
   if (!((det > 1.0f && det < 1e12f) || (det < -1.0f && det > -1e12f)))
      return 0;
   p->inv_det = 1.0f / det;
   return 1;
}

static float bg_plane_at(const bg_plane *p, float a0, float a1, float a2, float x, float y)
{
   const float d1   = a1 - a0;
   const float d2   = a2 - a0;
   const float dadx = (d1 * p->e2y - d2 * p->e1y) * p->inv_det;
   const float dady = (d2 * p->e1x - d1 * p->e2x) * p->inv_det;

   return a0 + dadx * (x - p->x0) + dady * (y - p->y0);
}

/* A colour component there, as the eight bits a vertex carries. */
static unsigned char bg_plane_colour(const bg_plane *p, unsigned a0, unsigned a1, unsigned a2, float x, float y)
{
   const float v = bg_plane_at(p, (float)a0, (float)a1, (float)a2, x, y);

   if (!(v > 0.0f))
      return 0;
   if (v >= 255.0f)
      return 255;
   return (unsigned char)(v + 0.5f);
}

#endif
