/* The ground the tile renderer's shared rules stand on: one text of each
 * rule for the C and C++ code (the TA decoder, the CPU reference, the
 * tests) and for the compute and fragment shaders.
 *
 * The same headers are read three ways:
 * - by a C89 or C++ compiler (nothing defined);
 * - by a GLSL compiler, with PVR_SHADER_GLSL defined (GLSL 4.30 or a
 *   Vulkan GLSL 4.50 kernel, through #include);
 * - by an HLSL compiler, with PVR_SHADER_HLSL defined.
 * So the rules are written in what the three have in common: functions
 * of unsigned and signed 32-bit integers and floats, by value, through
 * the macros below for the types, the casts and the few builtins. No
 * pointers, no structures, no 64-bit types, no casts but these.
 *
 * The lane operations (LANE_*) are the shader side only: a compute kernel
 * works one pixel per lane, its samples in adjacent lanes, and a pixel's
 * samples are combined by exchanging values between lanes. They are
 * subgroup operations where the device has them (PVR_LANE_SUBGROUP
 * defined: Vulkan, GL_KHR_shader_subgroup, D3D12 shader model 6.0), and
 * otherwise exchanges through shared memory with barriers, which needs
 * the kernel to define PVR_LANE_COUNT (the lanes of a group, a power of
 * two up to 32) and PVR_LANE_GROUP_SIZE (its workgroup's size, a multiple
 * of PVR_LANE_COUNT), and to call them all in uniform control flow. The C
 * side has the same exchanges over an array of lanes (pvr_lane_c_*), for
 * the CPU reference and the tests. */
#ifndef PVR_LANE_H
#define PVR_LANE_H

#if defined(PVR_SHADER_GLSL)

#define PVR_FN
#define pvr_u32 uint
#define pvr_s32 int
#define pvr_f32 float
#define PVR_U32(x) uint(x)
#define PVR_S32(x) int(x)
#define PVR_F32(x) float(x)
#define PVR_F32_BITS(x) floatBitsToUint(x)
#define PVR_CEIL(x) ceil(x)

#elif defined(PVR_SHADER_HLSL)

#define PVR_FN
#define pvr_u32 uint
#define pvr_s32 int
#define pvr_f32 float
#define PVR_U32(x) ((uint)(x))
#define PVR_S32(x) ((int)(x))
#define PVR_F32(x) ((float)(x))
#define PVR_F32_BITS(x) asuint(x)
#define PVR_CEIL(x) ceil(x)

#else

#include <math.h>
#include <string.h>
#include <retro_inline.h>

#define PVR_FN static INLINE
typedef unsigned int pvr_u32;
typedef int pvr_s32;
typedef float pvr_f32;
/* (32 bits on every target the core is built for) */
typedef char pvr_u32_is_32_bits[sizeof(pvr_u32) == 4 && sizeof(pvr_f32) == 4 ? 1 : -1];
#define PVR_U32(x) ((pvr_u32)(x))
#define PVR_S32(x) ((pvr_s32)(x))
#define PVR_F32(x) ((pvr_f32)(x))
#define PVR_CEIL(x) ((pvr_f32)ceil(x))

/* A float's bits, as floatBitsToUint() and asuint() give them */
PVR_FN pvr_u32 pvr_f32_bits(pvr_f32 f)
{
   pvr_u32 u;
   memcpy(&u, &f, sizeof(u));
   return u;
}
#define PVR_F32_BITS(x) pvr_f32_bits(x)

#endif

/* ---- Lane operations: shaders ---- */

#if defined(PVR_SHADER_GLSL) && defined(PVR_LANE_SUBGROUP)

#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_shuffle : require
#define pvr_ballot uvec4
#define LANE_ID() gl_SubgroupInvocationID
#define LANE_BALLOT(c) subgroupBallot(c)
#define LANE_PREFIX_COUNT(c) subgroupBallotExclusiveBitCount(subgroupBallot(c))
#define LANE_SHUFFLE(v, i) subgroupShuffle(v, i)
#define LANE_SHUFFLE_XOR(v, m) subgroupShuffleXor(v, m)
#define LANE_FIRST(v) subgroupBroadcastFirst(v)

#elif defined(PVR_SHADER_HLSL) && defined(PVR_LANE_SUBGROUP)

#define pvr_ballot uint4
#define LANE_ID() WaveGetLaneIndex()
#define LANE_BALLOT(c) WaveActiveBallot(c)
#define LANE_PREFIX_COUNT(c) WavePrefixCountBits(c)
#define LANE_SHUFFLE(v, i) WaveReadLaneAt(v, i)
#define LANE_SHUFFLE_XOR(v, m) WaveReadLaneAt(v, WaveGetLaneIndex() ^ (m))
#define LANE_FIRST(v) WaveReadLaneFirst(v)

