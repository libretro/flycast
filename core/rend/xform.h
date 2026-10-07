#ifndef REND_XFORM_H
#define REND_XFORM_H

#include <retro_inline.h>

/* The renderers' transforms.
 *
 * Every one of them - guest coordinates to clip space, to the viewport, to
 * the scissor rectangle - is a scale and then a move along each axis, and
 * never anything else: no rotation, no shear, no perspective. They used to
 * be 4x4 matrices from a general matrix library, built by multiplying 4x4
 * matrices together, sixty-four multiplications a time, to get the six
 * numbers that are not 0 or 1.
 *
 * Here a transform is those six numbers. Putting two together is three
 * multiplications and three multiply-adds; taking a point through one is
 * a multiply-add an axis. The results are the same floats the matrices
 * held: what was left out is multiplying by 1 and adding 0. A shader still
 * wants a 4x4 matrix and xform_to_mat4() writes one out. */
typedef struct xform
{
   float sx, sy, sz;    /* the scale */
   float tx, ty, tz;    /* and then the move */
} xform;

typedef struct vec2f
{
   float x, y;
} vec2f;

static INLINE void xform_set(xform *m, float sx, float sy, float sz, float tx, float ty, float tz)
{
   m->sx = sx;
   m->sy = sy;
   m->sz = sz;
   m->tx = tx;
   m->ty = ty;
   m->tz = tz;
}

/* @out is @b and then @a: what the matrix product a * b was. @out may be
 * either of them. */
static INLINE void xform_mul(xform *out, const xform *a, const xform *b)
{
   const float tx = a->sx * b->tx + a->tx;
   const float ty = a->sy * b->ty + a->ty;
   const float tz = a->sz * b->tz + a->tz;

   out->sx = a->sx * b->sx;
   out->sy = a->sy * b->sy;
   out->sz = a->sz * b->sz;
   out->tx = tx;
   out->ty = ty;
   out->tz = tz;
}

/* Where a point's x and y end up */
static INLINE float xform_x(const xform *m, float x)
{
   return m->sx * x + m->tx;
}

static INLINE float xform_y(const xform *m, float y)
{
   return m->sy * y + m->ty;
}

/* ...and a width's and a height's: scaled, not moved */
static INLINE float xform_w(const xform *m, float w)
{
   return m->sx * w;
}

static INLINE float xform_h(const xform *m, float h)
{
   return m->sy * h;
}

/* As the 4x4 matrix a shader takes, a column after the other */
static INLINE void xform_to_mat4(const xform *m, float *mat)
{
   mat[0]  = m->sx; mat[1]  = 0.0f;  mat[2]  = 0.0f;  mat[3]  = 0.0f;
   mat[4]  = 0.0f;  mat[5]  = m->sy; mat[6]  = 0.0f;  mat[7]  = 0.0f;
   mat[8]  = 0.0f;  mat[9]  = 0.0f;  mat[10] = m->sz; mat[11] = 0.0f;
   mat[12] = m->tx; mat[13] = m->ty; mat[14] = m->tz; mat[15] = 1.0f;
}

#endif
