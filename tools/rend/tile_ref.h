/* The compute renderer's reference on the CPU: see tile_ref.c */
#ifndef TILE_REF_H
#define TILE_REF_H

#ifdef __cplusplus
extern "C" {
#endif

struct tile_ref_stats
{
   unsigned dropped;       /* triangles left out, a vertex behind the eye */
   unsigned layers_most;   /* the most translucent fragments of a sample */
};

/* The frame's words (PVRC_F_*) drawn at @samples a pixel (1, 2, 4 or 8),
 * in test mode: the picture into @out, RGBA8, width x height of the frame;
 * @last the picture it starts from where the frame says so (or NULL).
 * 0 if there was not the memory. */
int tile_ref(const unsigned *frame, unsigned samples, const unsigned *last, unsigned *out, struct tile_ref_stats *st);

#ifdef __cplusplus
}
#endif

#endif
