/* A texture for texdecode_test.c to make both ways: its control words'
 * fields and the registers it is made with */
#ifndef TEXDECODE_CASE_H
#define TEXDECODE_CASE_H

#include <stddef.h>

struct texd_case
{
   unsigned addr;          /* in video memory, a multiple of 8 */
   unsigned fmt;           /* PixelFmt, 0..7 */
   unsigned scan_order, vq, mip, stride_sel, pal_select;
   unsigned tex_u, tex_v, filter;
   unsigned force32;       /* the renderer has no 16-bit formats */
   unsigned lod0;          /* Force Texture LOD0 */
   unsigned text_control;  /* the stride, in its low 5 bits */
   unsigned pal_ctrl;      /* the palette's format, 0..3 */
};

#ifdef __cplusplus
extern "C" {
#endif
struct texd_plan;
void texd_cpu_init(void *video_memory);
/* The texture made on the CPU into @out (@cap bytes): *@bytes of texels,
 * *@type its TextureType; and nonzero with *@plan what a renderer that
 * converts on the GPU would have been handed */
int texd_cpu_run(const struct texd_case *c, struct texd_plan *plan, void *out, size_t cap, size_t *bytes, int *type);
void *texd_cpu_palette_ram(void);
#ifdef __cplusplus
}
#endif

#endif
