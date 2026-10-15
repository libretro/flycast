/* rend/ta_gpu.h as text, for the kernels: made by tools/pvr/ta_gpu_test.sh update from it. Not to be edited. */
R"TAG_TEXT(
/* The tile accelerator's vertices and indices made on the GPU (GPU-side
 * TA processing, its decode): from a frame's 32-byte blocks as the game
 * sent them, and the tables the CPU's header pass (ta_gpu_prepare(),
 * hw/pvr/ta_vtx.cpp) makes of its parameters, the very Vertex, index and
 * modifier volume triangle buffers the CPU's full parse makes
 * (ta_parse_vdrc()), byte for byte, and the polygon parameters' index
 * ranges make_index() gives them.
 *
 * The header is read two ways, like rend/texdecode.h:
 * - by a C89 or C++ compiler (nothing defined), for the tests, which run
 *   the same steps one after the other (tools/pvr/ta_gpu_test);
 * - by a GLSL compiler, with TAG_SHADER_GLSL defined, and TAG_KERNEL the
 *   number of the kernel wanted (TAG_K_*): the core has the text in
 *   ta_gpu_text.inc, which tools/pvr/ta_gpu_test.sh makes and checks.
 * So it is written in what the two have in common: functions of unsigned
 * and signed 32-bit integers by value, and small structures. Where words
 * are read and written is the reader's business: TAG_TA(i) is the i-th
 * word of the frame's data, TAG_TAB(i) of the tables, TAG_VFLAG(p) of the
 * vertex flags; the C side has them in arrays (tag_c_*).
 *
 * The decode, in the order of its kernels:
 *  1 unpack     one thread a vertex (and one a modifier volume triangle):
 *               its run gives its block and header, its header the format
 *               and the face colours; written as the parse writes it.
 *               Sprites are solved for their fourth corner in integer
 *               arithmetic (tag_fadd() and the others: IEEE single
 *               precision, round to nearest, denormals, and the x86 rules
 *               for NaNs, as the CPU's code is compiled), so that every
 *               GPU gives the CPU's bits. Each vertex leaves its flags
 *               (kept, end of strip) at its place in index order.
 *  2-4 strips   the ends of strips, counted by a prefix sum over those
 *               flags: where every strip of the frame is, in index order.
 *  5 summary    one thread a strip: what make_index() gives it in each of
 *               the three ways it can come to it (a group of its own; or
 *               joined to the one before, after an even or an odd number
 *               of indices).
 *  6-8 groups   a prefix scan of those, whose element is a function of
 *               how a strip comes in (tag_elem, tag_combine()): where
 *               every strip's indices go, how it comes in, and the head
 *               of its group.
 *  9 emit       one thread a strip: its indices, and its record.
 *  10 finish    the indirect draw commands of the draw groups.
 *  11 bleeding  the texture bleeding fix, one workgroup a group.
 *  12 trdraw    one thread a strip: a translucent strip's record copied to
 *               its draw group's place in the translucent draw records
 *               (TAG_H_TRB), with the group's count, for an indirect draw
 *               of the group; and its polygon parameter's words for the
 *               per-pixel renderer (TAG_P_WORDS), at its place in the list.
 *  13 provoke   (TAG_FLAG_PROVOKE) one workgroup a strip: a flat-shaded
 *               group's triangles get the colours of their last vertex in
 *               their first, as the Vulkan renderers draw them
 *               (BaseDrawer::SetProvokingVertices(), whose order of reads
 *               and writes it keeps).
 *  14 shadow    (TAG_T_NSHD) one workgroup a draw group the CPU names: the
 *               rectangle around the vertices its indices reach, against
 *               the part of the screen its render pass's opaque modifier
 *               volumes cover, which the CPU gives; its draw command again
 *               for its second drawing for shadows, with no indices if
 *               they do not meet (PolyOverlaps() in rend/shadows.h).
 * Nothing is read back: the counts are indirect dispatch and draw
 * arguments, and the farthest depth (fZ_max) is a word of the frame
 * buffer for the GPU to read.
 *
 * Where the two can differ: inside a strip that has not ended, the full
 * parse takes every block that follows for a vertex, whatever its control
 * word says, until one ends the strip; the tile accelerator, and the
 * control-word walk the header pass goes by, take a parameter there for a
 * parameter. The decode does what the walk does, so a frame with a global
 * parameter inside an unterminated strip comes out as the hardware has it
 * and not as the full parse does (tools/pvr/ta_gpu_test counts such frames
 * apart: their vertex counts differ). Frames the decode does not do at all
 * are left to the full parse (ta_gpu_prepare()): a list still open at the
 * end of a render pass, data ending in the middle of a parameter or a
 * vertex, more vertices or modifier volume triangles than the parse has
 * room for. */
#ifndef REND_TA_GPU_H
#define REND_TA_GPU_H

/* The kernels (TAG_KERNEL), in the order they run */
#define TAG_K_UNPACK   1
#define TAG_K_COUNT    2
#define TAG_K_COUNTSUM 3
#define TAG_K_ENDS     4
#define TAG_K_SUMMARY  5
#define TAG_K_GROUPS   6
#define TAG_K_GROUPSUM 7
#define TAG_K_STATES   8
#define TAG_K_EMIT     9
#define TAG_K_FINISH   10
#define TAG_K_BLEED    11
#define TAG_K_TRDRAW   12
#define TAG_K_PROVOKE  13
#define TAG_K_SHADOW   14
#define TAG_KERNELS    14

#if defined(TAG_SHADER_GLSL)

layout(std430, set = 0, binding = 0) readonly buffer TaData { uint w[]; } ta;
layout(std430, set = 1, binding = 0) readonly buffer Tables { uint w[]; } tab;
layout(std430, set = 1, binding = 1) buffer Verts { uint w[]; } vtx;
layout(std430, set = 1, binding = 2) buffer ModTrig { uint w[]; } mtr;
layout(std430, set = 1, binding = 3) buffer Indices { uint w[]; } idx;
layout(std430, set = 1, binding = 4) buffer Work { uint w[]; } work;
layout(std430, set = 1, binding = 5) buffer Records { uint w[]; } rec;
layout(std430, set = 1, binding = 6) buffer Commands { uint w[]; } cmd;
layout(std430, set = 1, binding = 7) buffer Frame { uint w[]; } frm;
layout(std430, set = 1, binding = 8) buffer TrDraws { uint w[]; } trd;
layout(std430, set = 1, binding = 9) buffer TrParams { uint w[]; } trp;

#define TAG_TA(i) ta.w[i]
#define TAG_TAB(i) tab.w[i]
#define TAG_VFLAG(p) work.w[tab.w[TAG_T_W_VFLAG] + (p)]
#if TAG_KERNEL == TAG_K_EMIT
#define TAG_IDX_PUT(at, v) { if ((at) < tab.w[TAG_T_ROOM_IDX]) idx.w[at] = (v); }
#endif

#define TAG_FN
#define tag_u32 uint
#define tag_s32 int
#define TAG_U32(x) uint(x)
#define TAG_S32(x) int(x)
#define TAG_STRUCT(n) struct n
#define TAG_STRUCT_END(n) ;
#define TAG_F(b) uintBitsToFloat(b)
#define TAG_B(f) floatBitsToUint(f)

#else

#include <string.h>
#include <retro_inline.h>

#define TAG_FN static INLINE
typedef unsigned int tag_u32;
typedef int tag_s32;
#define TAG_U32(x) ((tag_u32)(x))
#define TAG_S32(x) ((tag_s32)(x))
#define TAG_STRUCT(n) typedef struct n
#define TAG_STRUCT_END(n) n;

static INLINE float tag_c_f(tag_u32 b) { float f; memcpy(&f, &b, 4); return f; }
static INLINE tag_u32 tag_c_b(float f) { tag_u32 b; memcpy(&b, &f, 4); return b; }
#define TAG_F(b) tag_c_f(b)
#define TAG_B(f) tag_c_b(f)

/* Where the words are on this side (tag_c_set()) */
static const tag_u32 *tag_c_ta;
static const tag_u32 *tag_c_tab;
static tag_u32 *tag_c_vflag;
static INLINE void tag_c_set(const tag_u32 *ta, const tag_u32 *tab, tag_u32 *vflag)
{
   tag_c_ta = ta;
   tag_c_tab = tab;
   tag_c_vflag = vflag;
}
#define TAG_TA(i) tag_c_ta[i]
#define TAG_TAB(i) tag_c_tab[i]
#define TAG_VFLAG(p) tag_c_vflag[p]

#endif

/* ---- The tables (TAG_TAB), as the header pass writes them ----
 *
 * A header of 32 words, then the runs, the headers, the modifier volume
 * runs, the segments and the background's vertices, where the header
 * says. All counts and places are words or entries. */
#define TAG_T_NVERTS   0   /* vertices, the background's four included */
#define TAG_T_NRUNS    1
#define TAG_T_NHDR     2
#define TAG_T_NMVRUN   3
#define TAG_T_NMOD     4   /* modifier volume triangles */
#define TAG_T_NSEG     5   /* segments: render passes x 3 lists */
#define TAG_T_NSLOT    6   /* draw command slots */
#define TAG_T_FLAGS    7
#define TAG_T_ROOM_IDX 8   /* the CPU's room for indices; past it the frame is overrun */
#define TAG_T_ROOM_OP  9   /* ...and for polygon parameters of each list */
#define TAG_T_ROOM_PT  10
#define TAG_T_ROOM_TR  11
#define TAG_T_OFF_RUN  12
#define TAG_T_OFF_HDR  13
#define TAG_T_OFF_MVRUN 14
#define TAG_T_OFF_SEG  15
#define TAG_T_OFF_BG   16
#define TAG_T_TA_BASE  17  /* where the frame's data starts in TAG_TA */
#define TAG_T_W_VFLAG  18  /* where things are in the work buffer */
#define TAG_T_W_SEND   19
#define TAG_T_W_SSUM   20
#define TAG_T_W_SSTATE 21
#define TAG_T_W_PART   22
#define TAG_T_W_CHUNK  23
#define TAG_T_W_WGAGG  24
#define TAG_T_W_WGPRE  25
#define TAG_T_W_SLOTH  26
#define TAG_T_REC_OP   27  /* where each list's records start in the record buffer */
#define TAG_T_REC_PT   28
#define TAG_T_REC_TR   29
#define TAG_T_NPASS    30
#define TAG_T_ROOM_VTX 31  /* vertices the output buffer has room for */
#define TAG_T_OFF_SHD  32  /* the shadow table (TAG_SH_*) */
#define TAG_T_NSHD     33  /* ...its entries */
#define TAG_T_SHD_CMD  34  /* where the second commands go in the command buffer (a slot's: this plus the slot) */
#define TAG_T_WORDS    36

#define TAG_FLAG_BLEED 1u  /* the texture bleeding fix (above 480 lines) */
#define TAG_FLAG_PROVOKE 2u  /* the provoking vertex's colours (kernel 13) */
#define TAG_FLAG_INSTANCE 4u /* the translucent draw records' firstInstance: the strip's place in its list */

/* A run: vertex blocks one after the other, of one header */
#define TAG_R_BLOCK  0     /* the first, from the frame's first */
#define TAG_R_VERTEX 1     /* the first vertex they make */
#define TAG_R_COUNT  2     /* vertices */
#define TAG_R_HDR    3     /* whose (in index order) */
#define TAG_R_WORDS  4

/* A modifier volume run */
#define TAG_M_BLOCK  0
#define TAG_M_FIRST  1     /* the first triangle */
#define TAG_M_COUNT  2
#define TAG_M_WORDS  4