#elif defined(PVR_SHADER_GLSL) || defined(PVR_SHADER_HLSL)

#if !defined(PVR_LANE_COUNT) || !defined(PVR_LANE_GROUP_SIZE)
#error "PVR_LANE_COUNT and PVR_LANE_GROUP_SIZE are needed without subgroup operations"
#endif

#if defined(PVR_SHADER_GLSL)
#define pvr_ballot uvec4
#define PVR_LANE_SELF gl_LocalInvocationIndex
#define PVR_LANE_SYNC() memoryBarrierShared(); barrier()
#define PVR_BALLOT_OF(x) uvec4(x, 0u, 0u, 0u)
#define PVR_BITCOUNT(x) uint(bitCount(x))
shared uint pvr_lane_xchg[PVR_LANE_GROUP_SIZE];
#else
#define pvr_ballot uint4
#define PVR_LANE_SELF pvr_lane_self
#define PVR_LANE_SYNC() GroupMemoryBarrierWithGroupSync()
#define PVR_BALLOT_OF(x) uint4(x, 0u, 0u, 0u)
#define PVR_BITCOUNT(x) countbits(x)
/* (HLSL has no global for the thread's index in its group: the kernel
 * sets this from SV_GroupIndex before the first lane operation) */
static uint pvr_lane_self;
groupshared uint pvr_lane_xchg[PVR_LANE_GROUP_SIZE];
#endif

uint pvr_lane_id()
{
   return PVR_LANE_SELF & uint(PVR_LANE_COUNT - 1);
}

/* lane @i's @v, of this lane's group */
uint pvr_lane_shuffle(uint v, uint i)
{
   uint got;
   PVR_LANE_SYNC();
   pvr_lane_xchg[PVR_LANE_SELF] = v;
   PVR_LANE_SYNC();
   got = pvr_lane_xchg[(PVR_LANE_SELF & ~uint(PVR_LANE_COUNT - 1)) | (i & uint(PVR_LANE_COUNT - 1))];
   return got;
}

pvr_ballot pvr_lane_ballot(bool c)
{
   uint base = PVR_LANE_SELF & ~uint(PVR_LANE_COUNT - 1);
   uint bits = 0u;
   uint i;
   PVR_LANE_SYNC();
   pvr_lane_xchg[PVR_LANE_SELF] = c ? 1u : 0u;
   PVR_LANE_SYNC();
   for (i = 0u; i < uint(PVR_LANE_COUNT); i++)
      bits |= pvr_lane_xchg[base + i] << i;
   return PVR_BALLOT_OF(bits);
}

uint pvr_lane_prefix_count(bool c)
{
   pvr_ballot b = pvr_lane_ballot(c);
   return PVR_BITCOUNT(b.x & ((1u << pvr_lane_id()) - 1u));
}

#define LANE_ID() pvr_lane_id()
#define LANE_BALLOT(c) pvr_lane_ballot(c)
#define LANE_PREFIX_COUNT(c) pvr_lane_prefix_count(c)
#define LANE_SHUFFLE(v, i) pvr_lane_shuffle(v, i)
#define LANE_SHUFFLE_XOR(v, m) pvr_lane_shuffle(v, pvr_lane_id() ^ (m))
/* (every lane takes part, so the first is lane 0) */
#define LANE_FIRST(v) pvr_lane_shuffle(v, 0u)

#else

/* ---- Lane operations: C, over an array of a group's lanes ---- */

/* what lane @lane gets from LANE_SHUFFLE_XOR(v, @mask) */
PVR_FN pvr_u32 pvr_lane_c_shuffle_xor(const pvr_u32 *lanes, pvr_u32 lane, pvr_u32 mask)
{
   return lanes[lane ^ mask];
}

/* LANE_BALLOT(c).x, of up to 32 lanes */
PVR_FN pvr_u32 pvr_lane_c_ballot(const pvr_u32 *cond, pvr_u32 count)
{
   pvr_u32 bits = 0, i;
   for (i = 0; i < count; i++)
      if (cond[i])
         bits |= 1u << i;
   return bits;
}

/* LANE_PREFIX_COUNT(c) for lane @lane */
PVR_FN pvr_u32 pvr_lane_c_prefix_count(const pvr_u32 *cond, pvr_u32 count, pvr_u32 lane)
{
   pvr_u32 bits = pvr_lane_c_ballot(cond, count) & ((1u << lane) - 1u), n = 0;
   for (; bits != 0; bits &= bits - 1)
      n++;
   return n;
}

#endif

#endif
