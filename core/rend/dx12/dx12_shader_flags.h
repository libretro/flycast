/* What the Direct3D 12 renderer's shaders and the renderer share: the
 * polygon's bits, which its pixel shader reads from the root constants, and
 * where the textures it always reads are in the bindless table. Plain
 * #defines, read by the C++ and by the HLSL (shaders/dx12_raster.hlsl, made
 * into DXBC by tools/rend/kernels_dx12.sh with this file in front). */
#ifndef REND_DX12_SHADER_FLAGS_H
#define REND_DX12_SHADER_FLAGS_H

/* A polygon's pixel shader: what the Direct3D 11 renderer made a shader of
 * its own for (DX11FragmentShaderParams), as bits */
#define DX12_PF_ALPHA_TEST       0x0001u	/* punch-through: tested on the finished alpha */
#define DX12_PF_CLIP_INSIDE      0x0002u	/* the tile clip's inside is not drawn */
#define DX12_PF_USE_ALPHA        0x0004u
#define DX12_PF_TEXTURE          0x0008u
#define DX12_PF_IGNORE_TEX_A     0x0010u
#define DX12_PF_SHAD_INSTR_SHIFT 5			/* two bits: the TSP's shading instruction */
#define DX12_PF_OFFSET           0x0080u
#define DX12_PF_FOG_SHIFT        8			/* two bits: the TSP's fog control */
#define DX12_PF_BUMPMAP          0x0400u
#define DX12_PF_CLAMP            0x0800u
#define DX12_PF_TRILINEAR        0x1000u
#define DX12_PF_PALETTE          0x2000u
#define DX12_PF_SHADOWED         0x4000u	/* drawn again where it is in a modifier volume */

/* The quad's: its picture sampled with the linear sampler, not the point one */
#define DX12_QF_LINEAR           0x0001u

/* The per-pixel renderer's second word of bits (oit/dx12_oitrenderer.cpp):
 * the polygon's blend (8 when the pass that puts the lists together is
 * to decide), and its second volume's bits, where the first volume's are
 * the DX12_PF_ ones above */
#define DX12_OF_SRC_SHIFT        0			/* four bits: the first volume's source blend, 8 for any */
#define DX12_OF_DST_SHIFT        4			/* four bits: its destination blend */
#define DX12_OF_SRC1_SHIFT       8			/* four bits: the second volume's */
#define DX12_OF_DST1_SHIFT       12
#define DX12_OF_SHAD_INSTR1_SHIFT 16		/* two bits */
#define DX12_OF_FOG1_SHIFT       18			/* two bits */
#define DX12_OF_USE_ALPHA1       0x00100000u
#define DX12_OF_IGNORE_TEX_A1    0x00200000u
#define DX12_OF_TWO_VOLUMES      0x00400000u
/* The translucent modifier volumes' passes (the same word): rend/shadows.h's modes */
#define DX12_OF_MV_XOR           0
#define DX12_OF_MV_OR            1
#define DX12_OF_MV_INCLUSION     2
#define DX12_OF_MV_EXCLUSION     3
/* The per-pixel passes, a pixel shader each (DX12_OIT_PASS when compiled) */
#define DX12_PASS_DEPTH          0
#define DX12_PASS_COLOR          1
#define DX12_PASS_OIT            2
/* How many of a pixel's translucent pixels are sorted at most */
#define DX12_OIT_MAX_PIXELS      32

/* The bindless table (t0 to DX12_SRV_TABLE_SIZE - 1, space 1): the fog
 * table, the palette and a view of nothing (sampled as zero) at these
 * places, everything else after them */
#define DX12_SRV_FOG             0
#define DX12_SRV_PALETTE         1
#define DX12_SRV_NONE            2
#define DX12_SRV_FIRST_FREE      3
#define DX12_SRV_TABLE_SIZE      16384
/* The per-pixel renderer's table (DX12Context::OitFirst): the lists (u1), their
 * heads (u2), the counter (u3), the stencil (t4) and the depth (t6) */
#define DX12_OIT_TABLE_SIZE      5
/* The polygons' samplers (s0 to DX12_SAMPLER_TABLE_SIZE - 1, space 1), by
 * the TSP's MipMapD, filtering, clamp and flip bits, ten of them
 * (dx12_sampler_index() in dx12_texture.h) */
#define DX12_SAMPLER_TABLE_SIZE  1024

#endif