/* A header: a polygon parameter with vertices, in index order (render
 * pass, list, then as they came) */
#define TAG_H_VFIRST 0     /* its first vertex */
#define TAG_H_VCOUNT 1
#define TAG_H_VSTART 2     /* its first vertex's place in index order */
#define TAG_H_INFO   3     /* TAG_INFO() */
#define TAG_H_SLOT   4     /* its draw group's command, TAG_NONE for none */
#define TAG_H_STATE  5     /* 7 words: what poly_same_state() compares */
#define TAG_H_FCOL   12    /* the face colours as bytes: base, offset, base 1, offset 1 */
#define TAG_H_FF     16    /* ...and as clamped floats, red green blue: base, offset, base 1, offset 1 */
#define TAG_H_SCOL   28    /* a sprite's base and offset colours, as sent */
#define TAG_H_TRB    30    /* (translucent) its draw group's place in the translucent draw records, TAG_NONE for the others */
#define TAG_H_TRV    31    /* ...and the group's first vertex's place in index order */
#define TAG_H_WORDS  32
/* the state words, in poly_same_state()'s order */
#define TAG_S_TSP    0
#define TAG_S_TCW    1
#define TAG_S_PCW    2     /* with the single-rate bit taken out */
#define TAG_S_ISP    3
#define TAG_S_TSP1   4
#define TAG_S_TCW1   5
#define TAG_S_CLIP   6

/* The shadow table: for each render pass the part of the screen its
 * opaque modifier volumes cover, as the floats' bits (min x, min y, max x,
 * max y: ModVolBounds()), then an entry for each draw group the
 * per-triangle renderer draws again for shadows: its command's slot and
 * its render pass */
#define TAG_SH_AREA    0
#define TAG_SH_ENTRY   (TAG_MAX_PASS * 4)
#define TAG_SH(slot, pass) (TAG_U32(slot) | (TAG_U32(pass) << 24))
#define TAG_SH_SLOT(e) ((e) & 0xFFFFFFu)
#define TAG_SH_PASS(e) ((e) >> 24)

/* A segment: one list of one render pass */
#define TAG_G_VSTART 0
#define TAG_G_VEND   1
#define TAG_G_WORDS  4

