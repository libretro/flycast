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

/* The bindless table (t0 to DX12_SRV_TABLE_SIZE - 1, space 1): the fog
 * table and the palette at these places, everything else after them */
#define DX12_SRV_FOG             0
#define DX12_SRV_PALETTE         1
#define DX12_SRV_FIRST_FREE      2
#define DX12_SRV_TABLE_SIZE      8192
/* The polygons' samplers (s0 to DX12_SAMPLER_TABLE_SIZE - 1, space 1), by
 * the TSP's MipMapD, filtering, clamp and flip bits (DX12Samplers::Index()) */
#define DX12_SAMPLER_TABLE_SIZE  512

#endif