#define TAG_NONE     0xFFFFFFFFu

)TAG_TEXT" R"TAG_TEXT(
/* A header's format, its vertices' size (1 or 2 blocks), its list (0
 * opaque, 1 punch-through, 2 translucent), its segment, and whether it is
 * textured */
#define TAG_INFO(fmt, vb, list, seg, tex) \
   (TAG_U32(fmt) | (TAG_U32((vb) - 1u) << 5) | (TAG_U32(list) << 6) | (TAG_U32(seg) << 8) | (TAG_U32(tex) << 16))
#define TAG_INFO_FMT(i)  ((i) & 31u)
#define TAG_INFO_VB(i)   ((((i) >> 5) & 1u) + 1u)
#define TAG_INFO_LIST(i) (((i) >> 6) & 3u)
#define TAG_INFO_SEG(i)  (((i) >> 8) & 255u)
#define TAG_INFO_TEX(i)  (((i) >> 16) & 1u)

#define TAG_FMT_SPRITE 15u
#define TAG_FMT_BG     16u

/* ---- The frame buffer (out): words the GPU leaves for the GPU ---- */
#define TAG_O_ZMAX    0    /* fZ_max's bits */
#define TAG_O_NSTRIPS 1
#define TAG_O_NIDX    2
#define TAG_O_OVERRUN 3    /* 1: indices, 2, 4, 8: the lists' polygon parameters, 16: strips */
#define TAG_O_ARGS64  4    /* indirect dispatch: a thread a strip, 64 a workgroup */
#define TAG_O_ARGSCH  7    /* ...a workgroup a 1024 strips (TAG_CH_WG x TAG_CH) */
#define TAG_O_ARGS1   10   /* ...a workgroup a strip */
#define TAG_O_PASS    16   /* each render pass's polygon parameters, as the lists have them at its end: 3 words */
#define TAG_O_SEG     48   /* each segment: TAG_OS_* */
#define TAG_O_WORDS   176
#define TAG_OS_SFIRST 0    /* its first strip */
#define TAG_OS_STRIPS 1
#define TAG_OS_LBASE  2    /* its first strip's place in its list */
#define TAG_OS_WORDS  4
#define TAG_MAX_PASS  10
#define TAG_MAX_SEG   (TAG_MAX_PASS * 3)

/* A record: a polygon parameter's index range, as make_index() leaves it,
 * in the place of a VkDrawIndexedIndirectCommand */
#define TAG_C_COUNT  0
#define TAG_C_INST   1
#define TAG_C_FIRST  2
#define TAG_C_INSTANCE 4
#define TAG_C_WORDS  5

/* The translucent draw records: for each translucent draw group, at its
 * TAG_H_TRB, a record whose first word is how many strips the group has,
 * then a record for each of them in their order, as its polygon
 * parameter's (with TAG_FLAG_INSTANCE, its place in the list for
 * firstInstance); the rest of the group's room is left as it is, which is
 * zeroes (a vkCmdDrawIndexedIndirect() of all of it draws nothing more) */

/* A translucent polygon parameter's words for the per-pixel renderer, as
 * OITDrawer::UploadMainBuffer() makes them */
#define TAG_P_WORDS  2

/* What a strip's summary and state take, in the work buffer */
#define TAG_SS_WORDS 6     /* n[3], last kept vertex | 0x80000000 if any, header, first vertex */
#define TAG_ST_WORDS 4     /* where its indices go, how it comes in, the link's vertex, its group's head */
#define TAG_E_WORDS  14    /* a scan element (tag_elem) */

/* The vertex flags: kept (not infinite), the end of a strip, the vertex */
#define TAG_VF_KEPT  1u
#define TAG_VF_EOS   2u
#define TAG_VF(v, kept, eos) ((TAG_U32(v) << 2) | (TAG_U32(eos) << 1) | TAG_U32(kept))

/* The scans' shapes */
#define TAG_SCAN_WG  256   /* threads of the strip count's workgroups */
#define TAG_SCAN_PER 4     /* ...and elements a thread */
#define TAG_SCAN_N   (TAG_SCAN_WG * TAG_SCAN_PER)
#define TAG_CH_WG    128   /* threads of the group scan's workgroups */
#define TAG_CH       8     /* ...and strips a thread */
#define TAG_CH_N     (TAG_CH_WG * TAG_CH)

#if !defined(TAG_SHADER_GLSL)
/* The work buffer's parts (TAG_T_W_VFLAG to TAG_T_W_SLOTH, @w[9]) for
 * frames of up to @verts vertices, @strips strips and @slots draw groups;
 * its size in words */
static INLINE tag_u32 tag_work_layout(tag_u32 verts, tag_u32 strips, tag_u32 slots, tag_u32 *w)
{
   const tag_u32 chunks = (strips + TAG_CH - 1u) / TAG_CH;
   const tag_u32 groups = (strips + TAG_CH_N - 1u) / TAG_CH_N;
   tag_u32 at = 0u;
   w[0] = at; at += verts;                                      /* vertex flags */
   w[1] = at; at += strips;                                     /* strip ends */
   w[2] = at; at += strips * TAG_SS_WORDS;                      /* summaries */
   w[3] = at; at += strips * TAG_ST_WORDS;                      /* states */
   w[4] = at; at += (verts + TAG_SCAN_N - 1u) / TAG_SCAN_N;     /* the strip count's partial sums */
   w[5] = at; at += (chunks + TAG_CH_WG) * TAG_E_WORDS;         /* the group scan's threads */
   w[6] = at; at += (groups + 1u) * TAG_E_WORDS;                /* ...its workgroups */
   w[7] = at; at += (groups + 1u) * TAG_E_WORDS;                /* ...and what comes before each */
   w[8] = at; at += slots;                                      /* each slot's head */
   return at;
}
#endif

/* ---- Single precision on the bits ----
 *
 * Add, subtract, multiply and divide as IEEE 754 has them, rounded to the
 * nearest (ties to even), denormals and all, in integer arithmetic: the
 * results are the x86 CPU's on any GPU, which may flush denormals or
 * divide approximately. A NaN in comes out as the x86 SSE instructions
 * give it: the first operand's if it is one, else the second's, quietened;
 * an invalid operation gives the default NaN, 0xFFC00000. (After Berkeley
 * SoftFloat 3's f32 routines.) */
#define TAG_QNAN 0xFFC00000u
#define TAG_INF  0x7F800000u

TAG_FN tag_u32 tag_isnan(tag_u32 a)
{
   return (a & 0x7FFFFFFFu) > 0x7F800000u ? 1u : 0u;
}

TAG_FN tag_u32 tag_nan2(tag_u32 a, tag_u32 b)
{
   return tag_isnan(a) != 0u ? (a | 0x00400000u) : (b | 0x00400000u);
}

TAG_FN tag_u32 tag_clz(tag_u32 a)
{
#if defined(TAG_SHADER_GLSL)
   return a == 0u ? 32u : 31u - TAG_U32(findMSB(a));
#else
   tag_u32 n = 0u;
   if (a == 0u)
      return 32u;
   if ((a & 0xFFFF0000u) == 0u) { n += 16u; a <<= 16; }
   if ((a & 0xFF000000u) == 0u) { n += 8u; a <<= 8; }
   if ((a & 0xF0000000u) == 0u) { n += 4u; a <<= 4; }
   if ((a & 0xC0000000u) == 0u) { n += 2u; a <<= 2; }
   if ((a & 0x80000000u) == 0u) { n += 1u; }
   return n;
#endif
}

/* the high word of a 32 x 32 bit product */
TAG_FN tag_u32 tag_mulhi(tag_u32 a, tag_u32 b)
{
   tag_u32 al = a & 0xFFFFu, ah = a >> 16, bl = b & 0xFFFFu, bh = b >> 16;
   tag_u32 lh = al * bh, hl = ah * bl;
   tag_u32 mid = ((al * bl) >> 16) + (lh & 0xFFFFu) + (hl & 0xFFFFu);
   return ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}

/* @a shifted right by @d, what goes out kept as the lowest bit */
TAG_FN tag_u32 tag_jam(tag_u32 a, tag_u32 d)
{
   if (d == 0u)
      return a;
   if (d < 31u)
      return (a >> d) | ((a << (32u - d)) != 0u ? 1u : 0u);
   return a != 0u ? 1u : 0u;
}

/* @s, an exponent @e (less one), a significand @m with its integer bit at
 * bit 30: rounded, and put together */
TAG_FN tag_u32 tag_round_pack(tag_u32 s, tag_s32 e, tag_u32 m)
{
   tag_u32 rb = m & 0x7Fu;
   if (TAG_U32(e) >= 0xFDu)
   {
      if (e < 0)
      {
         m = tag_jam(m, TAG_U32(-e));
         e = 0;
         rb = m & 0x7Fu;
      }
      else if (e > 0xFD || m + 0x40u >= 0x80000000u)
         return (s << 31) | TAG_INF;
   }
   m = (m + 0x40u) >> 7;
   if (rb == 0x40u)
      m &= ~1u;
   if (m == 0u)
      e = 0;
   return (s << 31) + (TAG_U32(e) << 23) + m;
}

TAG_FN tag_u32 tag_norm_round_pack(tag_u32 s, tag_s32 e, tag_u32 m)
{
   tag_s32 sh = TAG_S32(tag_clz(m)) - 1;
   e -= sh;
   if (sh >= 7 && TAG_U32(e) < 0xFDu)
      return (s << 31) + ((m != 0u ? TAG_U32(e) : 0u) << 23) + (m << TAG_U32(sh - 7));
   return tag_round_pack(s, e, m << TAG_U32(sh));
}

TAG_FN tag_u32 tag_add_mags(tag_u32 a, tag_u32 b)
{
   tag_s32 ea = TAG_S32((a >> 23) & 0xFFu), eb = TAG_S32((b >> 23) & 0xFFu);
   tag_s32 d = ea - eb, ez;
   tag_u32 ma = a & 0x7FFFFFu, mb = b & 0x7FFFFFu, s = a >> 31, z;

   if (d == 0)
   {
      if (ea == 0)
         return a + mb;
      if (ea == 0xFF)
         return a;
      ez = ea;
      z = 0x01000000u + ma + mb;
      if ((z & 1u) == 0u && ez < 0xFE)
         return (s << 31) + (TAG_U32(ez) << 23) + (z >> 1);
      z <<= 6;
   }
   else
   {
      ma <<= 6;
      mb <<= 6;
      if (d < 0)
      {
         if (eb == 0xFF)
            return (s << 31) | TAG_INF;
         ez = eb;
         ma += ea != 0 ? 0x20000000u : ma;
         ma = tag_jam(ma, TAG_U32(-d));
      }
      else
      {
         if (ea == 0xFF)
            return a;
         ez = ea;
         mb += eb != 0 ? 0x20000000u : mb;
         mb = tag_jam(mb, TAG_U32(d));
      }
      z = 0x20000000u + ma + mb;
      if (z < 0x40000000u)
      {
         ez--;
         z <<= 1;
      }
   }
   return tag_round_pack(s, ez, z);
}

TAG_FN tag_u32 tag_sub_mags(tag_u32 a, tag_u32 b)
{
   tag_s32 ea = TAG_S32((a >> 23) & 0xFFu), eb = TAG_S32((b >> 23) & 0xFFu);
   tag_s32 d = ea - eb, ez, sh, sd;
   tag_u32 ma = a & 0x7FFFFFu, mb = b & 0x7FFFFFu, s = a >> 31, x, y;

   if (d == 0)
   {
      if (ea == 0xFF)
         return TAG_QNAN;
      sd = TAG_S32(ma) - TAG_S32(mb);
      if (sd == 0)
         return 0u;
      if (ea != 0)
         ea--;
      if (sd < 0)
      {
         s ^= 1u;
         sd = -sd;
      }
      sh = TAG_S32(tag_clz(TAG_U32(sd))) - 8;
      ez = ea - sh;
      if (ez < 0)
      {
         sh = ea;
         ez = 0;
      }
      return (s << 31) + (TAG_U32(ez) << 23) + (TAG_U32(sd) << TAG_U32(sh));
   }
   ma <<= 7;
   mb <<= 7;
   if (d < 0)
   {
      s ^= 1u;
      if (eb == 0xFF)
         return (s << 31) | TAG_INF;
      ez = eb - 1;
      x = mb | 0x40000000u;
      y = ma + (ea != 0 ? 0x40000000u : ma);
      d = -d;
   }
   else
   {
      if (ea == 0xFF)
         return a;
      ez = ea - 1;
      x = ma | 0x40000000u;
      y = mb + (eb != 0 ? 0x40000000u : mb);
   }
   return tag_norm_round_pack(s, ez, x - tag_jam(y, TAG_U32(d)));
}

TAG_FN tag_u32 tag_fadd(tag_u32 a, tag_u32 b)
{
   if (tag_isnan(a) != 0u || tag_isnan(b) != 0u)
      return tag_nan2(a, b);
   return ((a ^ b) >> 31) != 0u ? tag_sub_mags(a, b) : tag_add_mags(a, b);
}

TAG_FN tag_u32 tag_fsub(tag_u32 a, tag_u32 b)
{
   if (tag_isnan(a) != 0u || tag_isnan(b) != 0u)
      return tag_nan2(a, b);
   return ((a ^ b) >> 31) != 0u ? tag_add_mags(a, b) : tag_sub_mags(a, b);
}

TAG_FN tag_u32 tag_fmul(tag_u32 a, tag_u32 b)
{
   tag_u32 s = (a ^ b) >> 31, ma = a & 0x7FFFFFu, mb = b & 0x7FFFFFu, z, sh;
   tag_s32 ea = TAG_S32((a >> 23) & 0xFFu), eb = TAG_S32((b >> 23) & 0xFFu), ez;

   if (tag_isnan(a) != 0u || tag_isnan(b) != 0u)
      return tag_nan2(a, b);
   if (ea == 0xFF)
      return (TAG_U32(eb) | mb) == 0u ? TAG_QNAN : (s << 31) | TAG_INF;
   if (eb == 0xFF)
      return (TAG_U32(ea) | ma) == 0u ? TAG_QNAN : (s << 31) | TAG_INF;
   if (ea == 0)
   {
      if (ma == 0u)
         return s << 31;
      sh = tag_clz(ma) - 8u;
      ea = 1 - TAG_S32(sh);
      ma <<= sh;
   }
   if (eb == 0)
   {
      if (mb == 0u)
         return s << 31;
      sh = tag_clz(mb) - 8u;
      eb = 1 - TAG_S32(sh);
      mb <<= sh;
   }
   ez = ea + eb - 0x7F;
   ma = (ma | 0x00800000u) << 7;
   mb = (mb | 0x00800000u) << 8;
   z = tag_mulhi(ma, mb) | (ma * mb != 0u ? 1u : 0u);
   if (z < 0x40000000u)
   {
      ez--;
      z <<= 1;
   }
   return tag_round_pack(s, ez, z);
}

TAG_FN tag_u32 tag_fdiv(tag_u32 a, tag_u32 b)
{
   tag_u32 s = (a ^ b) >> 31, ma = a & 0x7FFFFFu, mb = b & 0x7FFFFFu, q, r, k, i, sh;
   tag_s32 ea = TAG_S32((a >> 23) & 0xFFu), eb = TAG_S32((b >> 23) & 0xFFu), ez;

   if (tag_isnan(a) != 0u || tag_isnan(b) != 0u)
      return tag_nan2(a, b);
   if (ea == 0xFF)
      return eb == 0xFF ? TAG_QNAN : (s << 31) | TAG_INF;
   if (eb == 0xFF)
      return s << 31;
   if (eb == 0)
   {
      if (mb == 0u)
         return (TAG_U32(ea) | ma) == 0u ? TAG_QNAN : (s << 31) | TAG_INF;
      sh = tag_clz(mb) - 8u;
      eb = 1 - TAG_S32(sh);
      mb <<= sh;
   }
   if (ea == 0)
   {
      if (ma == 0u)
         return s << 31;
      sh = tag_clz(ma) - 8u;
      ea = 1 - TAG_S32(sh);
      ma <<= sh;
   }
   ez = ea - eb + 0x7E;
   ma |= 0x00800000u;
   mb |= 0x00800000u;
   k = 30u;
   if (ma < mb)
   {
      ez--;
      k = 31u;
   }
   /* the quotient of ma << k by mb, a bit at a time, and whether anything
    * was left over */
   q = 0u;
   r = ma;
   for (i = 0u; i <= k; i++)
   {
      q <<= 1;
      if (r >= mb)
      {
         r -= mb;
         q |= 1u;
      }
)TAG_TEXT" R"TAG_TEXT(
      r <<= 1;
   }
   return tag_round_pack(s, ez, q | (r != 0u ? 1u : 0u));
}

/* a == b and a < b, as floats compare (a NaN is never either) */
TAG_FN tag_u32 tag_feq(tag_u32 a, tag_u32 b)
{
   if (tag_isnan(a) != 0u || tag_isnan(b) != 0u)
      return 0u;
   return (a == b || ((a | b) & 0x7FFFFFFFu) == 0u) ? 1u : 0u;
}

TAG_FN tag_u32 tag_fkey(tag_u32 a)
{
   if ((a & 0x7FFFFFFFu) == 0u)
      a = 0u;
   return (a >> 31) != 0u ? ~a : a | 0x80000000u;
}

TAG_FN tag_u32 tag_flt(tag_u32 a, tag_u32 b)
{
   if (tag_isnan(a) != 0u || tag_isnan(b) != 0u)
      return 0u;
   return tag_fkey(a) < tag_fkey(b) ? 1u : 0u;
}

/* An unsigned integer, exactly (up to 2^24) */
TAG_FN tag_u32 tag_u2f(tag_u32 n)
{
   tag_u32 e;
   if (n == 0u)
      return 0u;
   e = 31u - tag_clz(n);
   return ((127u + e) << 23) | ((n << (23u - e)) & 0x7FFFFFu);
}

/* ---- Shadows: a rectangle around vertices ----
 *
 * As ScreenBounds and PolyOverlaps() have it (rend/shadows.h): from 1e30
 * down and -1e30 up, a coordinate that is not a number changes nothing,
 * and the corners are compared as floats are. Kept as tag_fkey()s, so
 * that the smallest and largest come out the same on any GPU. */
TAG_STRUCT(tag_bounds)
{
   tag_u32 lo_x, lo_y, hi_x, hi_y;
}
TAG_STRUCT_END(tag_bounds)

TAG_FN tag_u32 tag_umin(tag_u32 a, tag_u32 b) { return a < b ? a : b; }
TAG_FN tag_u32 tag_umax(tag_u32 a, tag_u32 b) { return a > b ? a : b; }

TAG_FN tag_bounds tag_bounds_none(void)
{
   tag_bounds b;
   b.lo_x = tag_fkey(0x7149F2CAu);   /* 1e30f */
   b.lo_y = b.lo_x;
   b.hi_x = tag_fkey(0xF149F2CAu);   /* -1e30f */
   b.hi_y = b.hi_x;
   return b;
}

TAG_FN tag_bounds tag_bounds_add(tag_bounds b, tag_u32 x, tag_u32 y)
{
   if (tag_isnan(x) == 0u)
   {
      b.lo_x = tag_umin(b.lo_x, tag_fkey(x));
      b.hi_x = tag_umax(b.hi_x, tag_fkey(x));
   }
   if (tag_isnan(y) == 0u)
   {
      b.lo_y = tag_umin(b.lo_y, tag_fkey(y));
      b.hi_y = tag_umax(b.hi_y, tag_fkey(y));
   }
   return b;
}

/* whether @b meets the rectangle of the floats' bits @x0 @y0 to @x1 @y1
 * (which are numbers) */
TAG_FN tag_u32 tag_bounds_meet(tag_bounds b, tag_u32 x0, tag_u32 y0, tag_u32 x1, tag_u32 y1)
{
   return (b.lo_x <= tag_fkey(x1) && b.hi_x >= tag_fkey(x0) && b.lo_y <= tag_fkey(y1) && b.hi_y >= tag_fkey(y0)) ? 1u : 0u;
}

/* ---- The vertex formats ---- */

/* saturate01() of a float's bits: 0 to 1, anything negative 0; anything
 * above 1, infinity and a NaN of either sign 1 */
TAG_FN tag_u32 tag_sat(tag_u32 b)
{
   if ((b >> 31) != 0u)
      return (b & 0x7FFFFFFFu) > 0x7F800000u ? 0x3F800000u : 0u;
   return b > 0x3F800000u ? 0x3F800000u : b;
}

/* float_to_satu8(): the clamped value times 255, truncated. (The product
 * of a number from 0 to 1 and 255 is the same correctly rounded on any
 * GPU, and one too small to count is 0 whether the GPU keeps denormals or
 * not.) */
TAG_FN tag_u32 tag_sat8(tag_u32 b)
{
#if defined(TAG_SHADER_GLSL)
   precise float p = TAG_F(tag_sat(b)) * 255.0;
   return TAG_U32(p);
#else
   return TAG_U32(TAG_F(tag_sat(b)) * 255.0f);
#endif
}

/* vert_face_base_color(): the face colour's clamped float @f times the
 * clamped intensity times 255, truncated */
TAG_FN tag_u32 tag_int8(tag_u32 f, tag_u32 i)
{
#if defined(TAG_SHADER_GLSL)
   precise float it = TAG_F(tag_sat(i)) * 255.0;
   precise float p = TAG_F(f) * it;
   return TAG_U32(p);
#else
   float it = TAG_F(tag_sat(i)) * 255.0f;
   return TAG_U32(TAG_F(f) * it);
#endif
}

/* A packed colour as sent (alpha, red, green, blue from the top) to the
 * vertex's red, green, blue, alpha bytes */
TAG_FN tag_u32 tag_packed(tag_u32 t)
{
   return (t & 0xFF00FF00u) | ((t >> 16) & 0xFFu) | ((t & 0xFFu) << 16);
}

/* Four floats alpha, red, green, blue (words @w of the data) to red,
 * green, blue, alpha bytes */
TAG_FN tag_u32 tag_floatcol(tag_u32 a, tag_u32 r, tag_u32 g, tag_u32 b)
{
   return tag_sat8(r) | (tag_sat8(g) << 8) | (tag_sat8(b) << 16) | (tag_sat8(a) << 24);
}

/* An intensity @i with the face colour of header word @hf (floats at
 * TAG_H_FF + 3 * @set, the alpha byte from TAG_H_FCOL + @set) */
TAG_FN tag_u32 tag_intcol(tag_u32 hdr, tag_u32 set, tag_u32 i)
{
   tag_u32 f = hdr + TAG_H_FF + 3u * set;
   return tag_int8(TAG_TAB(f), i) | (tag_int8(TAG_TAB(f + 1u), i) << 8) | (tag_int8(TAG_TAB(f + 2u), i) << 16)
         | (TAG_TAB(hdr + TAG_H_FCOL + set) & 0xFF000000u);
}

/* is_vertex_inf() */
TAG_FN tag_u32 tag_inf(tag_u32 x, tag_u32 y, tag_u32 z)
{
   const tag_u32 huge_xy = 0x69045951u;   /* 1e25f */
   const tag_u32 huge_z = 0x7DCCA14Bu;    /* 3.4e37f */
   return ((x & 0x7FFFFFFFu) > huge_xy || (y & 0x7FFFFFFFu) > huge_xy
         || (z & 0x7FFFFFFFu) > 0x7F800000u || (z < 0x80000000u && z > huge_z)) ? 1u : 0u;
}

/* update_fz(): whether a depth counts for fZ_max (it is above 1.0, the
 * value it starts at, and below 2^20) */
TAG_FN tag_u32 tag_zcounts(tag_u32 z)
{
   return (TAG_S32(z) > 0x3F800000 && TAG_S32(z) < 0x49800000) ? 1u : 0u;
}

/* The word @w of a vertex's block @b; the second block is the next one,
 * however the vertex was cut (ta_main() takes what comes next for it) */
#define TAG_W(b, w) TAG_TA(TAG_TAB(TAG_T_TA_BASE) + (b) * 8u + (w))

/* The Vertex's words */
#define TAG_V_X    0
#define TAG_V_Y    1
#define TAG_V_Z    2
#define TAG_V_COL  3
#define TAG_V_SPC  4
#define TAG_V_U    5
#define TAG_V_V    6
#define TAG_V_COL1 7
#define TAG_V_SPC1 8
#define TAG_V_U1   9
#define TAG_V_V1   10
#define TAG_V_WORDS 11

TAG_STRUCT(tag_vertex)
{
   tag_u32 w[TAG_V_WORDS];
}
TAG_STRUCT_END(tag_vertex)

/* The polygon vertex of format @fmt in block @b, under header word @hdr:
 * AppendPolyVertex<fmt>() (and B), fields it does not write left 0 */
TAG_FN tag_vertex tag_poly_vertex(tag_u32 fmt, tag_u32 b, tag_u32 hdr)
{
   tag_vertex o;
   tag_u32 k;
   for (k = 0u; k < TAG_V_WORDS; k++)
      o.w[k] = 0u;
   o.w[TAG_V_X] = TAG_W(b, 1u);
   o.w[TAG_V_Y] = TAG_W(b, 2u);
   o.w[TAG_V_Z] = TAG_W(b, 3u);
   if (fmt == 0u)
      o.w[TAG_V_COL] = tag_packed(TAG_W(b, 6u));
   else if (fmt == 1u)
      o.w[TAG_V_COL] = tag_floatcol(TAG_W(b, 4u), TAG_W(b, 5u), TAG_W(b, 6u), TAG_W(b, 7u));
   else if (fmt == 2u)
      o.w[TAG_V_COL] = tag_intcol(hdr, 0u, TAG_W(b, 6u));
   else if (fmt == 3u || fmt == 4u)
   {
      o.w[TAG_V_COL] = tag_packed(TAG_W(b, 6u));
      o.w[TAG_V_SPC] = tag_packed(TAG_W(b, 7u));
   }
   else if (fmt == 5u || fmt == 6u)
   {
      o.w[TAG_V_COL] = tag_floatcol(TAG_W(b, 8u), TAG_W(b, 9u), TAG_W(b, 10u), TAG_W(b, 11u));
      o.w[TAG_V_SPC] = tag_floatcol(TAG_W(b, 12u), TAG_W(b, 13u), TAG_W(b, 14u), TAG_W(b, 15u));
   }
   else if (fmt == 7u || fmt == 8u)
   {
      o.w[TAG_V_COL] = tag_intcol(hdr, 0u, TAG_W(b, 6u));
      o.w[TAG_V_SPC] = tag_intcol(hdr, 1u, TAG_W(b, 7u));
   }
   else if (fmt == 9u)
   {
      o.w[TAG_V_COL] = tag_packed(TAG_W(b, 4u));
      o.w[TAG_V_COL1] = tag_packed(TAG_W(b, 5u));
   }
   else if (fmt == 10u)
   {
      o.w[TAG_V_COL] = tag_intcol(hdr, 0u, TAG_W(b, 4u));
      o.w[TAG_V_COL1] = tag_intcol(hdr, 2u, TAG_W(b, 5u));
   }
   else if (fmt == 11u || fmt == 12u)
   {
      o.w[TAG_V_COL] = tag_packed(TAG_W(b, 6u));
      o.w[TAG_V_SPC] = tag_packed(TAG_W(b, 7u));
      o.w[TAG_V_COL1] = tag_packed(TAG_W(b, 10u));
      o.w[TAG_V_SPC1] = tag_packed(TAG_W(b, 11u));
   }
   else if (fmt == 13u || fmt == 14u)
   {
      o.w[TAG_V_COL] = tag_intcol(hdr, 0u, TAG_W(b, 6u));
      o.w[TAG_V_SPC] = tag_intcol(hdr, 1u, TAG_W(b, 7u));
      o.w[TAG_V_COL1] = tag_intcol(hdr, 2u, TAG_W(b, 10u));
      o.w[TAG_V_SPC1] = tag_intcol(hdr, 3u, TAG_W(b, 11u));
   }
   /* the texture coordinates: floats, or the top halves of floats
    * (16-bit: v in the low half of the word, u in the high) */
   if (fmt == 3u || fmt == 5u || fmt == 7u || fmt == 11u || fmt == 13u)
   {
      o.w[TAG_V_U] = TAG_W(b, 4u);
      o.w[TAG_V_V] = TAG_W(b, 5u);
   }
   else if (fmt == 4u || fmt == 6u || fmt == 8u || fmt == 12u || fmt == 14u)
   {
      o.w[TAG_V_U] = TAG_W(b, 4u) & 0xFFFF0000u;
      o.w[TAG_V_V] = TAG_W(b, 4u) << 16;
   }
   if (fmt == 11u || fmt == 13u)
   {
      o.w[TAG_V_U1] = TAG_W(b, 8u);
      o.w[TAG_V_V1] = TAG_W(b, 9u);
   }
   else if (fmt == 12u || fmt == 14u)
   {
      o.w[TAG_V_U1] = TAG_W(b, 8u) & 0xFFFF0000u;
      o.w[TAG_V_V1] = TAG_W(b, 8u) << 16;
   }
   return o;
}

/* Corner @c of the sprite in blocks @b and @b + 1: AppendSpriteVertexA()
 * and B(), and for corner 0, the one the game does not give a depth or
 * texture coordinates for, CaclulateSpritePlane() - the plane through the
 * other three, operation for operation as the C code has them */
TAG_FN tag_vertex tag_sprite_vertex(tag_u32 c, tag_u32 b, tag_u32 hdr)
{
   tag_vertex o;
   tag_u32 k;
   for (k = 0u; k < TAG_V_WORDS; k++)
      o.w[k] = 0u;
   o.w[TAG_V_COL] = tag_packed(TAG_TAB(hdr + TAG_H_SCOL));
   o.w[TAG_V_SPC] = tag_packed(TAG_TAB(hdr + TAG_H_SCOL + 1u));
   if (c == 2u)
   {
      o.w[TAG_V_X] = TAG_W(b, 1u);
      o.w[TAG_V_Y] = TAG_W(b, 2u);
      o.w[TAG_V_Z] = TAG_W(b, 3u);
      o.w[TAG_V_U] = TAG_W(b, 13u) & 0xFFFF0000u;
      o.w[TAG_V_V] = TAG_W(b, 13u) << 16;
   }
   else if (c == 3u)
   {
      o.w[TAG_V_X] = TAG_W(b, 4u);
      o.w[TAG_V_Y] = TAG_W(b, 5u);
      o.w[TAG_V_Z] = TAG_W(b, 6u);
      o.w[TAG_V_U] = TAG_W(b, 14u) & 0xFFFF0000u;
      o.w[TAG_V_V] = TAG_W(b, 14u) << 16;
   }
   else if (c == 1u)
   {
      o.w[TAG_V_X] = TAG_W(b, 7u);
      o.w[TAG_V_Y] = TAG_W(b, 8u);
      o.w[TAG_V_Z] = TAG_W(b, 9u);
      o.w[TAG_V_U] = TAG_W(b, 15u) & 0xFFFF0000u;
      o.w[TAG_V_V] = TAG_W(b, 15u) << 16;
   }
   else
   {
      /* A = corner 2, B = corner 3, C = corner 1, P = corner 0 */
      tag_u32 ax = TAG_W(b, 1u), ay = TAG_W(b, 2u), az = TAG_W(b, 3u);
      tag_u32 bx = TAG_W(b, 4u), by = TAG_W(b, 5u), bz = TAG_W(b, 6u);
      tag_u32 cx = TAG_W(b, 7u), cy = TAG_W(b, 8u), cz = TAG_W(b, 9u);
      tag_u32 px = TAG_W(b, 10u), py = TAG_W(b, 11u);
      tag_u32 au = TAG_W(b, 13u) & 0xFFFF0000u, av = TAG_W(b, 13u) << 16;
      tag_u32 bu = TAG_W(b, 14u) & 0xFFFF0000u, bv = TAG_W(b, 14u) << 16;
      tag_u32 cu = TAG_W(b, 15u) & 0xFFFF0000u, cv = TAG_W(b, 15u) << 16;
      tag_u32 ac_x = tag_fsub(cx, ax), ac_y = tag_fsub(cy, ay), ac_z = tag_fsub(cz, az);
      tag_u32 ab_x = tag_fsub(bx, ax), ab_y = tag_fsub(by, ay), ab_z = tag_fsub(bz, az);
      tag_u32 ap_x = tag_fsub(px, ax), ap_y = tag_fsub(py, ay);
      tag_u32 ab_v = tag_fsub(bv, av), ab_u = tag_fsub(bu, au);
      tag_u32 ac_v = tag_fsub(cv, av), ac_u = tag_fsub(cu, au);
      tag_u32 k3 = tag_fsub(tag_fmul(ac_x, ab_y), tag_fmul(ac_y, ab_x));
      tag_u32 k2 = tag_fdiv(tag_fsub(tag_fmul(ap_x, ab_y), tag_fmul(ap_y, ab_x)), k3);
      tag_u32 k1;
      if (tag_feq(ab_x, 0u) != 0u)
         k1 = tag_fdiv(tag_fsub(tag_fsub(py, ay), tag_fmul(k2, ac_y)), ab_y);
      else
         k1 = tag_fdiv(tag_fsub(tag_fsub(px, ax), tag_fmul(k2, ac_x)), ab_x);
      o.w[TAG_V_X] = px;
      o.w[TAG_V_Y] = py;
      o.w[TAG_V_Z] = tag_fadd(tag_fadd(az, tag_fmul(k1, ab_z)), tag_fmul(k2, ac_z));
      o.w[TAG_V_U] = tag_fadd(tag_fadd(au, tag_fmul(k1, ab_u)), tag_fmul(k2, ac_u));
      o.w[TAG_V_V] = tag_fadd(tag_fadd(av, tag_fmul(k1, ab_v)), tag_fmul(k2, ac_v));
   }
   return o;
}

/* The run vertex @v is in (the last whose first vertex is not after it) */
TAG_FN tag_u32 tag_find_run(tag_u32 v)
{
   tag_u32 base = TAG_TAB(TAG_T_OFF_RUN), lo = 0u, hi = TAG_TAB(TAG_T_NRUNS), mid;
   while (hi - lo > 1u)
   {
      mid = (lo + hi) >> 1;
      if (TAG_TAB(base + mid * TAG_R_WORDS + TAG_R_VERTEX) <= v)
         lo = mid;
      else
         hi = mid;
   }
   return base + lo * TAG_R_WORDS;
}

/* The header (its first word) whose vertices take place @p in index order */
TAG_FN tag_u32 tag_find_header(tag_u32 p)
{
   tag_u32 base = TAG_TAB(TAG_T_OFF_HDR), lo = 0u, hi = TAG_TAB(TAG_T_NHDR), mid;
   while (hi - lo > 1u)
   {
      mid = (lo + hi) >> 1;
      if (TAG_TAB(base + mid * TAG_H_WORDS + TAG_H_VSTART) <= p)
         lo = mid;
      else
         hi = mid;
   }
)TAG_TEXT" R"TAG_TEXT(
   return base + lo * TAG_H_WORDS;
}

/* The modifier volume triangle @t (9 words: ModTriangle) word @k */
TAG_FN tag_u32 tag_find_mvrun(tag_u32 t)
{
   tag_u32 base = TAG_TAB(TAG_T_OFF_MVRUN), lo = 0u, hi = TAG_TAB(TAG_T_NMVRUN), mid;
   while (hi - lo > 1u)
   {
      mid = (lo + hi) >> 1;
      if (TAG_TAB(base + mid * TAG_M_WORDS + TAG_M_FIRST) <= t)
         lo = mid;
      else
         hi = mid;
   }
   return base + lo * TAG_M_WORDS;
}

TAG_FN tag_u32 tag_modtrig_word(tag_u32 t, tag_u32 k)
{
   tag_u32 r = tag_find_mvrun(t);
   tag_u32 b = TAG_TAB(r + TAG_M_BLOCK) + 2u * (t - TAG_TAB(r + TAG_M_FIRST));
   /* x0 y0 z0 x1 y1 z1 x2 from the first block after its control word,
    * y2 z2 from the second */
   return TAG_W(b, k + 1u);
}

/* Vertex @v (the background's four from the tables): its words, and its
 * place in index order, its flags and whether its depth counts for
 * fZ_max, through @info: place << 2 | counts << 1 | (end of strip) */
TAG_STRUCT(tag_unpacked)
{
   tag_vertex v;
   tag_u32 place;
   tag_u32 flags;
   tag_u32 zcount;
}
TAG_STRUCT_END(tag_unpacked)

TAG_FN tag_unpacked tag_unpack(tag_u32 v)
{
   tag_unpacked u;
   tag_u32 r, hdr, fmt, k, b, eos, i;

   if (v < 4u)
   {
      for (i = 0u; i < TAG_V_WORDS; i++)
         u.v.w[i] = TAG_TAB(TAG_TAB(TAG_T_OFF_BG) + v * TAG_V_WORDS + i);
      u.place = v;
      u.zcount = 0u;
      u.flags = TAG_VF(v, 1u - tag_inf(u.v.w[0], u.v.w[1], u.v.w[2]), v == 3u ? 1u : 0u);
      return u;
   }
   r = tag_find_run(v);
   hdr = TAG_TAB(TAG_T_OFF_HDR) + TAG_TAB(r + TAG_R_HDR) * TAG_H_WORDS;
   fmt = TAG_INFO_FMT(TAG_TAB(hdr + TAG_H_INFO));
   k = v - TAG_TAB(r + TAG_R_VERTEX);
   if (fmt == TAG_FMT_SPRITE)
   {
      b = TAG_TAB(r + TAG_R_BLOCK) + 2u * (k >> 2);
      u.v = tag_sprite_vertex(k & 3u, b, hdr);
      eos = (k & 3u) == 3u ? 1u : 0u;
   }
   else
   {
      b = TAG_TAB(r + TAG_R_BLOCK) + k * TAG_INFO_VB(TAG_TAB(hdr + TAG_H_INFO));
      u.v = tag_poly_vertex(fmt, b, hdr);
      eos = (TAG_W(b, 0u) >> 28) & 1u;
   }
   u.place = TAG_TAB(hdr + TAG_H_VSTART) + (v - TAG_TAB(hdr + TAG_H_VFIRST));
   u.zcount = tag_zcounts(u.v.w[TAG_V_Z]);
   u.flags = TAG_VF(v, 1u - tag_inf(u.v.w[0], u.v.w[1], u.v.w[2]), eos);
   return u;
}

/* ---- Strips to indices: make_index() ----
 *
 * A strip comes to make_index() in one of three ways (its "case"):
 *   0  a group of its own: its indices start a draw;
 *   1  joined to the strip before (same list and render pass, drawn the
 *      same way, and that group has indices), after an even number of the
 *      group's indices;
 *   2  the same after an odd number.
 * Joined, it starts with the group's last index again, and its first kept
 * vertex twice; a run of infinite vertices between two kept ones is
 * bridged by the last kept one and the next, twice; and a vertex is
 * doubled where its place in the strip and the group's count of indices
 * are not of the same parity, so that every triangle keeps its winding. */

/* What a strip's summary is (TAG_SS_*): the indices in each case, the
 * last kept vertex (0x80000000 set: there is one), its header, and its
 * first vertex */
TAG_STRUCT(tag_strip)
{
   tag_u32 n[3];
   tag_u32 last;
   tag_u32 hdr;
   tag_u32 first;
}
TAG_STRUCT_END(tag_strip)

/* The strip of places @p0 to @p1 (in index order) */
TAG_FN tag_strip tag_strip_summary(tag_u32 p0, tag_u32 p1, tag_u32 hdr)
{
   tag_strip s;
   tag_u32 n0 = 0u, n1 = 1u, n2 = 1u;  /* indices so far; joined, the link is the first */
   tag_u32 c0 = 0u, c1 = 1u, c2 = 2u;  /* the group's count, for its parity */
   tag_u32 d0 = 0u, d1 = 1u, d2 = 1u;  /* the next kept vertex is doubled */
   tag_u32 last = 0u, prev_inf = 0u, i = 0u, p, f;

   for (p = p0; p <= p1; p++, i++)
   {
      f = TAG_VFLAG(p);
      if ((f & TAG_VF_KEPT) == 0u)
      {
         prev_inf = 1u;
         continue;
      }
      if (prev_inf != 0u && last != 0u)
      {
         /* the run of infinite ones bridged */
         n0++; c0++; d0 = 1u;
         n1++; c1++; d1 = 1u;
         n2++; c2++; d2 = 1u;
      }
      prev_inf = 0u;
      last = 0x80000000u | (f >> 2);
      if (d0 != 0u) { n0++; c0++; d0 = 0u; }
      if (d1 != 0u) { n1++; c1++; d1 = 0u; }
      if (d2 != 0u) { n2++; c2++; d2 = 0u; }
      if (((i ^ c0) & 1u) != 0u) { n0++; c0++; }
      if (((i ^ c1) & 1u) != 0u) { n1++; c1++; }
      if (((i ^ c2) & 1u) != 0u) { n2++; c2++; }
      n0++; c0++;
      n1++; c1++;
      n2++; c2++;
   }
   s.n[0] = n0;
   s.n[1] = n1;
   s.n[2] = n2;
   s.last = last;
   s.hdr = hdr;
   s.first = TAG_VFLAG(p0) >> 2;
   return s;
}

/* Whether a strip of header @b joins the group of a strip of header @a
 * (header words): the same segment, not the translucent list, and the
 * same state (poly_same_state()) */
TAG_FN tag_u32 tag_joins(tag_u32 a, tag_u32 b)
{
   tag_u32 k, ia = TAG_TAB(a + TAG_H_INFO), ib = TAG_TAB(b + TAG_H_INFO);
   if (TAG_INFO_SEG(ia) != TAG_INFO_SEG(ib) || TAG_INFO_LIST(ib) == 2u)
      return 0u;
   for (k = 0u; k < 7u; k++)
      if (TAG_TAB(a + TAG_H_STATE + k) != TAG_TAB(b + TAG_H_STATE + k))
         return 0u;
   return 1u;
}

/* The scan's element: what a run of strips does to the state it is come
 * to with, for each case its first strip can come in (0: no group to
 * join, 1 and 2: joined after an even or odd count). For each: the indices
 * (n), the last index of the group it leaves (l), the head of that group
 * (hd), and bits: 1 the group's count is odd, 2 the group has indices, 4
 * l is the one come in with, 8 hd is the one come in with. first and
 * last are the headers of its first and last strips; first TAG_NONE: no
 * strips (the identity). */
TAG_STRUCT(tag_elem)
{
   tag_u32 first;
   tag_u32 last;
   tag_u32 n[3];
   tag_u32 l[3];
   tag_u32 hd[3];
   tag_u32 bits[3];
}
TAG_STRUCT_END(tag_elem)

#define TAG_E_ODD   1u
#define TAG_E_HAS   2u
#define TAG_E_LPASS 4u
#define TAG_E_HPASS 8u

TAG_FN tag_elem tag_elem_none(void)
{
   tag_elem e;
   tag_u32 j;
   e.first = TAG_NONE;
   e.last = TAG_NONE;
   for (j = 0u; j < 3u; j++)
   {
      e.n[j] = 0u;
      e.l[j] = 0u;
      e.hd[j] = 0u;
      e.bits[j] = 0u;
   }
   return e;
}

/* The element of strip @k with summary @s */
TAG_FN tag_elem tag_elem_strip(tag_strip s, tag_u32 k)
{
   tag_elem e;
   tag_u32 j, has = (s.last & 0x80000000u) != 0u ? 1u : 0u;
   e.first = s.hdr;
   e.last = s.hdr;
   for (j = 0u; j < 3u; j++)
   {
      e.n[j] = s.n[j];
      e.l[j] = has != 0u ? (s.last & 0x7FFFFFFFu) : 0u;
      e.hd[j] = k;
      /* the group's count after: from 0, or from the parity come in with */
      e.bits[j] = ((s.n[j] + (j == 2u ? 1u : 0u)) & 1u) != 0u ? TAG_E_ODD : 0u;
      if (j == 0u)
      {
         if (has != 0u)
            e.bits[j] |= TAG_E_HAS;
      }
      else
      {
         e.bits[j] |= TAG_E_HAS | TAG_E_HPASS;
         if (has == 0u)
            e.bits[j] |= TAG_E_LPASS;
      }
   }
   return e;
}

/* The case a strip of header @hdr comes in with, after a run that left
 * @bits and whose last strip was of header @last */
TAG_FN tag_u32 tag_case(tag_u32 bits, tag_u32 last, tag_u32 hdr)
{
   if ((bits & TAG_E_HAS) == 0u || tag_joins(last, hdr) == 0u)
      return 0u;
   return (bits & TAG_E_ODD) != 0u ? 2u : 1u;
}

/* @a, then @b */
TAG_FN tag_elem tag_combine(tag_elem a, tag_elem b)
{
   tag_elem r;
   tag_u32 j, y;
   if (a.first == TAG_NONE)
      return b;
   if (b.first == TAG_NONE)
      return a;
   r.first = a.first;
   r.last = b.last;
   for (j = 0u; j < 3u; j++)
   {
      y = tag_case(a.bits[j], a.last, b.first);
      r.n[j] = a.n[j] + b.n[y];
      r.bits[j] = b.bits[y] & (TAG_E_ODD | TAG_E_HAS);
      if ((b.bits[y] & TAG_E_LPASS) != 0u)
      {
         r.l[j] = a.l[j];
         r.bits[j] |= a.bits[j] & TAG_E_LPASS;
      }
      else
         r.l[j] = b.l[y];
      if ((b.bits[y] & TAG_E_HPASS) != 0u)
      {
         r.hd[j] = a.hd[j];
         r.bits[j] |= a.bits[j] & TAG_E_HPASS;
      }
      else
         r.hd[j] = b.hd[y];
   }
   return r;
}

/* The indices of the strip of places @p0 to @p1 come in with case @y,
 * the group's last index @l: TAG_IDX_PUT(at, vertex) for each, from @at */
#if defined(TAG_IDX_PUT)
TAG_FN tag_u32 tag_strip_emit(tag_u32 p0, tag_u32 p1, tag_u32 y, tag_u32 l, tag_u32 at)
{
   tag_u32 c = y == 2u ? 1u : 0u, dupe = y != 0u ? 1u : 0u;
   tag_u32 last = 0u, prev_inf = 0u, i = 0u, p, f, v;

   if (y != 0u)
   {
      TAG_IDX_PUT(at, l); at++; c++;
   }
   for (p = p0; p <= p1; p++, i++)
   {
      f = TAG_VFLAG(p);
      if ((f & TAG_VF_KEPT) == 0u)
      {
         prev_inf = 1u;
         continue;
      }
      v = f >> 2;
      if (prev_inf != 0u && last != 0u)
      {
         TAG_IDX_PUT(at, last & 0x7FFFFFFFu); at++; c++;
         dupe = 1u;
      }
      prev_inf = 0u;
      last = 0x80000000u | v;
      if (dupe != 0u)
      {
         TAG_IDX_PUT(at, v); at++; c++;
         dupe = 0u;
      }
      if (((i ^ c) & 1u) != 0u)
      {
         TAG_IDX_PUT(at, v); at++; c++;
      }
      TAG_IDX_PUT(at, v); at++; c++;
   }
   return at;
}
#endif

/* ---- The texture bleeding fix ---- */

/* Whether a vertex's texture coordinate lets its polygon be fixed: 0, or
 * from just under 1 to 1 (fix_texture_bleeding()) */
TAG_FN tag_u32 tag_bleed_ok(tag_u32 u)
{
   if (tag_feq(u, 0u) != 0u)
      return 1u;
   return (tag_flt(0x3F7EB852u, u) != 0u || tag_isnan(u) != 0u) && tag_flt(0x3F800000u, u) == 0u ? 1u : 0u;
}

/* ...and it fixed, for a texture 2^@lw texels wide */
TAG_FN tag_u32 tag_bleed_fix(tag_u32 u, tag_u32 w)
{
   if (tag_flt(0x3F7EB852u, u) != 0u)
      u = 0x3F800000u;
   return tag_fdiv(tag_fadd(0x3F000000u, tag_fmul(u, tag_u2f(w - 1u))), tag_u2f(w));
}

/* =================================================================== */
/* The kernels                                                         */
/* =================================================================== */
#if defined(TAG_SHADER_GLSL) && defined(TAG_KERNEL)

/* The most strips there is room for: as many as the lists have polygon
 * parameters (more, and the full parse overruns) */
TAG_FN uint tag_strip_room(void)
{
   return tab.w[TAG_T_ROOM_OP] + tab.w[TAG_T_ROOM_PT] + tab.w[TAG_T_ROOM_TR];
}

TAG_FN void tag_store_elem(uint at, tag_elem e)
{
   uint j;
   work.w[at] = e.first;
   work.w[at + 1u] = e.last;
   for (j = 0u; j < 3u; j++)
   {
      work.w[at + 2u + j] = e.n[j];
      work.w[at + 5u + j] = e.l[j];
      work.w[at + 8u + j] = e.hd[j];
      work.w[at + 11u + j] = e.bits[j];
   }
}

TAG_FN tag_elem tag_load_elem(uint at)
{
   tag_elem e;
   uint j;
   e.first = work.w[at];
   e.last = work.w[at + 1u];
   for (j = 0u; j < 3u; j++)
   {
      e.n[j] = work.w[at + 2u + j];
      e.l[j] = work.w[at + 5u + j];
      e.hd[j] = work.w[at + 8u + j];
      e.bits[j] = work.w[at + 11u + j];
   }
   return e;
}

TAG_FN tag_strip tag_load_strip(uint k)
{
   tag_strip s;
   uint at = tab.w[TAG_T_W_SSUM] + k * TAG_SS_WORDS;
   s.n[0] = work.w[at];
   s.n[1] = work.w[at + 1u];
   s.n[2] = work.w[at + 2u];
   s.last = work.w[at + 3u];
   s.hdr = work.w[at + 4u];
   s.first = work.w[at + 5u];
   return s;
}

/* The places in index order strip @k runs over, and its header */
TAG_FN uvec3 tag_strip_places(uint k)
{
   uint send = tab.w[TAG_T_W_SEND];
   uint p1 = work.w[send + k];
   uint hdr = tag_find_header(p1);
   uint p0 = tab.w[hdr + TAG_H_VSTART];
   if (k > 0u)
      p0 = max(p0, work.w[send + k - 1u] + 1u);
   return uvec3(p0, p1, hdr);
}

/* A strip's place in its list's records */
TAG_FN uint tag_strip_record(uint k, uint hdr)
{
   uint info = tab.w[hdr + TAG_H_INFO];
   uint seg = TAG_O_SEG + TAG_INFO_SEG(info) * TAG_OS_WORDS;
   uint li = frm.w[seg + TAG_OS_LBASE] + (k - frm.w[seg + TAG_OS_SFIRST]);
   uint list = TAG_INFO_LIST(info);
   uint base = list == 0u ? tab.w[TAG_T_REC_OP] : list == 1u ? tab.w[TAG_T_REC_PT] : tab.w[TAG_T_REC_TR];
   uint room = list == 0u ? tab.w[TAG_T_ROOM_OP] : list == 1u ? tab.w[TAG_T_ROOM_PT] : tab.w[TAG_T_ROOM_TR];
)TAG_TEXT" R"TAG_TEXT(
   return li < room ? base + li * TAG_C_WORDS : TAG_NONE;
}

#if TAG_KERNEL == TAG_K_UNPACK
layout(local_size_x = 64) in;
void main()
{
   uint g = gl_GlobalInvocationID.x;
   uint nv = tab.w[TAG_T_NVERTS], i;
   if (g < nv)
   {
      tag_unpacked u = tag_unpack(g);
      for (i = 0u; i < TAG_V_WORDS; i++)
         vtx.w[g * TAG_V_WORDS + i] = u.v.w[i];
      TAG_VFLAG(u.place) = u.flags;
      if (u.zcount != 0u)
         atomicMax(frm.w[TAG_O_ZMAX], u.v.w[TAG_V_Z]);
   }
   else if (g - nv < tab.w[TAG_T_NMOD])
   {
      uint t = g - nv;
      for (i = 0u; i < 9u; i++)
         mtr.w[t * 9u + i] = tag_modtrig_word(t, i);
   }
}
#endif

#if TAG_KERNEL == TAG_K_COUNT || TAG_KERNEL == TAG_K_ENDS
/* The ends of strips at the places of this workgroup: their count, and
 * (TAG_K_ENDS) each one's number, written in its place */
layout(local_size_x = TAG_SCAN_WG) in;
shared uint s_sum[TAG_SCAN_WG];
void main()
{
   uint t = gl_LocalInvocationID.x;
   uint p0 = gl_WorkGroupID.x * TAG_SCAN_N + t * TAG_SCAN_PER;
   uint nv = tab.w[TAG_T_NVERTS], n = 0u, i, s;
   for (i = 0u; i < TAG_SCAN_PER; i++)
      if (p0 + i < nv && (TAG_VFLAG(p0 + i) & TAG_VF_EOS) != 0u)
         n++;
   s_sum[t] = n;
   barrier();
   /* an inclusive prefix sum over the workgroup */
   for (s = 1u; s < TAG_SCAN_WG; s <<= 1)
   {
      uint add = t >= s ? s_sum[t - s] : 0u;
      barrier();
      s_sum[t] += add;
      barrier();
   }
#if TAG_KERNEL == TAG_K_COUNT
   if (t == TAG_SCAN_WG - 1u)
      work.w[tab.w[TAG_T_W_PART] + gl_WorkGroupID.x] = s_sum[t];
#else
   {
      uint k = work.w[tab.w[TAG_T_W_PART] + gl_WorkGroupID.x] + s_sum[t] - n;
      uint send = tab.w[TAG_T_W_SEND], room = tag_strip_room();
      for (i = 0u; i < TAG_SCAN_PER; i++)
         if (p0 + i < nv && (TAG_VFLAG(p0 + i) & TAG_VF_EOS) != 0u)
         {
            if (k < room)
               work.w[send + k] = p0 + i;
            k++;
         }
   }
#endif
}
#endif

#if TAG_KERNEL == TAG_K_COUNTSUM
/* The workgroups' counts to where each one's strips start, the total, and
 * the dispatches that go by strips */
layout(local_size_x = TAG_SCAN_WG) in;
shared uint s_sum[TAG_SCAN_WG];
void main()
{
   uint t = gl_LocalInvocationID.x;
   uint groups = (tab.w[TAG_T_NVERTS] + TAG_SCAN_N - 1u) / TAG_SCAN_N;
   uint per = (groups + TAG_SCAN_WG - 1u) / TAG_SCAN_WG;
   uint part = tab.w[TAG_T_W_PART], n = 0u, i, s, run;
   for (i = 0u; i < per; i++)
      if (t * per + i < groups)
         n += work.w[part + t * per + i];
   s_sum[t] = n;
   barrier();
   for (s = 1u; s < TAG_SCAN_WG; s <<= 1)
   {
      uint add = t >= s ? s_sum[t - s] : 0u;
      barrier();
      s_sum[t] += add;
      barrier();
   }
   run = s_sum[t] - n;
   for (i = 0u; i < per; i++)
      if (t * per + i < groups)
      {
         uint c = work.w[part + t * per + i];
         work.w[part + t * per + i] = run;
         run += c;
      }
   if (t == TAG_SCAN_WG - 1u)
   {
      uint ns = s_sum[t];
      if (ns > tag_strip_room())
      {
         ns = tag_strip_room();
         atomicOr(frm.w[TAG_O_OVERRUN], 16u);
      }
      frm.w[TAG_O_NSTRIPS] = ns;
      frm.w[TAG_O_ARGS64] = (ns + 63u) / 64u;
      frm.w[TAG_O_ARGS64 + 1u] = 1u;
      frm.w[TAG_O_ARGS64 + 2u] = 1u;
      frm.w[TAG_O_ARGSCH] = (ns + TAG_CH_N - 1u) / TAG_CH_N;
      frm.w[TAG_O_ARGSCH + 1u] = 1u;
      frm.w[TAG_O_ARGSCH + 2u] = 1u;
      frm.w[TAG_O_ARGS1] = ns;
      frm.w[TAG_O_ARGS1 + 1u] = 1u;
      frm.w[TAG_O_ARGS1 + 2u] = 1u;
   }
}
#endif

#if TAG_KERNEL == TAG_K_SUMMARY
layout(local_size_x = 64) in;
void main()
{
   uint k = gl_GlobalInvocationID.x;
   if (k >= frm.w[TAG_O_NSTRIPS])
      return;
   uvec3 pl = tag_strip_places(k);
   tag_strip s = tag_strip_summary(pl.x, pl.y, pl.z);
   uint at = tab.w[TAG_T_W_SSUM] + k * TAG_SS_WORDS;
   work.w[at] = s.n[0];
   work.w[at + 1u] = s.n[1];
   work.w[at + 2u] = s.n[2];
   work.w[at + 3u] = s.last;
   work.w[at + 4u] = s.hdr;
   work.w[at + 5u] = s.first;
}
#endif

#if TAG_KERNEL == TAG_K_GROUPS || TAG_KERNEL == TAG_K_STATES
/* The group scan's workgroups: a thread's strips folded, the folds scanned
 * over the workgroup (an inclusive Hillis-Steele scan, in two buffers).
 * TAG_K_GROUPS leaves each thread's inclusive fold and the workgroup's
 * whole; TAG_K_STATES starts each thread from what came before it and
 * goes through its strips again, writing each one's state. */
layout(local_size_x = TAG_CH_WG) in;
#if TAG_KERNEL == TAG_K_GROUPS
shared uint s_e[2][TAG_CH_WG][TAG_E_WORDS];

void tag_put_shared(uint b, uint t, tag_elem e)
{
   uint j;
   s_e[b][t][0] = e.first;
   s_e[b][t][1] = e.last;
   for (j = 0u; j < 3u; j++)
   {
      s_e[b][t][2u + j] = e.n[j];
      s_e[b][t][5u + j] = e.l[j];
      s_e[b][t][8u + j] = e.hd[j];
      s_e[b][t][11u + j] = e.bits[j];
   }
}

tag_elem tag_get_shared(uint b, uint t)
{
   tag_elem e;
   uint j;
   e.first = s_e[b][t][0];
   e.last = s_e[b][t][1];
   for (j = 0u; j < 3u; j++)
   {
      e.n[j] = s_e[b][t][2u + j];
      e.l[j] = s_e[b][t][5u + j];
      e.hd[j] = s_e[b][t][8u + j];
      e.bits[j] = s_e[b][t][11u + j];
   }
   return e;
}
#endif

void main()
{
   uint t = gl_LocalInvocationID.x;
   uint ns = frm.w[TAG_O_NSTRIPS];
   uint k0 = gl_WorkGroupID.x * TAG_CH_N + t * TAG_CH;
   uint k;
#if TAG_KERNEL == TAG_K_GROUPS
   tag_elem e = tag_elem_none();
   uint s, cur = 0u;
   for (k = k0; k < k0 + TAG_CH && k < ns; k++)
      e = tag_combine(e, tag_elem_strip(tag_load_strip(k), k));
   tag_put_shared(0u, t, e);
   barrier();
   for (s = 1u; s < TAG_CH_WG; s <<= 1)
   {
      tag_elem x = tag_get_shared(cur, t);
      if (t >= s)
         x = tag_combine(tag_get_shared(cur, t - s), x);
      tag_put_shared(cur ^ 1u, t, x);
      cur ^= 1u;
      barrier();
   }
   tag_store_elem(tab.w[TAG_T_W_CHUNK] + (gl_WorkGroupID.x * TAG_CH_WG + t) * TAG_E_WORDS, tag_get_shared(cur, t));
   if (t == TAG_CH_WG - 1u)
      tag_store_elem(tab.w[TAG_T_W_WGAGG] + gl_WorkGroupID.x * TAG_E_WORDS, tag_get_shared(cur, t));
#else
   /* what came before this thread's strips: the workgroups before, and
    * the threads before in this one */
   tag_elem pre = tag_load_elem(tab.w[TAG_T_W_WGPRE] + gl_WorkGroupID.x * TAG_E_WORDS);
   if (t > 0u)
      pre = tag_combine(pre, tag_load_elem(tab.w[TAG_T_W_CHUNK] + (gl_WorkGroupID.x * TAG_CH_WG + t - 1u) * TAG_E_WORDS));
   /* the state that leaves, from no group at all */
   uint at = pre.n[0], bits = pre.bits[0], l = pre.l[0], hd = pre.hd[0], last = pre.last;
   uint sst = tab.w[TAG_T_W_SSTATE], sloth = tab.w[TAG_T_W_SLOTH];
   if (pre.first == TAG_NONE)
      bits = 0u;
   for (k = k0; k < k0 + TAG_CH && k < ns; k++)
   {
      tag_strip st = tag_load_strip(k);
      uint y = last == TAG_NONE ? 0u : tag_case(bits, last, st.hdr);
      uint has = (st.last & 0x80000000u) != 0u ? 1u : 0u;
      uint r = tag_strip_record(k, st.hdr);
      work.w[sst + k * TAG_ST_WORDS] = at;
      work.w[sst + k * TAG_ST_WORDS + 1u] = y;
      work.w[sst + k * TAG_ST_WORDS + 2u] = l;
      if (y == 0u)
      {
         hd = k;
         bits = ((st.n[0] & 1u) != 0u ? TAG_E_ODD : 0u) | (has != 0u ? TAG_E_HAS : 0u);
         if (st.n[0] != 0u && tab.w[st.hdr + TAG_H_SLOT] != TAG_NONE)
            work.w[sloth + tab.w[st.hdr + TAG_H_SLOT]] = k + 1u;
      }
      else
         bits = (((st.n[y] + (y == 2u ? 1u : 0u)) & 1u) != 0u ? TAG_E_ODD : 0u) | TAG_E_HAS;
      work.w[sst + k * TAG_ST_WORDS + 3u] = hd;
      if (has != 0u)
         l = st.last & 0x7FFFFFFFu;
      at += st.n[y];
      last = st.hdr;
      if (r != TAG_NONE)
      {
         rec.w[r + TAG_C_COUNT] = 0u;
         rec.w[r + TAG_C_INST] = 1u;
         rec.w[r + TAG_C_FIRST] = y == 0u ? at - st.n[y] : st.first;
         rec.w[r + 3u] = 0u;
         rec.w[r + 4u] = 0u;
      }
      if (k == ns - 1u)
      {
         frm.w[TAG_O_NIDX] = at;
         if (at > tab.w[TAG_T_ROOM_IDX])
            atomicOr(frm.w[TAG_O_OVERRUN], 1u);
      }
   }
#endif
}
#endif

#if TAG_KERNEL == TAG_K_GROUPSUM
/* The workgroups' folds to what comes before each, and the segments:
 * where each one's strips start, how many, and their place in the list */
layout(local_size_x = 64) in;
void main()
{
   uint t = gl_LocalInvocationID.x;
   uint ns = frm.w[TAG_O_NSTRIPS];
   uint groups = (ns + TAG_CH_N - 1u) / TAG_CH_N;
   uint nseg = tab.w[TAG_T_NSEG], i;
   if (t == 0u)
   {
      tag_elem pre = tag_elem_none();
      for (i = 0u; i < groups; i++)
      {
         tag_store_elem(tab.w[TAG_T_W_WGPRE] + i * TAG_E_WORDS, pre);
         pre = tag_combine(pre, tag_load_elem(tab.w[TAG_T_W_WGAGG] + i * TAG_E_WORDS));
      }
   }
   /* the first strip that ends at or after each segment's start */
   for (i = t; i < nseg; i += 64u)
   {
      uint at = tab.w[TAG_T_OFF_SEG] + i * TAG_G_WORDS;
      uint vs = tab.w[at + TAG_G_VSTART], lo = 0u, hi = ns, mid;
      uint send = tab.w[TAG_T_W_SEND];
      while (lo < hi)
      {
         mid = (lo + hi) >> 1;
         if (work.w[send + mid] < vs)
            lo = mid + 1u;
         else
            hi = mid;
      }
      frm.w[TAG_O_SEG + i * TAG_OS_WORDS + TAG_OS_SFIRST] = lo;
   }
   memoryBarrierBuffer();
   barrier();
   if (t == 0u)
   {
      uint lbase[3] = uint[3](0u, 0u, 0u);
      uint rooms[3] = uint[3](tab.w[TAG_T_ROOM_OP], tab.w[TAG_T_ROOM_PT], tab.w[TAG_T_ROOM_TR]);
      for (i = 0u; i < nseg; i++)
      {
         uint s = TAG_O_SEG + i * TAG_OS_WORDS;
         uint end = i + 1u < nseg ? frm.w[s + TAG_OS_WORDS + TAG_OS_SFIRST] : ns;
         uint n = end - frm.w[s + TAG_OS_SFIRST];
         uint l = i % 3u;
         frm.w[s + TAG_OS_STRIPS] = n;
         frm.w[s + TAG_OS_LBASE] = lbase[l];
         lbase[l] += n;
         if (l == 2u)
         {
            uint p = i / 3u;
            frm.w[TAG_O_PASS + p * 3u] = lbase[0];
            frm.w[TAG_O_PASS + p * 3u + 1u] = lbase[1];
            frm.w[TAG_O_PASS + p * 3u + 2u] = lbase[2];
         }
      }
      /* (a list gets one more for the polygon parameter being filled) */
      for (i = 0u; i < 3u; i++)
         if (lbase[i] + 1u > rooms[i])
            atomicOr(frm.w[TAG_O_OVERRUN], 2u << i);
   }
}
#endif

#if TAG_KERNEL == TAG_K_EMIT
/* A strip's indices, and their count added to its group's head's record */
layout(local_size_x = 64) in;
void main()
{
   uint k = gl_GlobalInvocationID.x;
   if (k >= frm.w[TAG_O_NSTRIPS])
      return;
   uvec3 pl = tag_strip_places(k);
   uint st = tab.w[TAG_T_W_SSTATE] + k * TAG_ST_WORDS;
   uint at = work.w[st], hd = work.w[st + 3u];
   uint n = tag_strip_emit(pl.x, pl.y, work.w[st + 1u], work.w[st + 2u], at) - at;
   if (n != 0u)
   {
      uint r = tag_strip_record(hd, work.w[tab.w[TAG_T_W_SSUM] + hd * TAG_SS_WORDS + 4u]);
      if (r != TAG_NONE)
         atomicAdd(rec.w[r + TAG_C_COUNT], n);
   }
}
#endif

#if TAG_KERNEL == TAG_K_FINISH
layout(local_size_x = 64) in;
void main()
{
   uint s = gl_GlobalInvocationID.x;
   if (s >= tab.w[TAG_T_NSLOT])
      return;
   uint at = s * TAG_C_WORDS;
   uint h = work.w[tab.w[TAG_T_W_SLOTH] + s];
   uint count = 0u, first = 0u;
   if (h != 0u && frm.w[TAG_O_OVERRUN] == 0u)
   {
      uint hdr = work.w[tab.w[TAG_T_W_SSUM] + (h - 1u) * TAG_SS_WORDS + 4u];
      uint r = tag_strip_record(h - 1u, hdr);
      if (r != TAG_NONE)
      {
         count = rec.w[r + TAG_C_COUNT];
         first = rec.w[r + TAG_C_FIRST];
      }
   }
   cmd.w[at + TAG_C_COUNT] = count;
   cmd.w[at + TAG_C_INST] = 1u;
   cmd.w[at + TAG_C_FIRST] = first;
   cmd.w[at + 3u] = 0u;
   cmd.w[at + 4u] = 0u;
}
#endif

#if TAG_KERNEL == TAG_K_BLEED
/* A workgroup for strip gl_WorkGroupID.x: if it heads a group of three
 * indices or more and is textured, the vertices from its first index's to
 * its last's are looked at together, and fixed if they all can be */
layout(local_size_x = 64) in;
shared uint s_ok;
void main()
{
   uint k = gl_WorkGroupID.x, t = gl_LocalInvocationID.x;
   uint sst = tab.w[TAG_T_W_SSTATE];
   if (work.w[sst + k * TAG_ST_WORDS + 1u] != 0u || frm.w[TAG_O_OVERRUN] != 0u)
)TAG_TEXT" R"TAG_TEXT(
      return;
   uint hdr = work.w[tab.w[TAG_T_W_SSUM] + k * TAG_SS_WORDS + 4u];
   uint r = tag_strip_record(k, hdr);
   if (r == TAG_NONE || TAG_INFO_TEX(tab.w[hdr + TAG_H_INFO]) == 0u || rec.w[r + TAG_C_COUNT] < 3u)
      return;
   uint f = rec.w[r + TAG_C_FIRST], c = rec.w[r + TAG_C_COUNT];
   uint a = idx.w[f], b = idx.w[f + c - 1u], v;
   uint z = vtx.w[a * TAG_V_WORDS + TAG_V_Z];
   if (t == 0u)
      s_ok = 1u;
   barrier();
   for (v = a + t; v <= b; v += 64u)
   {
      uint o = v * TAG_V_WORDS;
      if (tag_bleed_ok(vtx.w[o + TAG_V_U]) == 0u || tag_bleed_ok(vtx.w[o + TAG_V_V]) == 0u
            || (v != a && tag_feq(z, vtx.w[o + TAG_V_Z]) == 0u))
         s_ok = 0u;
   }
   barrier();
   if (s_ok == 0u)
      return;
   uint tsp = tab.w[hdr + TAG_H_STATE + TAG_S_TSP];
   uint w = 8u << ((tsp >> 3) & 7u), h = 8u << (tsp & 7u);
   for (v = a + t; v <= b; v += 64u)
   {
      uint o = v * TAG_V_WORDS;
      vtx.w[o + TAG_V_U] = tag_bleed_fix(vtx.w[o + TAG_V_U], w);
      vtx.w[o + TAG_V_V] = tag_bleed_fix(vtx.w[o + TAG_V_V], h);
   }
}
#endif

#if TAG_KERNEL == TAG_K_TRDRAW
/* A translucent strip's record in its draw group's place (TAG_H_TRB), and
 * the group's count by its last strip; its polygon parameter's words */
layout(local_size_x = 64) in;
/* The first strip at or after the place @v in index order: the first one
 * that ends there or later */
TAG_FN uint tag_strip_at(uint v, uint ns)
{
   uint send = tab.w[TAG_T_W_SEND], lo = 0u, hi = ns, mid;
   while (lo < hi)
   {
      mid = (lo + hi) >> 1;
      if (work.w[send + mid] < v)
         lo = mid + 1u;
      else
         hi = mid;
   }
   return lo;
}
void main()
{
   uint k = gl_GlobalInvocationID.x;
   uint ns = frm.w[TAG_O_NSTRIPS];
   if (k >= ns)
      return;
   uint ssum = tab.w[TAG_T_W_SSUM];
   uint hdr = work.w[ssum + k * TAG_SS_WORDS + 4u];
   if (TAG_INFO_LIST(tab.w[hdr + TAG_H_INFO]) != 2u)
      return;
   uint r = tag_strip_record(k, hdr);
   if (r == TAG_NONE)
      return;
   uint li = (r - tab.w[TAG_T_REC_TR]) / TAG_C_WORDS;
   uint st = hdr + TAG_H_STATE;
   trp.w[li * TAG_P_WORDS] = (tab.w[st + TAG_S_TSP] & 0xFFFF00C0u) | ((tab.w[st + TAG_S_ISP] >> 16) & 0xE400u)
         | ((tab.w[st + TAG_S_PCW] >> 7) & 1u);
   trp.w[li * TAG_P_WORDS + 1u] = tab.w[st + TAG_S_TSP1];
   if (frm.w[TAG_O_OVERRUN] != 0u)
      return;
   uint b = tab.w[hdr + TAG_H_TRB];
   uint j = k - tag_strip_at(tab.w[hdr + TAG_H_TRV], ns);
   uint at = (b + 1u + j) * TAG_C_WORDS;
   trd.w[at + TAG_C_COUNT] = rec.w[r + TAG_C_COUNT];
   trd.w[at + TAG_C_INST] = 1u;
   trd.w[at + TAG_C_FIRST] = rec.w[r + TAG_C_FIRST];
   trd.w[at + 3u] = 0u;
   trd.w[at + TAG_C_INSTANCE] = (tab.w[TAG_T_FLAGS] & TAG_FLAG_INSTANCE) != 0u ? li : 0u;
   bool last = k + 1u == ns;
   if (!last)
   {
      uint next = work.w[ssum + (k + 1u) * TAG_SS_WORDS + 4u];
      last = TAG_INFO_LIST(tab.w[next + TAG_H_INFO]) != 2u || tab.w[next + TAG_H_TRB] != b;
   }
   if (last)
      trd.w[b * TAG_C_WORDS] = j + 1u;
}
#endif

#if TAG_KERNEL == TAG_K_PROVOKE
/* A workgroup for strip gl_WorkGroupID.x: if it heads a flat-shaded group
 * of three indices or more, each index but the last two takes the colours
 * of the vertex two indices on. The CPU does it one index after another,
 * so each reads the colours as they were (an earlier index of a strip is
 * never the same vertex as one three or more places on: make_index()
 * repeats a vertex at most three times running), and a vertex at several
 * indices running keeps what the last of them wrote. Here a run of 64
 * indices at a time reads, then writes. */
layout(local_size_x = 64) in;
void main()
{
   uint k = gl_WorkGroupID.x, t = gl_LocalInvocationID.x;
   if (frm.w[TAG_O_OVERRUN] != 0u)
      return;
   uint hdr = work.w[tab.w[TAG_T_W_SSUM] + k * TAG_SS_WORDS + 4u];
   if ((tab.w[hdr + TAG_H_STATE + TAG_S_PCW] & 2u) != 0u)
      return;   /* Gouraud */
   uint r = tag_strip_record(k, hdr);
   if (r == TAG_NONE || rec.w[r + TAG_C_COUNT] <= 2u)
      return;
   uint f = rec.w[r + TAG_C_FIRST], n = rec.w[r + TAG_C_COUNT] - 2u, base;
   for (base = 0u; base < n; base += 64u)
   {
      uint i = base + t, dst = 0u, c0 = 0u, c1 = 0u, c2 = 0u, c3 = 0u;
      bool put = false;
      if (i < n)
      {
         uint s = idx.w[f + i + 2u] * TAG_V_WORDS;
         c0 = vtx.w[s + TAG_V_COL];
         c1 = vtx.w[s + TAG_V_SPC];
         c2 = vtx.w[s + TAG_V_COL1];
         c3 = vtx.w[s + TAG_V_SPC1];
         dst = idx.w[f + i];
         put = i + 1u >= n || idx.w[f + i + 1u] != dst;
      }
      memoryBarrierBuffer();
      barrier();
      if (put)
      {
         uint o = dst * TAG_V_WORDS;
         vtx.w[o + TAG_V_COL] = c0;
         vtx.w[o + TAG_V_SPC] = c1;
         vtx.w[o + TAG_V_COL1] = c2;
         vtx.w[o + TAG_V_SPC1] = c3;
      }
      memoryBarrierBuffer();
      barrier();
   }
}
#endif

#if TAG_KERNEL == TAG_K_SHADOW
/* A workgroup for each entry of the shadow table (as many workgroups as
 * there are, up to the most a dispatch has, each then taking every so
 * many): the rectangle around the vertices of its draw group's indices,
 * gathered by its 64 threads, against its render pass's; its command
 * again, with its indices if they meet, none if not. The CPU's walk stops
 * as soon as the rectangle meets, which the rectangle of all of them then
 * does too. */
layout(local_size_x = 64) in;
shared uint s_b[256];
void main()
{
   uint t = gl_LocalInvocationID.x, n = tab.w[TAG_T_NSHD], base = tab.w[TAG_T_OFF_SHD], e, i, h;
   for (e = gl_WorkGroupID.x; e < n; e += gl_NumWorkGroups.x)
   {
      uint w = tab.w[base + TAG_SH_ENTRY + e];
      uint at = TAG_SH_SLOT(w) * TAG_C_WORDS, a = base + TAG_SH_AREA + TAG_SH_PASS(w) * 4u;
      uint count = cmd.w[at + TAG_C_COUNT], first = cmd.w[at + TAG_C_FIRST];
      tag_bounds b = tag_bounds_none();
      for (i = t; i < count; i += 64u)
      {
         uint o = idx.w[first + i] * TAG_V_WORDS;
         b = tag_bounds_add(b, vtx.w[o + TAG_V_X], vtx.w[o + TAG_V_Y]);
      }
      s_b[t] = b.lo_x;
      s_b[64u + t] = b.lo_y;
      s_b[128u + t] = b.hi_x;
      s_b[192u + t] = b.hi_y;
      barrier();
      for (h = 32u; h > 0u; h >>= 1)
      {
         if (t < h)
         {
            s_b[t] = min(s_b[t], s_b[t + h]);
            s_b[64u + t] = min(s_b[64u + t], s_b[64u + t + h]);
            s_b[128u + t] = max(s_b[128u + t], s_b[128u + t + h]);
            s_b[192u + t] = max(s_b[192u + t], s_b[192u + t + h]);
         }
         barrier();
      }
      if (t == 0u)
      {
         uint o = (tab.w[TAG_T_SHD_CMD] + TAG_SH_SLOT(w)) * TAG_C_WORDS;
         b.lo_x = s_b[0];
         b.lo_y = s_b[64];
         b.hi_x = s_b[128];
         b.hi_y = s_b[192];
         cmd.w[o + TAG_C_COUNT] = count != 0u && tag_bounds_meet(b, tab.w[a], tab.w[a + 1u], tab.w[a + 2u], tab.w[a + 3u]) != 0u
               ? count : 0u;
         cmd.w[o + TAG_C_INST] = 1u;
         cmd.w[o + TAG_C_FIRST] = first;
         cmd.w[o + 3u] = 0u;
         cmd.w[o + 4u] = 0u;
      }
      barrier();
   }
}
#endif

#endif /* the kernels */

#endif
)TAG_TEXT"
