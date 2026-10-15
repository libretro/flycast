/*
	TA-VTX handling

	Parsing of the TA stream and generation of vertex data !
*/
#include "ta.h"
#include "bg_plane.h"
#include "ta_ctx.h"
#include "pvr_mem.h"
#include "Renderer_if.h"
#include "rend/tile/pvr_tile.h"
#include "rend/ta_gpu.h"
#include "rend/sorter.h"
#include "rend/shadows.h"
#include <retro_atomic.h>

/* The larger and the smaller of two, as std::max() and std::min() have
 * them (which of two equal ones, and what a NaN does, included). */
#define ta_max(a, b) ((a) < (b) ? (b) : (a))
#define ta_min(a, b) ((b) < (a) ? (b) : (a))

#include <cmath>
#include <algorithm>

// TODO/FIXME - should be moved later

u32 _pal_rev_256[4]={0};
u32 _pal_rev_16[64]={0};
u32 pal_rev_256[4]={0};
u32 pal_rev_16[64]={0};
//u32 palette_ram[1024];

u32 decoded_colors[3][65536];

extern rend_context vd_rc;
extern int screen_height;

#define TACALL DYNACALL

//cache state vars
static u32 tileclip_val = 0;

/* Whether flat strips are looked for (ta_set_flat_strips()), and the AND
 * and the OR of the bits of the depths of the strip being decoded so far,
 * for whether it is flat (PVR_FLAT_RATE_BIT()): the strip's end puts the
 * answer in its control word's single-rate bit. Looked for, the vertices
 * are decoded by another copy of the decoders, which does that; not, by
 * the ones that do not, and nothing on the way costs a thing - a
 * polygon's single-rate bit is then what the game left in that reserved
 * bit, and only the sprites' and the background's mean anything. */
static bool flat_strips_wanted;
static u32 strip_z_and = PVR_FLAT_AND_INIT;
static u32 strip_z_or = PVR_FLAT_OR_INIT;

/* (from the thread that renders, which is the one that parses) */
void ta_set_flat_strips(bool on)
{
	flat_strips_wanted = on;
}

bool ta_flat_strips(void)
{
	return flat_strips_wanted;
}

/* Whether the region array's table of tiles is built (rend_context's
 * region_tiles), from the same thread */
static bool region_table_wanted;

void ta_set_region_table(bool on)
{
	region_table_wanted = on;
}

/* A colour component given as a float, as the hardware takes it: clamped to
 * 0..1 (anything negative is 0; anything above 1, infinity and NaN are 1).
 *
 * NaN with either sign. The SH4's own NaN has the sign bit clear, and one
 * a game computes there counts as above 1; computed on this host it can
 * come out with the bit set, and was taken for negative: black where the
 * console has full brightness (upstream: Gaiamaster Kessen!'s textures). */
static inline float saturate01(float val)
{
	const s32 bits = (s32&)val;
	if (bits < 0)
		return (bits & 0x7fffffff) > 0x7f800000 ? 1.f : 0.f;
	return bits > 0x3f800000 ? 1.f : val;
}

/* ...and as eight bits. This used to go through a table of 65536 entries
 * looked up by the top 16 bits of the float, which leaves seven bits of
 * mantissa: any colour from a half up came out as the value for the start
 * of its 1/256th, one too low about half the time. */
static inline u8 float_to_satu8(float val)
{
	return (u8)(saturate01(val) * 255.f);
}

//vdec state variables
static ModTriangle* lmr;

static PolyParam* CurrentPP;
static List<PolyParam>* CurrentPPlist;

//TA state vars	
DECL_ALIGN(4) static u8 FaceBaseColor[4];
DECL_ALIGN(4) static u8 FaceOffsColor[4];
DECL_ALIGN(4) static u8 FaceBaseColor1[4];
DECL_ALIGN(4) static u8 FaceOffsColor1[4];
/* The same face colours as they were given, floats clamped to 0..1, for the
 * intensity modes: red, green, blue. */
static float FaceBaseColorF[3];
static float FaceOffsColorF[3];
static float FaceBaseColor1F[3];
static float FaceOffsColor1F[3];
static u32 SFaceBaseColor;
static u32 SFaceOffsColor;

//misc ones
const u32 ListType_None = -1;
const u32 SZ32 = 1;
const u32 SZ64 = 2;

#include "ta_structs.h"

typedef Ta_Dma* DYNACALL TaListFP(Ta_Dma* data,Ta_Dma* data_end);
typedef void TACALL TaPolyParamFP(void* ptr);

static TaListFP* TaCmd;
	
static u32 CurrentList;
static TaListFP* VertexDataFP;
// (by list type as a parameter has it, three bits: 5 to 7 are no list, and can be asked for all the same)
static bool ListIsFinished[8];

static INLINE f32 f16(u16 v)
{
	u32 z=v<<16;
	return *(f32*)&z;
}

#define vdrc vd_rc

/* For the fast vertex loop (ta_poly_data): position straight to a vertex
 * that is already there, and a packed colour turned from the PowerVR's
 * blue, green, red, alpha to red, green, blue, alpha in one word where
 * the host has its bytes in that order. */
#define vert_fast_base \
	cv->x = vtx->xyz[0]; \
	cv->y = vtx->xyz[1]; \
	cv->z = vtx->xyz[2];

#if !defined(MSB_FIRST) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#include <stddef.h>
#define TA_VERT_SSE2 1
/* (the vertex's fields where that code takes them to be) */
typedef char ta_vert_layout[(offsetof(Vertex, col) == 12 && offsetof(Vertex, vtx_spc) == 16
		&& offsetof(Vertex, u) == 20 && offsetof(Vertex, v) == 24) ? 1 : -1];
#endif

/* A colour given as four floats - alpha, red, green, blue, the order the
 * game sends them in - to the vertex's red, green, blue, alpha bytes, all
 * four at once: float_to_satu8() of each, bit for bit, for every float
 * there is. The clamp is done on the floats' bits, as saturate01() does it. */
#if defined(TA_VERT_SSE2)
#define TA_COLOUR_SIMD 1
static inline void float_argb_to_satu8(u8 *to, const void *argb)
{
	const __m128i bits = _mm_loadu_si128((const __m128i *)argb);
	/* above 1, infinity and NaN with either sign */
	const __m128i one = _mm_or_si128(_mm_cmpgt_epi32(bits, _mm_set1_epi32(0x3f800000)),
			_mm_cmpgt_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)), _mm_set1_epi32(0x7f800000)));
	/* anything else with the sign set is 0 */
	__m128i c = _mm_andnot_si128(_mm_srai_epi32(bits, 31), bits);
	c = _mm_or_si128(_mm_andnot_si128(one, c), _mm_and_si128(one, _mm_set1_epi32(0x3f800000)));
	c = _mm_cvttps_epi32(_mm_mul_ps(_mm_castsi128_ps(c), _mm_set1_ps(255.f)));
	c = _mm_shuffle_epi32(c, _MM_SHUFFLE(0, 3, 2, 1));
	c = _mm_packs_epi32(c, c);
	c = _mm_packus_epi16(c, c);
	const u32 rgba = (u32)_mm_cvtsi128_si32(c);
	memcpy(to, &rgba, 4);
}
#elif !defined(MSB_FIRST) && (defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64))
#include <arm_neon.h>
#define TA_COLOUR_SIMD 1
static inline void float_argb_to_satu8(u8 *to, const void *argb)
{
	const int32x4_t bits = vreinterpretq_s32_u8(vld1q_u8((const u8 *)argb));
	/* above 1, infinity and NaN with either sign */
	const uint32x4_t one = vorrq_u32(vcgtq_s32(bits, vdupq_n_s32(0x3f800000)),
			vcgtq_s32(vandq_s32(bits, vdupq_n_s32(0x7fffffff)), vdupq_n_s32(0x7f800000)));
	/* anything else with the sign set is 0 */
	int32x4_t c = vbicq_s32(bits, vshrq_n_s32(bits, 31));
	c = vbslq_s32(one, vdupq_n_s32(0x3f800000), c);
	const uint16x4_t n = vmovn_u32(vcvtq_u32_f32(vmulq_n_f32(vreinterpretq_f32_s32(c), 255.f)));
	const uint8x8_t b = vmovn_u16(vcombine_u16(n, n));   /* A R G B A R G B */
	const u32 rgba = vget_lane_u32(vreinterpret_u32_u8(vext_u8(b, b, 1)), 0);
	memcpy(to, &rgba, 4);
}
#endif

#ifdef MSB_FIRST
#define vert_fast_packed_color(to,src) vert_packed_color_(cv->to,vtx->src)
#else
#define vert_fast_packed_color(to,src) \
	{ \
		const u32 t = vtx->src; \
		*(u32 *)cv->to = (t & 0xff00ff00u) | ((t >> 16) & 0xffu) | ((t & 0xffu) << 16); \
	}
#endif

//Splitter function (normally ta_dma_main , modified for split dma's)

class FifoSplitter
{
   static const u32 *ta_type_lut;
	/* The vertex decoders by vertex type: [1] the ones that look for flat
	 * strips, and which of the two this frame uses */
	static TaListFP* const poly_data_luts[2][15];
	static TaListFP* const* poly_data_lut;

	static void ta_list_start(u32 new_list)
	{
		//verify(CurrentList==ListType_None);
		//verify(ListIsFinished[new_list]==false);
		//printf("Starting list %d\n",new_list);
		CurrentList=new_list;
		StartList(CurrentList);
	}

	static Ta_Dma* DYNACALL NullVertexData(Ta_Dma* data,Ta_Dma* data_end)
	{
		INFO_LOG(PVR, "TA: Invalid state, ignoring VTX data");
		return data+SZ32;
	}

	//part : 0 fill all data , 1 fill upper 32B , 2 fill lower 32B
	//Poly decoder , will be moved to pvr code
	template <u32 poly_type,u32 part>
	__forceinline
	static Ta_Dma* TACALL ta_handle_poly(Ta_Dma* data,Ta_Dma* data_end)
	{
		TA_VertexParam* vp=(TA_VertexParam*)data;
		u32 rv=0;

		if (part==2)
		{
			TaCmd=ta_main;
		}

		switch (poly_type)
		{
#define ver_32B_def(num) \
case num : {\
AppendPolyVertex##num(&vp->vtx##num);\
rv=SZ32; }\
break;

			//32b , always in one pass :)
			ver_32B_def(0);//(Non-Textured, Packed Color)
			ver_32B_def(1);//(Non-Textured, Floating Color)
			ver_32B_def(2);//(Non-Textured, Intensity)
			ver_32B_def(3);//(Textured, Packed Color)
			ver_32B_def(4);//(Textured, Packed Color, 16bit UV)
			ver_32B_def(7);//(Textured, Intensity)
			ver_32B_def(8);//(Textured, Intensity, 16bit UV)
			ver_32B_def(9);//(Non-Textured, Packed Color, with Two Volumes)
			ver_32B_def(10);//(Non-Textured, Intensity,	with Two Volumes)

#undef ver_32B_def

#define ver_64B_def(num) \
case num : {\
/*process first half*/\
	if (part!=2)\
	{\
	rv+=SZ32;\
	AppendPolyVertex##num##A(&vp->vtx##num##A);\
	}\
	/*process second half*/\
	if (part==0)\
	{\
	AppendPolyVertex##num##B(&vp->vtx##num##B);\
	rv+=SZ32;\
	}\
	else if (part==2)\
	{\
	AppendPolyVertex##num##B((TA_Vertex##num##B*)data);\
	rv+=SZ32;\
	}\
	}\
	break;


			//64b , may be on 2 pass
			ver_64B_def(5);//(Textured, Floating Color)
			ver_64B_def(6);//(Textured, Floating Color, 16bit UV)
			ver_64B_def(11);//(Textured, Packed Color,	with Two Volumes)	
			ver_64B_def(12);//(Textured, Packed Color, 16bit UV, with Two Volumes)
			ver_64B_def(13);//(Textured, Intensity,	with Two Volumes)
			ver_64B_def(14);//(Textured, Intensity, 16bit UV, with Two Volumes)
#undef ver_64B_def
		}

		return data+rv;
	};

	//Code Splitter/routers
		
	//helper function for dummy dma's.Handles 32B and then switches to ta_main for next data
	static Ta_Dma* TACALL ta_dummy_32(Ta_Dma* data,Ta_Dma* data_end)
	{
		TaCmd=ta_main;
		return data+SZ32;
	}
	static Ta_Dma* TACALL ta_modvolB_32(Ta_Dma* data,Ta_Dma* data_end)
	{
		AppendModVolVertexB((TA_ModVolB*)data);
		TaCmd=ta_main;
		return data+SZ32;
	}
		
	static Ta_Dma* TACALL ta_mod_vol_data(Ta_Dma* data,Ta_Dma* data_end)
	{
		TA_VertexParam* vp=(TA_VertexParam*)data;
		if (data==data_end)
		{
			AppendModVolVertexA(&vp->mvolA);
			//32B more needed , 32B done :)
			TaCmd=ta_modvolB_32;
			return data+SZ32;
		}
		else
		{
			//all 64B done
			AppendModVolVertexA(&vp->mvolA);
			AppendModVolVertexB(&vp->mvolB);
			return data+SZ64;
		}
	}
	static Ta_Dma* TACALL ta_spriteB_data(Ta_Dma* data,Ta_Dma* data_end)
	{
		//32B more needed , 32B done :)
		TaCmd=ta_main;
			
		AppendSpriteVertexB((TA_Sprite1B*)data);

		return data+SZ32;
	}
	static Ta_Dma* TACALL ta_sprite_data(Ta_Dma* data,Ta_Dma* data_end)
	{
		//verify(data->pcw.ParaType==ParamType_Vertex_Parameter);
		if (data==data_end)
		{
			//32B more needed , 32B done :)
			TaCmd=ta_spriteB_data;

			TA_VertexParam* vp=(TA_VertexParam*)data;

			AppendSpriteVertexA(&vp->spr1A);
			return data+SZ32;
		}
		else
		{
			TA_VertexParam* vp=(TA_VertexParam*)data;

			AppendSpriteVertexA(&vp->spr1A);
			AppendSpriteVertexB(&vp->spr1B);

			return data+SZ64;
		}
	}

	/* The bits of the depth (1/w) of the vertex parameter at @data: every
	 * kind has it at the same place, in its first 32 bytes */
	static inline u32 ta_vertex_z_bits(const Ta_Dma* data)
	{
		u32 z;
		memcpy(&z, &((const TA_VertexParam*)data)->vtx0.xyz[2], sizeof(z));
		return z;
	}

	/* @flat: the strips' flatness is worked out as they go (flat_strips_wanted) */
	template <u32 poly_type,u32 poly_size,bool flat>
	static Ta_Dma* TACALL ta_poly_data(Ta_Dma* data,Ta_Dma* data_end)
	{
					//If SZ64  && 32 bytes
#define IS_FIST_HALF ((poly_size!=SZ32) && (data==data_end))

		/* (a strip that starts here starts from nothing: whatever a strip the
		 * game never ended left is not part of it) */
		if (flat && CurrentPP != NULL && CurrentPP->first == (u32)vdrc.verts.used())
		{
			strip_z_and = PVR_FLAT_AND_INIT;
			strip_z_or = PVR_FLAT_OR_INIT;
		}

		/* The formats nearly every vertex comes in - packed colour or
		 * intensity, with or without a texture - when the vertex list has
		 * room for all there are here: the list's place and the farthest
		 * depth are kept in registers for the strip, where the loop below
		 * reads and writes them in memory for every vertex (a vertex's
		 * colours are stored as bytes, which the compiler has to take as
		 * possibly being those very variables). The same vertices come out. */
		if (poly_size == SZ32
				&& (poly_type == 0 || poly_type == 2 || poly_type == 3 || poly_type == 4 || poly_type == 7 || poly_type == 8)
				&& (ptrdiff_t)(data_end - data) < (ptrdiff_t)vdrc.verts.avail)
		{
			Vertex* const first = vdrc.verts.daty;
			Vertex* const head = vdrc.verts.head();
			Vertex* cv = first;
			PolyParam* pp = CurrentPP;
			s32 z_max = (s32&)vdrc.fZ_max;
			u32 z_and = strip_z_and;
			u32 z_or = strip_z_or;

			for (;;)
			{
				s32 z;
				bool end_of_strip;

				vert_fast<poly_type>(data, cv);
				z = (s32&)cv->z;
				if (z_max < z && z < 0x49800000)
					z_max = z;
				if (flat)
				{
					z_and &= (u32)z;
					z_or |= (u32)z;
				}
				cv++;
				end_of_strip = data->pcw.EndOfStrip;
				data++;
				if (end_of_strip)
				{
					/* EndPolyStrip(), from what is in hand. The strip's
					 * parameters are read for the copy before its count is
					 * written: read after, the wide loads of the copy wait
					 * for that narrow store to reach memory. */
					const u32 used = (u32)(cv - head);
					const u32 count = used - pp->first;
					const u32 pcw = flat ? (pp->pcw.full & ~PVR_PCW_SINGLE_RATE) | PVR_FLAT_RATE_BIT(z_and, z_or) : 0;
					if (count > 0)
					{
						const PolyParam copy = *pp;
						PolyParam* d_pp = CurrentPPlist->Append();
						pp->count = count;
						if (flat)
							pp->pcw.full = pcw;	// (the copy's strip sets its own)
						*d_pp = copy;
						d_pp->first = used;
						d_pp->count = 0;
						pp = d_pp;
					}
					else
						pp->count = count;
					if (flat)
					{
						z_and = PVR_FLAT_AND_INIT;
						z_or = PVR_FLAT_OR_INIT;
					}
					/* The next strip of the same polygon follows at once,
					 * more often than not: it is gone on with here, where
					 * ta_main would only come back for it. */
					if (data > data_end || data->pcw.ParaType != ParamType_Vertex_Parameter)
						break;
				}
				else if (data > data_end)
					break;
			}

			vdrc.verts.avail -= (int)(cv - first);
			vdrc.verts.daty = cv;
			(s32&)vdrc.fZ_max = z_max;
			if (flat)
			{
				strip_z_and = z_and;
				strip_z_or = z_or;
			}
			CurrentPP = pp;
			TaCmd=ta_main;
			return data;
		}

		/* (with @flat, the strip's depths kept in registers here, and in
		 * memory only from one call to the next) */
		u32 z_and = strip_z_and;
		u32 z_or = strip_z_or;

		if (IS_FIST_HALF)
			goto fist_half;

		do
      {
         ta_handle_poly<poly_type,0>(data, 0);
         if (flat)
         {
            z_and &= ta_vertex_z_bits(data);
            z_or |= ta_vertex_z_bits(data);
         }
         if (data->pcw.EndOfStrip)
            goto strip_end;
         data += poly_size;
      } while (poly_size == SZ32 ? data <= data_end : data < data_end);
		if (flat)
		{
			strip_z_and = z_and;
			strip_z_or = z_or;
		}
			
		if (IS_FIST_HALF)
		{
		fist_half:
			ta_handle_poly<poly_type,1>(data,0);
			if (flat)
			{
				strip_z_and &= ta_vertex_z_bits(data);
				strip_z_or |= ta_vertex_z_bits(data);
			}
			if (data->pcw.EndOfStrip) EndPolyStrip<flat>();
			TaCmd=ta_handle_poly<poly_type,2>;
					
			data+=SZ32;
		}
			
		return data;

strip_end:
		if (flat)
		{
			strip_z_and = z_and;
			strip_z_or = z_or;
		}
		TaCmd=ta_main;
		if (data->pcw.EndOfStrip)
			EndPolyStrip<flat>();
		return data+poly_size;
	}

	static void TACALL AppendPolyParam2Full(void* vpp)
	{
		Ta_Dma* pp=(Ta_Dma*)vpp;

		AppendPolyParam2A((TA_PolyParam2A*)&pp[0]);
		AppendPolyParam2B((TA_PolyParam2B*)&pp[1]);
	}

	static void TACALL AppendPolyParam4Full(void* vpp)
	{
		Ta_Dma* pp=(Ta_Dma*)vpp;

		AppendPolyParam4A((TA_PolyParam4A*)&pp[0]);
		AppendPolyParam4B((TA_PolyParam4B*)&pp[1]);
	}
	//Second part of poly data
	template <int t>
	static Ta_Dma* TACALL ta_poly_B_32(Ta_Dma* data,Ta_Dma* data_end)
	{
		if (t==2)
			AppendPolyParam2B((TA_PolyParam2B*)data);
		else
			AppendPolyParam4B((TA_PolyParam4B*)data);
	
		TaCmd=ta_main;
		return data+SZ32;
	}

	//Group_En bit seems ignored, thanks p1pkin 
#define group_EN() /*if (data->pcw.Group_En) */{ TileClipMode(data->pcw.User_Clip);}
	static Ta_Dma* TACALL ta_main(Ta_Dma* data,Ta_Dma* data_end)
	{
		do
		{
			switch (data->pcw.ParaType)
			{
				//Control parameter
				//32Bw3
			case ParamType_End_Of_List:
				{

					if (CurrentList==ListType_None)
					{
						CurrentList=data->pcw.ListType;
						//printf("End_Of_List : list error\n");
					}
					else
					{
						//end of list should be all 0's ...
						EndList(CurrentList);//end a list olny if it was realy started
					}

					//printf("End list %X\n",CurrentList);
					ListIsFinished[CurrentList]=true;
					CurrentList=ListType_None;
					VertexDataFP=NullVertexData;
					data+=SZ32;
				}
				break;
				//32B
			case ParamType_User_Tile_Clip:
				{

					SetTileClip(data->data_32[3]&63,data->data_32[4]&31,data->data_32[5]&63,data->data_32[6]&31);
					data+=SZ32;
				}
				break;
				//32B
			case ParamType_Object_List_Set:
				{
					INFO_LOG(PVR, "Unsupported list type: ParamType_Object_List_Set");	// NAOMI Virtual on Oratorio Tangram

					// *cough* ignore it :p
					data+=SZ32;
				}
				break;

				//Global Parameter
				//ModVolue :32B
				//PolyType :32B/64B
			case ParamType_Polygon_or_Modifier_Volume:
				{
					group_EN();
					//Yep , C++ IS lame & limited
					#include "ta_const_df.h"
					if (CurrentList == ListType_None && data->pcw.ListType > ListType_Punch_Through)
					{
						/* A list of a type there is none of (5 to 7): there
						 * is nowhere for its polygons to go, and the first
						 * of them used to be put there all the same -
						 * through a null pointer. Passed over, as upstream
						 * does (a0b50cd41, from a crash report). */
						WARN_LOG(PVR, "Invalid TA list type %d", data->pcw.ListType);
						data += SZ32;
						break;
					}
					if (CurrentList==ListType_None)
						ta_list_start(data->pcw.ListType);	//start a list ;)

					if (IsModVolList(CurrentList))
					{
						//accept mod data
						StartModVol((TA_ModVolParam*)data);
						VertexDataFP=ta_mod_vol_data;
						data+=SZ32;
					}
					else
					{

                  u32 uid = ta_type_lut[data->pcw.obj_ctrl];
						u32 psz=uid>>30;
						u32 pdid=(u8)(uid);
						u32 ppid=(u8)(uid>>8);

						if (uid == 0xFFFFFFFF)
						{
							/* A polygon of a type there is no such thing as.
							 * Its numbers would index past the ends of the
							 * tables below: it is passed over. */
							WARN_LOG(PVR, "Invalid TA type %08x", data->pcw.full);
							data+=SZ32;
							continue;
						}
						VertexDataFP=poly_data_lut[pdid];
							

						if (data != data_end || psz==1)
						{

							//poly , 32B/64B
							ta_poly_param_lut[ppid](data);
							data+=psz;
						}
						else
						{

							//AppendPolyParam64A((TA_PolyParamA*)data);
							//64b , first part
							ta_poly_param_a_lut[ppid](data);
							//Handle next 32B ;)
							TaCmd=ta_poly_param_b_lut[ppid];
							data+=SZ32;
						}
					}
				}
				break;
				//32B
				//Sets Sprite info , and switches to ta_sprite_data function
			case ParamType_Sprite:
				{
					group_EN();
					if (CurrentList == ListType_None && data->pcw.ListType > ListType_Punch_Through)
					{
						// (as for a polygon)
						WARN_LOG(PVR, "Invalid TA list type %d", data->pcw.ListType);
						data += SZ32;
						break;
					}
					if (CurrentList==ListType_None)
						ta_list_start(data->pcw.ListType);	//start a list ;)

					VertexDataFP=ta_sprite_data;
					AppendSpriteParam((TA_SpriteParam*)data);
					data+=SZ32;
				}
				break;

				//Variable size
			case ParamType_Vertex_Parameter:
            data = VertexDataFP(data, data_end);
				break;

				//not handled
				//Assumed to be 32B
			case 3:
			case 6:
				{
					/* Passed over, which is what was meant: die() ended
					 * the emulator before the line after it was reached. */
					WARN_LOG(PVR, "Unhandled TA parameter %08x", data->pcw.full);
					data+=SZ32;
				}
				break;
			}
		}
		while (data <= data_end);
		return data;
	}

public:
	//Fill in lookup table
	FifoSplitter()
	{
		VertexDataFP = NullVertexData;
      ta_type_lut  = TaTypeLut::instance().table;
	}
	/*
	Volume,Col_Type,Texture,Offset,Gouraud,16bit_UV
	0   0   0   (0) x   invalid Polygon Type 0  Polygon Type 0
	0   0   1   x   x   0       Polygon Type 0  Polygon Type 3
	0   0   1   x   x   1       Polygon Type 0  Polygon Type 4
	0   1   0   (0) x   invalid Polygon Type 0  Polygon Type 1
	0   1   1   x   x   0       Polygon Type 0  Polygon Type 5
	0   1   1   x   x   1       Polygon Type 0  Polygon Type 6
	0   2   0   (0) x   invalid Polygon Type 1  Polygon Type 2
	0   2   1   0   x   0       Polygon Type 1  Polygon Type 7
	0   2   1   0   x   1       Polygon Type 1  Polygon Type 8
	0   2   1   1   x   0       Polygon Type 2  Polygon Type 7
	0   2   1   1   x   1       Polygon Type 2  Polygon Type 8
	0   3   0   (0) x   invalid Polygon Type 0  Polygon Type 2
	0   3   1   x   x   0       Polygon Type 0  Polygon Type 7
	0   3   1   x   x   1       Polygon Type 0  Polygon Type 8
	1   0   0   (0) x   invalid Polygon Type 3  Polygon Type 9
	1   0   1   x   x   0       Polygon Type 3  Polygon Type 11
	1   0   1   x   x   1       Polygon Type 3  Polygon Type 12
	1   2   0   (0) x   invalid Polygon Type 4  Polygon Type 10
	1   2   1   x   x   0       Polygon Type 4  Polygon Type 13
	1   2   1   x   x   1       Polygon Type 4  Polygon Type 14
	1   3   0   (0) x   invalid Polygon Type 3  Polygon Type 10
	1   3   1   x   x   0       Polygon Type 3  Polygon Type 13
	1   3   1   x   x   1       Polygon Type 3  Polygon Type 14
	Sprites :
	(0) (0) 0 (0) (0) invalid Sprite  Sprite Type 0
	(0) (0) 1  x   (0) (1)     Sprite  Sprite Type 1
	*/
	//helpers 0-14
	static u32 poly_data_type_id(PCW pcw)
	{
		if (pcw.Texture)
		{
			//textured
			if (pcw.Volume==0)
			{	//single volume
				if (pcw.Col_Type==0)
				{
					if (pcw.UV_16bit==0)
						return 3;           //(Textured, Packed Color , 32b uv)
					else
						return 4;           //(Textured, Packed Color , 16b uv)
				}
				else if (pcw.Col_Type==1)
				{
					if (pcw.UV_16bit==0)
						return 5;           //(Textured, Floating Color , 32b uv)
					else
						return 6;           //(Textured, Floating Color , 16b uv)
				}
				else
				{
					if (pcw.UV_16bit==0)
						return 7;           //(Textured, Intensity , 32b uv)
					else
						return 8;           //(Textured, Intensity , 16b uv)
				}
			}
			else
			{
				//two volumes
				if (pcw.Col_Type==0)
				{
					if (pcw.UV_16bit==0)
						return 11;          //(Textured, Packed Color, with Two Volumes)	

					else
						return 12;          //(Textured, Packed Color, 16bit UV, with Two Volumes)

				}
				else if (pcw.Col_Type==1)
				{
					//die ("invalid");
					return 0xFFFFFFFF;
				}
				else
				{
					if (pcw.UV_16bit==0)
						return 13;          //(Textured, Intensity, with Two Volumes)	

					else
						return 14;          //(Textured, Intensity, 16bit UV, with Two Volumes)
				}
			}
		}
		else
		{
			//non textured
			if (pcw.Volume==0)
			{	//single volume
				if (pcw.Col_Type==0)
					return 0;               //(Non-Textured, Packed Color)
				else if (pcw.Col_Type==1)
					return 1;               //(Non-Textured, Floating Color)
				else
					return 2;               //(Non-Textured, Intensity)
			}
			else
			{
				//two volumes
				if (pcw.Col_Type==0)
					return 9;               //(Non-Textured, Packed Color, with Two Volumes)
				else if (pcw.Col_Type==1)
				{
					//die ("invalid");
					return 0xFFFFFFFF;
				}
				else
				{
					return 10;              //(Non-Textured, Intensity, with Two Volumes)
				}
			}
		}
	}
	//0-4 | 0x80
	static u32 poly_header_type_size(PCW pcw)
	{
		if (pcw.Volume == 0)
		{
			if ( pcw.Col_Type<2 ) //0,1
			{
				return 0  | 0;              //Polygon Type 0 -- SZ32
			}
			else if ( pcw.Col_Type == 2 )
			{
				if (pcw.Texture)
				{
					if (pcw.Offset)
					{
						return 2 | 0x80;    //Polygon Type 2 -- SZ64
					}
					else
					{
						return 1 | 0;       //Polygon Type 1 -- SZ32
					}
				}
				else
				{
					return 1 | 0;           //Polygon Type 1 -- SZ32
				}
			}
			else	//col_type ==3
			{
				return 0 | 0;               //Polygon Type 0 -- SZ32
			}
		}
		else
		{
			if ( pcw.Col_Type==0 ) //0
			{
				return 3 | 0;              //Polygon Type 3 -- SZ32
			}
			else if ( pcw.Col_Type==2 ) //2
			{
				return 4 | 0x80;           //Polygon Type 4 -- SZ64
			}
			else if ( pcw.Col_Type==3 ) //3
			{
				return 3 | 0;              //Polygon Type 3 -- SZ32
			}
			else
			{
				return 0xFFDDEEAA;//die ("data->pcw.Col_Type==1 && volume ==1");
			}
		}
	}


	void vdec_init()
	{
		VDECInit();
		TaCmd = ta_main;
		CurrentList = ListType_None;
		ListIsFinished[0] = ListIsFinished[1] = ListIsFinished[2] = ListIsFinished[3] = ListIsFinished[4] = false;
		VertexDataFP = NullVertexData;
		memset(FaceBaseColor, 0xff, sizeof(FaceBaseColor));
		memset(FaceOffsColor, 0xff, sizeof(FaceOffsColor));
		memset(FaceBaseColor1, 0xff, sizeof(FaceBaseColor1));
		memset(FaceOffsColor1, 0xff, sizeof(FaceOffsColor1));
		for (int i = 0; i < 3; i++)
			FaceBaseColorF[i] = FaceOffsColorF[i] = FaceBaseColor1F[i] = FaceOffsColor1F[i] = 1.f;
		SFaceBaseColor = 0;
		SFaceOffsColor = 0;
		lmr = NULL;
		CurrentPP = NULL;
		CurrentPPlist = NULL;
		poly_data_lut = poly_data_luts[flat_strips_wanted ? 1 : 0];
		strip_z_and = PVR_FLAT_AND_INIT;
		strip_z_or = PVR_FLAT_OR_INIT;
		/* Until the game gives a clipping rectangle, it is everything: 40
		 * by 15 tiles. It was whatever the last frame left, at first the
		 * one tile at the top left, and a polygon that asked to be clipped
		 * before any rectangle was given was clipped to that (upstream:
		 * Irides - Master of Blocks). */
		SetTileClip(0, 0, 39, 14);
		TileClipMode(0);
	}
		
private:
	__forceinline
		static void SetTileClip(u32 xmin,u32 ymin,u32 xmax,u32 ymax)
	{
		u32 rv=tileclip_val & 0xF0000000;
		rv|=xmin; //6 bits
		rv|=xmax<<6; //6 bits
		rv|=ymin<<12; //5 bits
		rv|=ymax<<17; //5 bits
		tileclip_val=rv;
	}

	__forceinline
		static void TileClipMode(u32 mode)
	{
		tileclip_val=(tileclip_val&(~0xF0000000)) | (mode<<28);
	}

	//list handling
	__forceinline
		static void StartList(u32 ListType)
	{
		if (ListType==ListType_Opaque)
			CurrentPPlist=&vdrc.global_param_op;
		else if (ListType==ListType_Punch_Through)
			CurrentPPlist=&vdrc.global_param_pt;
		else if (ListType==ListType_Translucent)
			CurrentPPlist=&vdrc.global_param_tr;

		CurrentPP = NULL;
	}

	__forceinline
		static void EndList(u32 ListType)
	{
		if (CurrentPP != NULL && CurrentPP->count == 0)
			CurrentPPlist->PopLast();
		CurrentPP = NULL;
		CurrentPPlist = NULL;

      if (ListType == ListType_Opaque_Modifier_Volume
            || ListType == ListType_Translucent_Modifier_Volume)
         EndModVol();
	}

	//Polys  -- update code on sprites if that gets updated too --
	template<class T>
	static void glob_param_bdc_(T* pp)
	{
		PolyParam* d_pp = CurrentPP;
		if (d_pp == NULL || d_pp->count != 0)
		{
			d_pp = CurrentPPlist->Append();
			CurrentPP = d_pp;
		}
		d_pp->first = vdrc.verts.used();
		d_pp->count = 0;

		d_pp->isp = pp->isp;
		d_pp->tsp = pp->tsp;
		d_pp->tcw = pp->tcw;
		d_pp->pcw = pp->pcw;
		d_pp->tileclip = tileclip_val;

		d_pp->texid = -1;

		if (d_pp->pcw.Texture)
			d_pp->texid = renderer->GetTexture(d_pp->tsp,d_pp->tcw);

		d_pp->tsp1.full = -1;
		d_pp->tcw1.full = -1;
		d_pp->texid1 = -1;
	}

	#define glob_param_bdc(pp) glob_param_bdc_( (TA_PolyParam0*)pp)

#ifdef TA_COLOUR_SIMD
	#define poly_float_color_(to,a,r,g,b) \
		float_argb_to_satu8(to, &(a));
#else
	#define poly_float_color_(to,a,r,g,b) \
		to[0] = float_to_satu8(r);	\
		to[1] = float_to_satu8(g);	\
		to[2] = float_to_satu8(b);	\
		to[3] = float_to_satu8(a);
#endif


	#define poly_float_color(to,src) \
		poly_float_color_(to,pp->src##A,pp->src##R,pp->src##G,pp->src##B) \
		to##F[0] = saturate01(pp->src##R); \
		to##F[1] = saturate01(pp->src##G); \
		to##F[2] = saturate01(pp->src##B);

	// Poly param handling

	// Packed/Floating Color
	__forceinline
		static void TACALL AppendPolyParam0(void* vpp)
	{
		TA_PolyParam0* pp=(TA_PolyParam0*)vpp;

		glob_param_bdc(pp);
	}

	// Intensity, no Offset Color
	__forceinline
		static void TACALL AppendPolyParam1(void* vpp)
	{
		TA_PolyParam1* pp=(TA_PolyParam1*)vpp;

		glob_param_bdc(pp);
		poly_float_color(FaceBaseColor,FaceColor);
	}

	// Intensity, use Offset Color
	__forceinline
		static void TACALL AppendPolyParam2A(void* vpp)
	{
		TA_PolyParam2A* pp=(TA_PolyParam2A*)vpp;

		glob_param_bdc(pp);
	}
	__forceinline
		static void TACALL AppendPolyParam2B(void* vpp)
	{
		TA_PolyParam2B* pp=(TA_PolyParam2B*)vpp;

		poly_float_color(FaceBaseColor,FaceColor);
		poly_float_color(FaceOffsColor,FaceOffset);
	}

	// Packed Color, with Two Volumes
	__forceinline
		static void TACALL AppendPolyParam3(void* vpp)
	{
		TA_PolyParam3* pp=(TA_PolyParam3*)vpp;

		glob_param_bdc(pp);

		CurrentPP->tsp1.full = pp->tsp1.full;
		CurrentPP->tcw1.full = pp->tcw1.full;
		if (pp->pcw.Texture)
		   CurrentPP->texid1 = renderer->GetTexture(pp->tsp1, pp->tcw1, 1);
	}

	// Intensity, with Two Volumes
	__forceinline
		static void TACALL AppendPolyParam4A(void* vpp)
	{
		TA_PolyParam4A* pp=(TA_PolyParam4A*)vpp;

		glob_param_bdc(pp);

		CurrentPP->tsp1.full = pp->tsp1.full;
		CurrentPP->tcw1.full = pp->tcw1.full;
		if (pp->pcw.Texture)
		   CurrentPP->texid1 = renderer->GetTexture(pp->tsp1, pp->tcw1, 1);
	}
	__forceinline
		static void TACALL AppendPolyParam4B(void* vpp)
	{
		TA_PolyParam4B* pp=(TA_PolyParam4B*)vpp;

		poly_float_color(FaceBaseColor,FaceColor0);
		poly_float_color(FaceBaseColor1, FaceColor1);
	}

	//Poly Strip handling
	template <bool flat>
	__forceinline
		static void EndPolyStrip()
	{
		PolyParam* const pp = CurrentPP;

		CurrentPP->count = vdrc.verts.used() - CurrentPP->first;

		if (CurrentPP->count > 0)
		{
			PolyParam* d_pp = CurrentPPlist->Append();
			*d_pp = *CurrentPP;
			CurrentPP = d_pp;
			d_pp->first = vdrc.verts.used();
			d_pp->count = 0;
		}
		/* (after the copy, whose wide loads would otherwise wait for this
		 * narrow store to reach memory; the copy's strip sets its own) */
		if (flat)
		{
			pp->pcw.full = (pp->pcw.full & ~PVR_PCW_SINGLE_RATE) | PVR_FLAT_RATE_BIT(strip_z_and, strip_z_or);
			strip_z_and = PVR_FLAT_AND_INIT;
			strip_z_or = PVR_FLAT_OR_INIT;
		}
	}


	
	static inline void update_fz(float z)
	{
		if ((s32&)vdrc.fZ_max<(s32&)z && (s32&)z<0x49800000)
			vdrc.fZ_max=z;
	}

		//Poly Vertex handlers
		//Append vertex base
	template<class T>
	static Vertex* vert_cvt_base_(T* vtx)
	{
		f32 invW=vtx->xyz[2];
		Vertex* cv=vdrc.verts.Append();
		cv->x=vtx->xyz[0];
		cv->y=vtx->xyz[1];
		cv->z=invW;
		update_fz(invW);
		return cv;
	}

	#define vert_cvt_base Vertex* cv=vert_cvt_base_((TA_Vertex0*)vtx)

		//Resume vertex base (for B part)
	#define vert_res_base \
		Vertex* cv=vdrc.verts.LastPtr();

		//uv 16/32
	#define vert_uv_32(u_name,v_name) \
		cv->u = (vtx->u_name);\
		cv->v = (vtx->v_name);

	#define vert_uv_16(u_name,v_name) \
		cv->u = f16(vtx->u_name);\
		cv->v = f16(vtx->v_name);

   #define vert_uv1_32(u_name,v_name) \
		cv->u1 = (vtx->u_name);\
		cv->v1 = (vtx->v_name);

	#define vert_uv1_16(u_name,v_name) \
		cv->u1 = f16(vtx->u_name);\
		cv->v1 = f16(vtx->v_name);

		//Color conversions
	#define vert_packed_color_(to,src) \
		{ \
		u32 t=src; \
		to[2] = (u8)(t);t>>=8;\
		to[1] = (u8)(t);t>>=8;\
		to[0] = (u8)(t);t>>=8;\
		to[3] = (u8)(t);      \
		}

   #define vert_face_base_color1(baseint) \
		{ const float intensity = saturate01(vtx->baseint) * 255.f; \
		cv->col1[0] = (u8)(FaceBaseColor1F[0] * intensity);  \
		cv->col1[1] = (u8)(FaceBaseColor1F[1] * intensity);  \
		cv->col1[2] = (u8)(FaceBaseColor1F[2] * intensity);  \
		cv->col1[3] = FaceBaseColor1[3]; }

	#define vert_face_offs_color1(offsint) \
		{ const float intensity = saturate01(vtx->offsint) * 255.f; \
		cv->spc1[0] = (u8)(FaceOffsColor1F[0] * intensity);  \
		cv->spc1[1] = (u8)(FaceOffsColor1F[1] * intensity);  \
		cv->spc1[2] = (u8)(FaceOffsColor1F[2] * intensity);  \
		cv->spc1[3] = FaceOffsColor1[3]; }

#ifdef TA_COLOUR_SIMD
	#define vert_float_color_(to,a,r,g,b) \
		float_argb_to_satu8(to, &(a));
#else
	#define vert_float_color_(to,a,r,g,b) \
		to[0] = float_to_satu8(r); \
		to[1] = float_to_satu8(g); \
		to[2] = float_to_satu8(b); \
		to[3] = float_to_satu8(a);
#endif

		//Macros to make thins easier ;)
	#define vert_packed_color(to,src) \
		vert_packed_color_(cv->to,vtx->src);

	#define vert_float_color(to,src) \
		vert_float_color_(cv->to,vtx->src##A,vtx->src##R,vtx->src##G,vtx->src##B)

		//Intensity handling

		//Notes:
		//Alpha doesn't get intensity
		//Intensity is clamped before the mul, as well as on face color to work the same as the hardware. [Fixes red dog]
		//The face colour and the intensity are multiplied as the floats they are
		//and the product made eight bits, once. Both used to be made eight
		//bits first and multiplied as a/256 fractions, which lost up to two
		//levels on the way and could not reach 255: full intensity on a white
		//face came out 254.

	#define vert_face_base_color(baseint) \
		{ const float intensity = saturate01(vtx->baseint) * 255.f; \
		cv->col[0] = (u8)(FaceBaseColorF[0] * intensity);  \
		cv->col[1] = (u8)(FaceBaseColorF[1] * intensity);  \
		cv->col[2] = (u8)(FaceBaseColorF[2] * intensity);  \
		cv->col[3] = FaceBaseColor[3]; }

	#define vert_face_offs_color(offsint) \
		{ const float intensity = saturate01(vtx->offsint) * 255.f; \
		cv->vtx_spc[0] = (u8)(FaceOffsColorF[0] * intensity);  \
		cv->vtx_spc[1] = (u8)(FaceOffsColorF[1] * intensity);  \
		cv->vtx_spc[2] = (u8)(FaceOffsColorF[2] * intensity);  \
		cv->vtx_spc[3] = FaceOffsColor[3]; }

	//vert_float_color_(cv->vtx_spc,FaceOffsColor[3],FaceOffsColor[0]*satint/256,FaceOffsColor[1]*satint/256,FaceOffsColor[2]*satint/256); }


	//(Non-Textured, Packed Color)
	__forceinline
		static void AppendPolyVertex0(TA_Vertex0* vtx)
	{
		vert_cvt_base;

		vert_packed_color(col,BaseCol);
	}

	//(Non-Textured, Floating Color)
	__forceinline
		static void AppendPolyVertex1(TA_Vertex1* vtx)
	{
		vert_cvt_base;

		vert_float_color(col,Base);
	}

	//(Non-Textured, Intensity)
	__forceinline
		static void AppendPolyVertex2(TA_Vertex2* vtx)
	{
		vert_cvt_base;

		vert_face_base_color(BaseInt);
	}

	//(Textured, Packed Color)
	__forceinline
		static void AppendPolyVertex3(TA_Vertex3* vtx)
	{
		vert_cvt_base;

		vert_packed_color(col,BaseCol);
		vert_packed_color(vtx_spc,OffsCol);

		vert_uv_32(u,v);
	}

	//(Textured, Packed Color, 16bit UV)
	__forceinline
		static void AppendPolyVertex4(TA_Vertex4* vtx)
	{
		vert_cvt_base;

		vert_packed_color(col,BaseCol);
		vert_packed_color(vtx_spc,OffsCol);

		vert_uv_16(u,v);
	}

	//(Textured, Floating Color)
	__forceinline
		static void AppendPolyVertex5A(TA_Vertex5A* vtx)
	{
		vert_cvt_base;

		//Colors are on B

		vert_uv_32(u,v);
	}

	__forceinline
		static void AppendPolyVertex5B(TA_Vertex5B* vtx)
	{
		vert_res_base;

		vert_float_color(col,Base);
		vert_float_color(vtx_spc,Offs);
	}

	//(Textured, Floating Color, 16bit UV)
	__forceinline
		static void AppendPolyVertex6A(TA_Vertex6A* vtx)
	{
		vert_cvt_base;

		//Colors are on B

		vert_uv_16(u,v);
	}
	__forceinline
		static void AppendPolyVertex6B(TA_Vertex6B* vtx)
	{
		vert_res_base;

		vert_float_color(col,Base);
		vert_float_color(vtx_spc,Offs);
	}

	//(Textured, Intensity)
	__forceinline
		static void AppendPolyVertex7(TA_Vertex7* vtx)
	{
		vert_cvt_base;

		vert_face_base_color(BaseInt);
		vert_face_offs_color(OffsInt);

		vert_uv_32(u,v);
	}

	//(Textured, Intensity, 16bit UV)
	__forceinline
		static void AppendPolyVertex8(TA_Vertex8* vtx)
	{
		vert_cvt_base;

		vert_face_base_color(BaseInt);
		vert_face_offs_color(OffsInt);

		vert_uv_16(u,v);

	}

	/* One vertex of a 32-byte format to @cv: what AppendPolyVertex<n>() does
	 * past getting the vertex from the list and looking at its depth. */
	template <u32 poly_type>
	__forceinline
	static void vert_fast(const Ta_Dma* data, Vertex* cv)
	{
		switch (poly_type)
		{
		case 0:
			{
				const TA_Vertex0* vtx = &((const TA_VertexParam*)data)->vtx0;
				vert_fast_base;
				vert_fast_packed_color(col,BaseCol);
			}
			break;
		case 2:
			{
				const TA_Vertex2* vtx = &((const TA_VertexParam*)data)->vtx2;
				vert_fast_base;
				vert_face_base_color(BaseInt);
			}
			break;
		case 3:
#ifdef TA_VERT_SSE2
			{
				/* The format most vertices are in, and its fields lie in the
				 * block nearly as they do in the vertex: three loads and
				 * three stores. Position goes over with the word after it,
				 * which lands where the colours then go; the two colours
				 * are turned from blue, green, red, alpha to red, green,
				 * blue, alpha together. */
				const u8* from = (const u8*)data;
				const __m128i red_blue = _mm_set1_epi32(0x00ff00ff);
				const __m128i xyz = _mm_loadu_si128((const __m128i*)(from + 4));
				const __m128i uv  = _mm_loadl_epi64((const __m128i*)(from + 16));
				__m128i colours   = _mm_loadl_epi64((const __m128i*)(from + 24));
				const __m128i rb  = _mm_and_si128(colours, red_blue);

				colours = _mm_or_si128(_mm_andnot_si128(red_blue, colours),
						_mm_or_si128(_mm_slli_epi32(rb, 16), _mm_srli_epi32(rb, 16)));
				_mm_storeu_si128((__m128i*)cv, xyz);
				_mm_storel_epi64((__m128i*)cv->col, colours);
				_mm_storel_epi64((__m128i*)&cv->u, uv);
			}
#else
			{
				const TA_Vertex3* vtx = &((const TA_VertexParam*)data)->vtx3;
				vert_fast_base;
				vert_fast_packed_color(col,BaseCol);
				vert_fast_packed_color(vtx_spc,OffsCol);
				vert_uv_32(u,v);
			}
#endif
			break;
		case 4:
			{
				const TA_Vertex4* vtx = &((const TA_VertexParam*)data)->vtx4;
				vert_fast_base;
				vert_fast_packed_color(col,BaseCol);
				vert_fast_packed_color(vtx_spc,OffsCol);
				vert_uv_16(u,v);
			}
			break;
		case 7:
			{
				const TA_Vertex7* vtx = &((const TA_VertexParam*)data)->vtx7;
				vert_fast_base;
				vert_face_base_color(BaseInt);
				vert_face_offs_color(OffsInt);
				vert_uv_32(u,v);
			}
			break;
		case 8:
			{
				const TA_Vertex8* vtx = &((const TA_VertexParam*)data)->vtx8;
				vert_fast_base;
				vert_face_base_color(BaseInt);
				vert_face_offs_color(OffsInt);
				vert_uv_16(u,v);
			}
			break;
		}
	}

	//(Non-Textured, Packed Color, with Two Volumes)
	__forceinline
		static void AppendPolyVertex9(TA_Vertex9* vtx)
	{
		vert_cvt_base;

		vert_packed_color(col,BaseCol0);
		vert_packed_color(col1, BaseCol1);
	}

	//(Non-Textured, Intensity,	with Two Volumes)
	__forceinline
		static void AppendPolyVertex10(TA_Vertex10* vtx)
	{
		vert_cvt_base;

		vert_face_base_color(BaseInt0);
		vert_face_base_color1(BaseInt1);
	}

	//(Textured, Packed Color,	with Two Volumes)	
	__forceinline
		static void AppendPolyVertex11A(TA_Vertex11A* vtx)
	{
		vert_cvt_base;

		vert_packed_color(col,BaseCol0);
		vert_packed_color(vtx_spc,OffsCol0);

		vert_uv_32(u0,v0);
	}
	__forceinline
		static void AppendPolyVertex11B(TA_Vertex11B* vtx)
	{
		vert_res_base;

		vert_packed_color(col1, BaseCol1);
		vert_packed_color(spc1, OffsCol1);

		vert_uv1_32(u1, v1);
	}

	//(Textured, Packed Color, 16bit UV, with Two Volumes)
	__forceinline
		static void AppendPolyVertex12A(TA_Vertex12A* vtx)
	{
		vert_cvt_base;

		vert_packed_color(col,BaseCol0);
		vert_packed_color(vtx_spc,OffsCol0);

		vert_uv_16(u0,v0);
	}
	__forceinline
		static void AppendPolyVertex12B(TA_Vertex12B* vtx)
	{
		vert_res_base;

		vert_packed_color(col1, BaseCol1);
		vert_packed_color(spc1, OffsCol1);

		vert_uv1_16(u1, v1);
	}

	//(Textured, Intensity,	with Two Volumes)
	__forceinline
		static void AppendPolyVertex13A(TA_Vertex13A* vtx)
	{
		vert_cvt_base;

		vert_face_base_color(BaseInt0);
		vert_face_offs_color(OffsInt0);

		vert_uv_32(u0,v0);
	}
	__forceinline
		static void AppendPolyVertex13B(TA_Vertex13B* vtx)
	{
		vert_res_base;

		vert_face_base_color1(BaseInt1);
		vert_face_offs_color1(OffsInt1);

		vert_uv1_32(u1,v1);
	}

	//(Textured, Intensity, 16bit UV, with Two Volumes)
	__forceinline
		static void AppendPolyVertex14A(TA_Vertex14A* vtx)
	{
		vert_cvt_base;

		vert_face_base_color(BaseInt0);
		vert_face_offs_color(OffsInt0);

		vert_uv_16(u0,v0);
	}
	__forceinline
		static void AppendPolyVertex14B(TA_Vertex14B* vtx)
	{
		vert_res_base;

		vert_face_base_color1(BaseInt1);
		vert_face_offs_color1(OffsInt1);

		vert_uv1_16(u1, v1);
	}

	//Sprites
	__forceinline
		static void AppendSpriteParam(TA_SpriteParam* spr)
	{
		//printf("Sprite\n");
		PolyParam* d_pp=CurrentPP;
		if (CurrentPP == NULL || CurrentPP->count != 0)
		{
         if (CurrentPPlist == NULL)	// wldkickspw
				return;
			d_pp=CurrentPPlist->Append(); 
			CurrentPP=d_pp;
		}

		d_pp->first = vdrc.verts.used();
		d_pp->count=0;
		d_pp->isp=spr->isp; 
		d_pp->tsp=spr->tsp; 
		d_pp->tcw=spr->tcw; 
		d_pp->pcw=spr->pcw; 
		d_pp->pcw.full |= PVR_PCW_SINGLE_RATE;	// a sprite is drawn at a single rate
		d_pp->tileclip=tileclip_val;

		d_pp->texid = -1;
		
		if (d_pp->pcw.Texture) {
			d_pp->texid = renderer->GetTexture(d_pp->tsp,d_pp->tcw);
		}
		d_pp->tcw1.full = -1;
		d_pp->tsp1.full = -1;
		d_pp->texid1 = -1;

		SFaceBaseColor=spr->BaseCol;
		SFaceOffsColor=spr->OffsCol;

      d_pp->isp.CullMode ^= 1;
	}

	#define append_sprite(indx) \
		vert_packed_color_(cv[indx].col,SFaceBaseColor)\
		vert_packed_color_(cv[indx].vtx_spc,SFaceOffsColor)

	#define sprite_uv(indx,u_name,v_name) \
		cv[indx].u = f16(sv->u_name);\
		cv[indx].v = f16(sv->v_name);

	//Sprite Vertex Handlers
	__forceinline
		static void AppendSpriteVertexA(TA_Sprite1A* sv)
	{
		/* A sprite's vertices with no polygon to belong to: its parameter
		 * came when no list was open and was dropped. */
		if (CurrentPP == NULL)
			return;
        CurrentPP->count = 4;

		Vertex* cv = vdrc.verts.Append(4);

		//Fill static stuff
		append_sprite(0);
		append_sprite(1);
		append_sprite(2);
		append_sprite(3);

		cv[2].x=sv->x0;
		cv[2].y=sv->y0;
		cv[2].z=sv->z0;
		update_fz(sv->z0);

		cv[3].x=sv->x1;
		cv[3].y=sv->y1;
		cv[3].z=sv->z1;
		update_fz(sv->z1);

		cv[1].x=sv->x2;
	}
public:
	// (also the GPU sort's, for the depth of a sprite's corner: see ta_gpu_sort_strips())
	static void CaclulateSpritePlane(Vertex* base)
	{
		const Vertex& A=base[2];
		const Vertex& B=base[3];
		const Vertex& C=base[1];
		Vertex& P=base[0];
		//Vector AB = B-A;
		//Vector AC = C-A;
		//Vector AP = P-A;
		float AC_x=C.x-A.x,AC_y=C.y-A.y,AC_z=C.z-A.z,
			AB_x=B.x-A.x,AB_y=B.y-A.y,AB_z=B.z-A.z,
			AP_x=P.x-A.x,AP_y=P.y-A.y;

		float P_y = P.y, P_x = P.x, A_x = A.x, A_y = A.y, A_z = A.z;

		float AB_v=B.v-A.v,AB_u=B.u-A.u,
			AC_v=C.v-A.v,AC_u=C.u-A.u;

		float /*P_v,P_u,*/A_v=A.v,A_u=A.u;

		float k3 = (AC_x * AB_y - AC_y * AB_x);

		if (k3 == 0)
		{
			//throw new Exception("WTF?!");
		}

		float k2 = (AP_x * AB_y - AP_y * AB_x) / k3;

		float k1 = 0;

		if (AB_x == 0)
		{
			//if (AB_y == 0)
			//	;
			//    //throw new Exception("WTF?!");

			k1 = (P_y - A_y - k2 * AC_y) / AB_y;
		}
		else
		{
			k1 = (P_x - A_x - k2 * AC_x) / AB_x;
		}

		P.z = A_z + k1 * AB_z + k2 * AC_z;
		P.u = A_u + k1 * AB_u + k2 * AC_u;
		P.v = A_v + k1 * AB_v + k2 * AC_v;
	}
private:
	__forceinline
		static void AppendSpriteVertexB(TA_Sprite1B* sv)
	{
		if (CurrentPP == NULL)
			return;
		vert_res_base;
		cv-=3;

		cv[1].y=sv->y2;
		cv[1].z=sv->z2;
		update_fz(sv->z2);

		cv[0].x=sv->x3;
		cv[0].y=sv->y3;


		sprite_uv(2, u0,v0);
		sprite_uv(3, u1,v1);
		sprite_uv(1, u2,v2);
		//sprite_uv(0, u0,v2);//or sprite_uv(u2,v0); ?

		CaclulateSpritePlane(cv);

		update_fz(cv[0].z);

		PolyParam* d_pp = CurrentPPlist->Append();
		*d_pp = *CurrentPP;
		CurrentPP = d_pp;
		d_pp->first = vdrc.verts.used();
		d_pp->count = 0;
	}

   // Modifier Volumes Vertex handlers
   
   static void EndModVol()
	{
		List<ModifierVolumeParam> *list = NULL;
		if (CurrentList == ListType_Opaque_Modifier_Volume)
			list = &vdrc.global_param_mvo;
		else if (CurrentList == ListType_Translucent_Modifier_Volume)
			list = &vdrc.global_param_mvo_tr;
		else
			return;
		if (list->used() > 0)
		{
			ModifierVolumeParam *p = list->LastPtr();
			p->count = vdrc.modtrig.used() - p->first;
			if (p->count == 0)
				list->PopLast();

		}
	}

	//Mod Volume Vertex handlers
	static void StartModVol(TA_ModVolParam* param)
	{
      EndModVol();

      ModifierVolumeParam *p = NULL;
		if (CurrentList == ListType_Opaque_Modifier_Volume)
         p = vdrc.global_param_mvo.Append();
      else if (CurrentList == ListType_Translucent_Modifier_Volume)
			p = vdrc.global_param_mvo_tr.Append();
      else
			return;
      p->isp.full = param->isp.full;
      p->isp.VolumeLast = param->pcw.Volume != 0;
      p->first = vdrc.modtrig.used();
      p->tileclip = tileclip_val;
	}
	__forceinline
		static void AppendModVolVertexA(TA_ModVolA* mvv)
	{
	   if (CurrentList != ListType_Opaque_Modifier_Volume && CurrentList != ListType_Translucent_Modifier_Volume)
			return;
		lmr=vdrc.modtrig.Append();

		lmr->x0=mvv->x0;
		lmr->y0=mvv->y0;
		lmr->z0=mvv->z0;
		//update_fz(mvv->z0);

		lmr->x1=mvv->x1;
		lmr->y1=mvv->y1;
		lmr->z1=mvv->z1;
		//update_fz(mvv->z1);

		lmr->x2=mvv->x2;
	}

	__forceinline
		static void AppendModVolVertexB(TA_ModVolB* mvv)
	{
	   if (CurrentList != ListType_Opaque_Modifier_Volume && CurrentList != ListType_Translucent_Modifier_Volume)
		  return;
	   lmr->y2=mvv->y2;
	   lmr->z2=mvv->z2;
	   //update_fz(mvv->z2);
	}

	static void VDECInit()
	{
		vd_rc.Clear();

		//allocate storage for BG poly
		vd_rc.global_param_op.Append();
		vd_rc.verts.Append(4);
	}
};

const u32 *FifoSplitter::ta_type_lut;

#define TA_POLY_DATA_LUT(flat) { \
	ta_poly_data<0,SZ32,flat>, ta_poly_data<1,SZ32,flat>, ta_poly_data<2,SZ32,flat>, \
	ta_poly_data<3,SZ32,flat>, ta_poly_data<4,SZ32,flat>, ta_poly_data<5,SZ64,flat>, \
	ta_poly_data<6,SZ64,flat>, ta_poly_data<7,SZ32,flat>, ta_poly_data<8,SZ32,flat>, \
	ta_poly_data<9,SZ32,flat>, ta_poly_data<10,SZ32,flat>, ta_poly_data<11,SZ64,flat>, \
	ta_poly_data<12,SZ64,flat>, ta_poly_data<13,SZ64,flat>, ta_poly_data<14,SZ64,flat> }
TaListFP* const FifoSplitter::poly_data_luts[2][15] = { TA_POLY_DATA_LUT(false), TA_POLY_DATA_LUT(true) };
#undef TA_POLY_DATA_LUT
TaListFP* const* FifoSplitter::poly_data_lut = FifoSplitter::poly_data_luts[0];

TaTypeLut::TaTypeLut()
{
	for (int i = 0; i < 256; i++)
	{
		PCW pcw;
		pcw.obj_ctrl = i;
		u32 rv = FifoSplitter::poly_data_type_id(pcw);
		u32 type = FifoSplitter::poly_header_type_size(pcw);

		if (rv == 0xFFFFFFFF || type == 0xFFDDEEAA)
		{
			// no such type: see where the table is read
			table[i] = 0xFFFFFFFF;
			continue;
		}
		if (type & 0x80)
			rv |= SZ64 << 30;
		else
			rv |= SZ32 << 30;

		rv |= (type & 0x7F) << 8;

		table[i] = rv;
	}
}

static void getRegionSettings(int pass_number, RenderPass *pass);
static void getRegionTileClipping(u32& xmin, u32& xmax, u32& ymin, u32& ymax);

FifoSplitter TAFifo0;

int ta_parse_cnt = 0;

//
// Check if a vertex has NaN or huge x,y,z values
//
/* Asked of the bits, not the values: a float's bits with the sign taken off
 * go up as its size does, with infinity and the NaNs above everything, so
 * one integer comparison says "huge, infinite or not a number". And it has
 * to be the bits: on targets built with fast-math the compiler is told
 * there are no NaNs, and the std::isnan() calls that were here were dropped,
 * so that a vertex with a NaN in it went through to the renderer. */
static bool is_vertex_inf(const Vertex& vtx)
{
	// Manic Panic Ghosts needs x and y dropped from 1e25 up
	const u32 huge_xy = float_bits(1e25f);
	const u32 huge_z = float_bits(3.4e37f);
	const u32 z = float_bits(vtx.z);

	return (float_bits(vtx.x) & 0x7fffffff) > huge_xy
			|| (float_bits(vtx.y) & 0x7fffffff) > huge_xy
			/* A negative z is drawn (the base runner views in World Series
			 * Baseball 2K1): only a NaN of either sign, or a huge positive
			 * value. */
			|| (z & 0x7fffffff) > 0x7f800000
			|| (z < 0x80000000 && z > huge_z);
}

//
// Create the vertex index, eliminating invalid vertices and merging strips when possible.
//
/* The index list ran out (List<>::sig_overrun()): the frame is marked and
 * will not be drawn; what is written from here on only has to stay inside
 * the list, and it starts again at its beginning. */
static NOINLINE u32 *make_index_overrun(rend_context* ctx, u32 **out_end)
{
	u32 *start;
	ctx->idx.daty = *out_end;
	ctx->idx.avail = 0;
	start = ctx->idx.sig_overrun(0);	// (nothing taken: the caller keeps its own place)
	*out_end = start + ctx->idx.avail;
	return start;
}

#include <stddef.h>
/* (the words poly_same_state() takes two at a time are side by side) */
typedef char ta_poly_layout[(offsetof(PolyParam, tcw) == offsetof(PolyParam, tsp) + 4
		&& offsetof(PolyParam, isp) == offsetof(PolyParam, pcw) + 4
		&& offsetof(PolyParam, tcw1) == offsetof(PolyParam, tsp1) + 4) ? 1 : -1];

/* Whether two polygons are drawn in the same way, so that their strips can
 * be joined into one: the same control, depth, shading and texture words,
 * the same clipping rectangle and mode, and the same second shading and
 * texture words (those of a polygon under two volumes; all ones on any
 * other). Two words are compared at a time and there is one test for all
 * of them. The single-rate bit is left out of the control word's
 * comparison: a draw group's is its strips' together (flat_groups()). */
static INLINE bool poly_same_state(const PolyParam *a, const PolyParam *b)
{
	static const u32 ctl_mask_words[2] = { ~PVR_PCW_SINGLE_RATE, ~0u };	// (pcw, then isp)
	u64 a_tex, b_tex, a_ctl, b_ctl, a_tex1, b_tex1, ctl_mask;

	memcpy(&a_tex,  &a->tsp,  sizeof(a_tex));
	memcpy(&b_tex,  &b->tsp,  sizeof(b_tex));
	memcpy(&a_ctl,  &a->pcw,  sizeof(a_ctl));
	memcpy(&b_ctl,  &b->pcw,  sizeof(b_ctl));
	memcpy(&a_tex1, &a->tsp1, sizeof(a_tex1));
	memcpy(&b_tex1, &b->tsp1, sizeof(b_tex1));
	memcpy(&ctl_mask, ctl_mask_words, sizeof(ctl_mask));

	return ((a_tex ^ b_tex) | ((a_ctl ^ b_ctl) & ctl_mask) | (a_tex1 ^ b_tex1)
			| (u64)(a->tileclip ^ b->tileclip)) == 0;
}

/* Where flat strips are looked for: a draw group - polygons one after
 * another in a render pass's list drawn the same way (poly_same_state()),
 * which make_index() joins and the GPU's decode draws as one - is drawn at
 * a single rate only if every strip of it is flat (or a sprite, or the
 * background), and then all of its strips have the bit; strips with no
 * vertices count for nothing. The groups are the GPU's decode's (see
 * ta_gpu_prepare()), so that a frame is drawn at the same rates whichever
 * of the two made it; the translucent strips, which are drawn one by one,
 * too. One pass over the polygon parameters, none over the vertices. */
static void flat_groups(const List<PolyParam> *polys, int first, int end)
{
	PolyParam *const pp = polys->head();
	int i = first;
	while (i < end)
	{
		if (pp[i].count == 0)
		{
			i++;
			continue;
		}
		u32 bit = PVR_PCW_SINGLE_RATE;
		int j = i, last = i;
		for (; j < end; j++)
		{
			if (pp[j].count == 0)
				continue;
			if (!poly_same_state(&pp[last], &pp[j]))
				break;
			bit &= pp[j].pcw.full;
			last = j;
		}
		for (int k = i; k < j; k++)
			if (pp[k].count != 0)
				pp[k].pcw.full = (pp[k].pcw.full & ~PVR_PCW_SINGLE_RATE) | bit;
		i = j;
	}
}

static void make_index(const List<PolyParam> *polys, int first, int end, bool merge, rend_context* ctx)
{
	u32 * const indices = ctx->idx.head();
	const Vertex *vertices = ctx->verts.head();

	/* Where the next index goes and where the list ends are kept here for
	 * the whole of it, and given back to the list at the end: appending
	 * through the list reads and writes both in memory for every index. */
	u32 *out = ctx->idx.daty;
	u32 *out_end = out + ctx->idx.avail;
#define IDX_PUT(v) \
	do { \
		if (out == out_end) \
			out = make_index_overrun(ctx, &out_end); \
		*out++ = (v); \
	} while (0)

	PolyParam *last_poly = NULL;
	const PolyParam *end_poly = &polys->head()[end];
	for (PolyParam *poly = &polys->head()[first]; poly != end_poly; poly++)
	{
		int first_index;
		bool dupe_next_vtx = false;
		if (merge
				&& last_poly != NULL
				&& last_poly->count != 0	// (one that came to no indices has no last of them to repeat)
				&& poly_same_state(poly, last_poly))
		{
			const u32 last_vtx = indices[last_poly->first + last_poly->count - 1];
			IDX_PUT(last_vtx);
			dupe_next_vtx = true;
			first_index = last_poly->first;
		}
		else
		{
			last_poly = poly;
			first_index = (int)(out - indices);
		}
		int last_good_vtx = -1;
		const u32 poly_first = poly->first;
		const u32 poly_count = poly->count;
		const Vertex *vtx = &vertices[poly_first];
		for (u32 i = 0; i < poly_count; i++)
		{
			if (is_vertex_inf(vtx[i]))
			{
				while (i < poly_count - 1)
				{
					if (!is_vertex_inf(vtx[i + 1]))
					{
						// repeat last and next vertices to link strips
						if (last_good_vtx >= 0)
						{
							verify(!dupe_next_vtx);
							IDX_PUT(last_good_vtx);
							dupe_next_vtx = true;
						}
						break;
					}
					i++;
				}
			}
			else
			{
				last_good_vtx = poly_first + i;
				if (dupe_next_vtx)
				{
					IDX_PUT(last_good_vtx);
					dupe_next_vtx = false;
				}
				const u32 count = (u32)(out - indices) - first_index;
				if ((i ^ count) & 1)
					IDX_PUT(last_good_vtx);
				IDX_PUT(last_good_vtx);
			}
		}
		if (last_poly == poly)
		{
			poly->first = first_index;
			poly->count = (u32)(out - indices) - first_index;
		}
		else
		{
			last_poly->count = (u32)(out - indices) - last_poly->first;
			poly->count = 0;
		}
	}
#undef IDX_PUT
	ctx->idx.avail = (int)(out_end - out);
	ctx->idx.daty = out;
}

static void fix_texture_bleeding(const List<PolyParam> *list)
{
	const PolyParam *pp_end = list->LastPtr(0);
	const u32 *idx_base = vd_rc.idx.head();
	Vertex *vtx_base = vd_rc.verts.head();
	for (const PolyParam *pp = list->head(); pp != pp_end; pp++)
	{
		if (!pp->pcw.Texture || pp->count < 3)
			continue;
		// Find polygons that are facing the camera (constant z)
		// and only use 0 and 1 for U and V (some tolerance around 1 for SA2)
		// then apply a half-pixel correction on U and V.
		const u32 first = idx_base[pp->first];
		const u32 last = idx_base[pp->first + pp->count - 1];
		bool need_fixing = true;
		float z = 0.f;
		for (u32 idx = first; idx <= last && need_fixing; idx++)
		{
			Vertex& vtx = vtx_base[idx];

			if (vtx.u != 0.f && (vtx.u <= 0.995f || vtx.u > 1.f))
				need_fixing = false;
			else if (vtx.v != 0.f && (vtx.v <= 0.995f || vtx.v > 1.f))
				need_fixing = false;
			else if (idx == first)
				z = vtx.z;
			else if (z != vtx.z)
				need_fixing = false;
		}
		if (!need_fixing)
			continue;
		u32 tex_width = 8 << pp->tsp.TexU;
		u32 tex_height = 8 << pp->tsp.TexV;
		for (u32 idx = first; idx <= last; idx++)
		{
			Vertex& vtx = vtx_base[idx];
			if (vtx.u > 0.995f)
				vtx.u = 1.f;
			vtx.u = (0.5f + vtx.u * (tex_width - 1)) / tex_width;
			if (vtx.v > 0.995f)
				vtx.v = 1.f;
			vtx.v = (0.5f + vtx.v * (tex_height - 1)) / tex_height;
		}
	}
}

static bool ta_parse_vdrc_full(TA_context* ctx)
{
	bool rv=false;
	vd_ctx = ctx;
	vd_rc = vd_ctx->rend;

	ta_parse_cnt++;

	if (ctx->rend.isRTT || 0 == (ta_parse_cnt %  ( settings.pvr.ta_skip + 1)))
	{
		TAFifo0.vdec_init();

		bool empty_context = true;
		int op_poly_count = 0;
		int pt_poly_count = 0;
		int tr_poly_count = 0;

		PolyParam *bgpp = vd_rc.global_param_op.head();
		if (bgpp->pcw.Texture)
		{
			bgpp->texid = renderer->GetTexture(bgpp->tsp, bgpp->tcw);
			empty_context = false;
		}

		for (u32 pass = 0; pass <= ctx->tad.render_pass_count; pass++)
		{
			ctx->MarkRend(pass);
			vd_rc.proc_start = ctx->rend.proc_start;
			vd_rc.proc_end = ctx->rend.proc_end;

         Ta_Dma* ta_data     = (Ta_Dma*)vd_rc.proc_start;
         Ta_Dma* ta_data_end = (Ta_Dma*)vd_rc.proc_end - 1;

         while (ta_data <= ta_data_end)
            ta_data =TaCmd(ta_data,ta_data_end);

         if (ctx->rend.Overrun)
            break;

			/* Opaque polygons of the first render pass are not blended,
			 * whatever their blend instructions say: they are drawn as
			 * "source x 1, destination x 0". It is only in the passes that
			 * follow, a list continuation, that an opaque polygon's
			 * instructions are gone by. Upstream flycast sets them here in
			 * the same way, and the renderers go by what they find. */
			if (pass == 0)
			{
				PolyParam *pp = vd_rc.global_param_op.head();
				for (int i = vd_rc.global_param_op.used(); i > 0; i--, pp++)
				{
					pp->tsp.SrcInstr = 1;
					pp->tsp.DstInstr = 0;
				}
			}

			bool empty_pass = vd_rc.global_param_op.used() == (pass == 0 ? 0 : (int)vd_rc.render_passes.LastPtr()->op_count)
					&& vd_rc.global_param_pt.used() == (pass == 0 ? 0 : (int)vd_rc.render_passes.LastPtr()->pt_count)
					&& vd_rc.global_param_tr.used() == (pass == 0 ? 0 : (int)vd_rc.render_passes.LastPtr()->tr_count);
			empty_context = empty_context && empty_pass;

			if (pass == 0 || !empty_pass)
			{
				RenderPass *render_pass = vd_rc.render_passes.Append();
				render_pass->op_count = vd_rc.global_param_op.used();
				if (flat_strips_wanted)
				{
					flat_groups(&vd_rc.global_param_op, op_poly_count, vd_rc.global_param_op.used());
					flat_groups(&vd_rc.global_param_pt, pt_poly_count, vd_rc.global_param_pt.used());
					flat_groups(&vd_rc.global_param_tr, tr_poly_count, vd_rc.global_param_tr.used());
				}
				make_index(&vd_rc.global_param_op, op_poly_count,
						render_pass->op_count, true, &vd_rc);
				op_poly_count = render_pass->op_count;
				render_pass->mvo_count = vd_rc.global_param_mvo.used();
				render_pass->pt_count = vd_rc.global_param_pt.used();
				make_index(&vd_rc.global_param_pt, pt_poly_count,
						render_pass->pt_count, true, &vd_rc);
				pt_poly_count = render_pass->pt_count;
				render_pass->tr_count = vd_rc.global_param_tr.used();
				make_index(&vd_rc.global_param_tr, tr_poly_count,
						render_pass->tr_count, false, &vd_rc);
				tr_poly_count = render_pass->tr_count;
				render_pass->mvo_tr_count = vd_rc.global_param_mvo_tr.used();
				getRegionSettings(pass, render_pass);
			}
		}
		rv = !empty_context;
	}
	bool overrun = ctx->rend.Overrun;
	if (overrun)
		WARN_LOG(PVR, "ERROR: TA context overrun");
	else if (screen_height > 480)
	{
		fix_texture_bleeding(&vd_rc.global_param_op);
		fix_texture_bleeding(&vd_rc.global_param_pt);
		fix_texture_bleeding(&vd_rc.global_param_tr);
	}
   if (rv && !overrun)
   {
      u32 xmin, xmax, ymin, ymax;
      getRegionTileClipping(xmin, xmax, ymin, ymax);
      vd_rc.fb_X_CLIP.min = ta_max(vd_rc.fb_X_CLIP.min, xmin);
      vd_rc.fb_X_CLIP.max = ta_min(vd_rc.fb_X_CLIP.max, xmax + 31);
      vd_rc.fb_Y_CLIP.min = ta_max(vd_rc.fb_Y_CLIP.min, ymin);
      vd_rc.fb_Y_CLIP.max = ta_min(vd_rc.fb_Y_CLIP.max, ymax + 31);
   }


	vd_ctx->rend = vd_rc;
	vd_ctx = 0;

   ctx->rend.Overrun = overrun;

	return rv && !overrun;
}

/* The header pass check (at the end of the file) */
static bool ta_walk_check_frame(TA_context *ctx);
static bool ta_walk_checking;

bool ta_parse_vdrc(TA_context* ctx)
{
	if (ta_walk_checking)
		return ta_walk_check_frame(ctx);
	return ta_parse_vdrc_full(ctx);
}

/* For the check: the lists of the frame strip by strip, as the parse above
 * makes them and gives each render pass's make_index() - which then turns
 * them into ranges of indices, and joins some - kept in the order of the
 * passes (ta_walk_snap_full()). The parse's own steps, but for the indices,
 * the texture bleeding fix and the clipping rectangle; it leaves the frame
 * for the parse to make again (from the same data, the same). */
static void ta_walk_snap_full(u32 op_from, u32 pt_from, u32 tr_from);

static void ta_walk_full_strips(TA_context* ctx)
{
	const bool overrun = ctx->rend.Overrun;
	vd_ctx = ctx;
	vd_rc = vd_ctx->rend;

	TAFifo0.vdec_init();
	u32 op_poly_count = 0, pt_poly_count = 0, tr_poly_count = 0;

	PolyParam *bgpp = vd_rc.global_param_op.head();
	if (bgpp->pcw.Texture)
		bgpp->texid = renderer->GetTexture(bgpp->tsp, bgpp->tcw);

	for (u32 pass = 0; pass <= ctx->tad.render_pass_count; pass++)
	{
		ctx->MarkRend(pass);
		Ta_Dma* ta_data     = (Ta_Dma*)ctx->rend.proc_start;
		Ta_Dma* ta_data_end = (Ta_Dma*)ctx->rend.proc_end - 1;

		while (ta_data <= ta_data_end)
			ta_data = TaCmd(ta_data, ta_data_end);
		if (ctx->rend.Overrun)
			break;
		if (pass == 0)
		{
			PolyParam *pp = vd_rc.global_param_op.head();
			for (int i = vd_rc.global_param_op.used(); i > 0; i--, pp++)
			{
				pp->tsp.SrcInstr = 1;
				pp->tsp.DstInstr = 0;
			}
		}
		bool empty_pass = vd_rc.global_param_op.used() == (pass == 0 ? 0 : (int)vd_rc.render_passes.LastPtr()->op_count)
				&& vd_rc.global_param_pt.used() == (pass == 0 ? 0 : (int)vd_rc.render_passes.LastPtr()->pt_count)
				&& vd_rc.global_param_tr.used() == (pass == 0 ? 0 : (int)vd_rc.render_passes.LastPtr()->tr_count);
		if (pass == 0 || !empty_pass)
		{
			RenderPass *render_pass = vd_rc.render_passes.Append();
			ta_walk_snap_full(op_poly_count, pt_poly_count, tr_poly_count);
			render_pass->op_count = op_poly_count = vd_rc.global_param_op.used();
			render_pass->pt_count = pt_poly_count = vd_rc.global_param_pt.used();
			render_pass->tr_count = tr_poly_count = vd_rc.global_param_tr.used();
		}
	}
	vd_ctx = 0;
	ctx->rend.Overrun = overrun;
}

//decode a vertex in the native pvr format
//used for bg poly
static void decode_pvr_vertex(u32 base,u32 ptr,Vertex* cv)
{
	//ISP
	//TSP
	//TCW
	ISP_TSP isp;
	isp.full=vri(base);

	//XYZ
	//UV
	//Base Col
	//Offset Col

	//XYZ are _always_ there :)
	cv->x=vrf(ptr);
   ptr+=4;
	cv->y=vrf(ptr);
   ptr+=4;
	cv->z=vrf(ptr);
   ptr+=4;

	if (isp.Texture)
	{	//Do texture , if any
		if (isp.UV_16b)
		{
			u32 uv=vri(ptr);
			cv->u = f16((u16)uv);
			cv->v = f16((u16)(uv >> 16));
			ptr+=4;
		}
		else
		{
			cv->u=vrf(ptr);
         ptr+=4;
			cv->v=vrf(ptr);
         ptr+=4;
		}
	}

	//Color
	u32 col = vri(ptr);
	ptr += 4;
   vert_packed_color_(cv->col,col);

	if (isp.Offset)
	{
		//Intensity color (can be missing too ;p)
		u32 col=vri(ptr);
      ptr+=4;
      vert_packed_color_(cv->vtx_spc, col);
	}
}

void vtxdec_init(void)
{
}


static OnLoad ol_vtxdec(&vtxdec_init);

void FillBGP(TA_context* ctx)
{
	
	//Render pre-code
	//--BG poly
	u32 param_base=PARAM_BASE & 0xF00000;


	PolyParam* bgpp=ctx->rend.global_param_op.head();
	Vertex* cv=ctx->rend.verts.head();

   bool PSVM=FPU_SHAD_SCALE.intensity_shadow!=0; //double parameters for volumes

	//Get the strip base
	u32 strip_base=(param_base + ISP_BACKGND_T.tag_address*4)&0x7FFFFF;	//this is *not* VRAM_MASK on purpose.It fixes naomi bios and quite a few naomi games
	//i have *no* idea why that happens, they manage to set the render target over there as well
	//and that area is *not* written by the games (they instead write the params on 000000 instead of 800000)
	//could be a h/w bug ? param_base is 400000 and tag is 100000*4
	//Calculate the vertex size
	//Update: Looks like I was handling the bank interleave wrong for 16 megs ram, could that be it?

	u32 strip_vs=3 + ISP_BACKGND_T.skip;
	u32 strip_vert_num=ISP_BACKGND_T.tag_offset;

	if (PSVM && ISP_BACKGND_T.shadow)
	{
		strip_vs+=ISP_BACKGND_T.skip;//2x the size needed :p
	}
	strip_vs*=4;
	//Get vertex ptr
	u32 vertex_ptr=strip_vert_num*strip_vs+strip_base +3*4;
	//now , all the info is ready :p

	bgpp->texid = -1;

	bgpp->isp.full=vri(strip_base);
	bgpp->tsp.full=vri(strip_base+4);
	bgpp->tcw.full=vri(strip_base+8);
	bgpp->tcw1.full = -1;
	bgpp->tsp1.full = -1;
	bgpp->texid1 = -1;
	bgpp->count=4;
	bgpp->first=0;
	bgpp->tileclip=0;//disabled ! HA ~

	bgpp->isp.DepthMode=7;// -> this makes things AWFULLY slow .. sometimes
	bgpp->isp.CullMode=0;// -> so that its not culled, or somehow else hidden !
	//Set some pcw bits .. I should really get rid of pcw ..
	bgpp->pcw.UV_16bit=bgpp->isp.UV_16b;
	bgpp->pcw.Gouraud=bgpp->isp.Gouraud;
	bgpp->pcw.Offset=bgpp->isp.Offset;
	bgpp->pcw.Texture = bgpp->isp.Texture;
   bgpp->pcw.Shadow = ISP_BACKGND_T.shadow;
	bgpp->pcw.full |= PVR_PCW_SINGLE_RATE;	// so is the background

	float scale_x= (SCALER_CTL.hscale) ? 2.f:1.f;	//if AA hack the hacked pos value hacks
	for (int i=0;i<3;i++)
	{
		decode_pvr_vertex(strip_base,vertex_ptr,&cv[i]);
		vertex_ptr+=strip_vs;
	}

	f32 bg_depth = ISP_BACKGND_D.f;
	reinterpret_cast<u32&>(bg_depth) &= 0xFFFFFFF0;	// ISP_BACKGND_D has only 28 bits
	/* And a little further away than it says. The chip does not tell depths
	 * this close apart, and what a game draws at the background's own depth
	 * shows; here it lost to the background by a rounding. Upstream's
	 * figure, and its list: the sky in Xtreme Sports, Blue Stinger's (JP)
	 * intro, and the videos of many Windows CE games, which were black. */
	bg_depth = ta_max(bg_depth - 1e-6f, 1e-11f);

	/* The plane through the three vertices, over the whole screen: see
	 * bg_plane.h. What is drawn is wider than the screen by 256 pixels on
	 * either side, as before. The four corners are written over the three
	 * vertices, so what is needed of those is taken first, and no more
	 * than is needed: the texture coordinates, and the colours only where
	 * they vary - without Gouraud shading the colour is the third vertex's
	 * everywhere, and the fourth corner is all that has to be given it. */
	{
		bg_plane plane;
		if (bg_plane_init(&plane, cv[0].x, cv[0].y, cv[1].x, cv[1].y, cv[2].x, cv[2].y))
		{
			const float u[3] = { cv[0].u, cv[1].u, cv[2].u };
			const float v[3] = { cv[0].v, cv[1].v, cv[2].v };
			const bool gouraud = bgpp->pcw.Gouraud != 0;
			u8 col[3][4], spc[3][4];
			if (gouraud)
				for (int i = 0; i < 3; i++)
				{
					memcpy(col[i], cv[i].col, 4);
					memcpy(spc[i], cv[i].vtx_spc, 4);
				}
			else
			{
				memcpy(cv[3].col, cv[2].col, 4);
				memcpy(cv[3].vtx_spc, cv[2].vtx_spc, 4);
			}
			for (int i = 0; i < 4; i++)
			{
				const float x = (i & 1) ? 896.f : -256.f;
				const float y = (i & 2) ? 480.f : 0.f;
				cv[i].x = x * scale_x;
				cv[i].y = y;
				cv[i].z = bg_depth;
				cv[i].u = bg_plane_at(&plane, u[0], u[1], u[2], x, y);
				cv[i].v = bg_plane_at(&plane, v[0], v[1], v[2], x, y);
				if (gouraud)
					for (int c = 0; c < 4; c++)
					{
						cv[i].col[c] = bg_plane_colour(&plane, col[0][c], col[1][c], col[2][c], x, y);
						cv[i].vtx_spc[c] = bg_plane_colour(&plane, spc[0][c], spc[1][c], spc[2][c], x, y);
					}
			}
			return;
		}
	}
	// the three are not a plane: the picture stretched over the screen, as it always was
	f32 min_u = ta_min(cv[0].u, ta_min(cv[1].u, cv[2].u));
	f32 max_u = ta_max(cv[0].u, ta_max(cv[1].u, cv[2].u));
	if (max_u == 0.f)
		max_u = 1.f;
	const f32 diff_u = (max_u - min_u) * 0.4f;
	max_u += diff_u;
	min_u -= diff_u;
	const f32 min_v = ta_min(cv[0].v, ta_min(cv[1].v, cv[2].v));
	f32 max_v = ta_max(cv[0].v, ta_max(cv[1].v, cv[2].v));
	if (max_v == 0.f)
		max_v = 1.f;
	cv[0].x = -256.f * scale_x;
	cv[0].y = 0.f;
	cv[0].z=bg_depth;
	cv[0].u = min_u;
	cv[0].v = min_v;

	cv[1].x = 896.f * scale_x;
	cv[1].y = 0.f;
	cv[1].z=bg_depth;
	cv[1].u = max_u;
	cv[1].v = min_v;

	cv[2].x = -256.f * scale_x;
	cv[2].y = 480.f;
	cv[2].z=bg_depth;
	cv[2].u = min_u;
	cv[2].v = max_v;

	cv[3]=cv[2];
	cv[3].x = 896.f * scale_x;
	cv[3].y = 480.f;
	cv[3].u = max_u;
	cv[3].v = max_v;
}

/* @table: the entries are also kept in the frame's table of tiles */
template <bool table>
__forceinline
static void walkRegionArray(u32& xmin, u32& xmax, u32& ymin, u32& ymax)
{
   xmin = 20;
   xmax = 0;
   ymin = 15;
   ymax = 0;

   u32 addr = REGION_BASE;
   const bool type1_tile = ((FPU_PARAM_CFG >> 21) & 1) == 0;
   int tile_size = (type1_tile ? 5 : 6) * 4;
   bool empty_first_region = true;
   for (int i = type1_tile ? 4 : 5; i > 0; i--)
      if ((vri(addr + i * 4) & 0x80000000) == 0)
      {
         empty_first_region = false;
         break;
      }
   if (empty_first_region)
      addr += tile_size;

   /* The array ends at the entry marked last. A game that starts a render
    * without having written one leaves no such entry, and vri() wraps
    * around video memory, so stop once all of it has been walked: the
    * render thread must not spin on what the guest left in VRAM. */
   RegionArrayTile tile;
   u32 walked = 0;
   u32 entries = 0, entry = 0;
   do {
      tile.full = vri(addr);
      if (table)
      {
         entry = pvr_region_entry(tile.full, entry, entries == 0);
         if (entries < REGION_TILES_MAX)
            vd_rc.region_tiles[entries] = entry;
         entries++;
      }
      xmin = ta_min(xmin, tile.X);
      xmax = ta_max(xmax, tile.X);
      ymin = ta_min(ymin, tile.Y);
      ymax = ta_max(ymax, tile.Y);
      if (type1_tile && tile.PreSort)
         // Windows CE weirdness
         tile_size = 6 * 4;
      addr += tile_size;
      walked += tile_size;
   } while (!tile.LastRegion && walked < VRAM_SIZE);
   if (table)
      vd_rc.region_tile_count = entries;

   xmin *= 32;
   xmax *= 32;
   ymin *= 32;
   ymax *= 32;
}

/* The tiles the region array lists, as a rectangle of pixels; and the
 * frame's table of them, when it is wanted */
static void getRegionTileClipping(u32& xmin, u32& xmax, u32& ymin, u32& ymax)
{
   if (region_table_wanted)
      walkRegionArray<true>(xmin, xmax, ymin, ymax);
   else
      walkRegionArray<false>(xmin, xmax, ymin, ymax);
}

// Where the region array entry of a render pass is
__forceinline
static u32 getRegionTileAddr(int pass_number)
{
	u32 addr = REGION_BASE;
   const bool type1_tile   = ((FPU_PARAM_CFG >> 21) & 1) == 0;
	int tile_size           = (type1_tile ? 5 : 6) * 4;
	bool empty_first_region = true;
	for (int i = type1_tile ? 4 : 5; i > 0; i--)
		if ((vri(addr + i * 4) & 0x80000000) == 0)
		{
			empty_first_region = false;
			break;
		}
	if (empty_first_region)
		addr += tile_size;

	RegionArrayTile tile;
   tile.full = vri(addr);
	if (type1_tile && tile.PreSort)
		// Windows CE weirdness
		tile_size = 6 * 4;

	return addr + pass_number * tile_size;
}

__forceinline
static void getRegionSettings(int pass_number, RenderPass *pass)
{
	const u32 addr = getRegionTileAddr(pass_number);
	RegionArrayTile tile;
	tile.full = vri(addr);

	if (((FPU_PARAM_CFG >> 21) & 1) == 0)
		// Type 1 region header type
		pass->autosort = (ISP_FEED_CFG & 1) == 0;
	else
		// Type 2
		pass->autosort = !tile.PreSort;
	pass->z_clear = !tile.NoZClear;
	/* The region's opaque and translucent modifier volume lists can be the
	 * same list: the translucent volumes are then the opaque ones (Crazy
	 * Taxi, Daytona USA). Two empty ones are not that. */
	const u32 op_mv = vri(addr + 8);
	pass->mv_op_tr_shared = op_mv == vri(addr + 16) && (op_mv & 0x80000000) == 0;
}

/* ---- The header pass: a frame from its control words alone ----
 *
 * The CPU side of GPU-side TA processing. As the blocks come in, the
 * control-word walk (ta.cpp, ta_set_walk()) notes where each block that is
 * not a vertex is (TA_context::walk). From that table and the parameters
 * themselves, this pass makes what the full parse above makes of the
 * frame but for the vertices: the polygon parameters with their texture
 * lookups, the modifier volumes, the render passes and their region
 * settings, the clipping rectangle, and the draw groups (the polygons
 * make_index() may join). Each parameter is a header here, with the
 * blocks of its vertices, how big they are and in which format, and the
 * place its first vertex goes. A vertex is never read: the vertices
 * between two entries are counted from where the entries are.
 *
 * What it cannot know without the vertices is where strips end - the
 * full parse makes a polygon parameter of each strip - and what
 * make_index() keeps of them. ta_walk_check_frame() reads the vertices'
 * end-of-strip bits to cut its headers into strips the full parse's way,
 * and compares the two. */
#include <chrono>

#define TA_WALK_SPRITE   15		// a header's format: a sprite
#define TA_WALK_NO_GROUP 0xFFFFFFFFu	// a header's group: it has no vertices
#define TA_WALK_BG       0xFFFFFFFFu	// the header of a strip: the background's
#define TA_WALK_MODVOL   0x80000000u	// a run's header: a modifier volume's
#define TA_WALK_UNENDED  0xFFFFFFFFu	// a modifier volume's count: its end did not come

struct TaWalkHeader
{
	PolyParam pp;	// first: its first vertex; count: its vertices, all strips together
	u32 block;	// where it is in the frame's data, in 32-byte blocks
	u32 group;	// its draw group
	u8 list;	// ListType_Opaque, _Punch_Through or _Translucent
	u8 format;	// its vertices' format, 0-14, or TA_WALK_SPRITE
	u8 vblocks;	// blocks a vertex takes, 1 or 2
	u8 pass;	// its render pass
};

// Vertex blocks one after the other, all of one header
struct TaWalkRun
{
	u32 block;	// the first
	u32 blocks;
	u32 vertex;	// the first vertex they make (a modifier volume's: triangle)
	u32 header;	// whose: a header, or TA_WALK_MODVOL | a modifier volume
};

/* Polygon parameters as each render pass gives them to make_index(), in
 * the order of the passes: opaque, punch-through, translucent */
struct TaWalkStrips
{
	PolyParam *pp[3];
	u32 *header[3];	// (the header pass's) the header each is a strip of
	u32 count[3];
	u32 room[3];
};

/* What a header's vertices are coloured with besides their own words, for
 * the GPU decode: the face colours of the intensity formats as they are
 * once its parameter is in (FaceBaseColor, FaceOffsColor and their
 * second-volume twins), and a sprite's two colours */
struct TaWalkFace
{
	u32 col[4];	// base, offset, base 1, offset 1: red, green, blue, alpha bytes
	u32 f[12];	// ...and as the clamped floats' bits, red, green, blue each
	u32 sprite[2];	// a sprite's base and offset colours, as sent
};

struct TaWalkFrame
{
	// in: what the frame had before it was parsed
	PolyParam bg_in;
	FB_X_CLIP_type x_clip_in;
	FB_Y_CLIP_type y_clip_in;

	TaWalkHeader *header;
	u32 headers, header_room;
	TaWalkRun *run;
	u32 runs, run_room;
	ModifierVolumeParam *mvo[2];	// opaque, translucent
	u32 mvos[2], mvo_room;
	RenderPass pass[10];
	u32 passes;
	PolyParam bg;
	u32 verts, modtrig;
	FB_X_CLIP_type x_clip;
	FB_Y_CLIP_type y_clip;
	bool rv;

	/* For the GPU decode (gpu): each header's colours (face, room for
	 * header_room), and whether the frame is one it does not do as the
	 * full parse does (gpu_bad): a list open at the end of a render pass,
	 * or the data ending in the middle of something */
	bool gpu;
	bool gpu_bad;
	TaWalkFace *face;

	// the check's: the lists strip by strip, as the full parse has them
	PolyParam *list[3];
	u32 *list_header[3];
	u32 list_room[3];
	TaWalkStrips snap;
};

/* @strips: the vertices' end-of-strip bits are read, and the lists made
 * strip by strip as the full parse makes them (for the check); otherwise
 * a vertex is never looked at. The code is the full parse's, ta_main()
 * and its helpers, with each vertex counted where it would be decoded. */
template <bool strips>
struct TaWalker
{
	enum { NONE, POLY, SPRITE, MODVOL };	// what vertices are, as VertexDataFP has it
	enum { WHOLE, HALF_PARAM, HALF_VERTEX };	// the next block is the second half of...

	TaWalkFrame &f;
	const u8 *root;
	const u32 *entry;
	u32 entries, ei;
	const u32 *type_lut;

	u32 list;	// the list that is open (CurrentList), ListType_None for none
	int pplist;	// where polygon parameters go (CurrentPPlist): 0-2, -1 nowhere
	bool has_pp;	// there is one being filled (CurrentPP)
	bool counted;	// ...and its count is set: a sprite between its halves
	u32 pp;		// (strips) its place in its list
	u32 hdr;	// its header
	u32 kind, vblocks, half;
	u32 tileclip;
	u32 data_blocks;	// blocks the frame's buffer has
	u32 used[3];	// polygon parameters in each list: strips (strips), or headers
	u32 indexed[3];	// how many of them earlier render passes took
	u32 pass_header;	// the first header since the last render pass
	bool pass_made;	// (headers) this pass made or dropped a polygon parameter, or made vertices
	u32 next_group;
	bool fail;
	PolyParam spare;
	TaWalkFace fc;	// (gpu) the colours of the moment

	TaWalker(TaWalkFrame &frame) : f(frame) {}

	static int pp_list(u32 l)
	{
		return l == ListType_Opaque ? 0 : l == ListType_Punch_Through ? 1 : l == ListType_Translucent ? 2 : -1;
	}

	bool eos(u32 block) const
	{
		return ((const PCW *)(root + (size_t)block * 32))->EndOfStrip != 0;
	}

	// (strips) CurrentPPlist->Append()
	PolyParam *append()
	{
		const u32 l = (u32)pplist;
		if (used[l] == f.list_room[l])
		{
			fail = true;
			return &spare;
		}
		pp = used[l]++;
		f.list_header[l][pp] = hdr;
		return &f.list[l][pp];
	}

	TaWalkHeader *new_header(u32 block, u32 format, u32 vb)
	{
		if (f.headers == f.header_room)
		{
			fail = true;
			return NULL;
		}
		hdr = f.headers++;
		TaWalkHeader *h = &f.header[hdr];
		memset(h, 0, sizeof(*h));
		h->block = block;
		h->group = TA_WALK_NO_GROUP;
		h->list = (u8)(pplist == 0 ? ListType_Opaque : pplist == 1 ? ListType_Punch_Through : ListType_Translucent);
		h->format = (u8)format;
		h->vblocks = (u8)vb;
		h->pp.first = f.verts;
		return h;
	}

	// glob_param_bdc() and AppendSpriteParam(): which polygon parameter it is
	void start_pp(const PolyParam &p)
	{
		if (strips)
		{
			PolyParam *d = (!has_pp || counted) ? append() : &f.list[pplist][pp];
			*d = p;
			f.list_header[pplist][pp] = hdr;
		}
		else
			used[pplist]++;
		pass_made = true;
		has_pp = true;
		counted = false;
	}

	/* poly_float_color(): face colour set @set from the four floats alpha,
	 * red, green, blue at @w */
	void face_set(u32 set, const u32 *w)
	{
		float c[4];
		memcpy(c, w, sizeof(c));
		fc.col[set] = (u32)float_to_satu8(c[1]) | ((u32)float_to_satu8(c[2]) << 8)
				| ((u32)float_to_satu8(c[3]) << 16) | ((u32)float_to_satu8(c[0]) << 24);
		for (int i = 0; i < 3; i++)
		{
			const float v = saturate01(c[1 + i]);
			memcpy(&fc.f[set * 3 + i], &v, sizeof(v));
		}
	}

	/* The face colours of parameter @ppid at @block (AppendPolyParam1(),
	 * 2B, 4B), whose second half, if it has one, is the next block
	 * whatever comes between (ta_main() takes it as that) */
	void face_param(u32 block, u32 ppid, u32 room)
	{
		const u32 *w = (const u32 *)(root + (size_t)block * 32);
		if (ppid == 1)
			face_set(0, w + 4);
		else if (ppid == 2 || ppid == 4)
		{
			if (block + 1 >= room)
			{
				f.gpu_bad = true;
				return;
			}
			face_set(0, w + 8);
			face_set(ppid == 2 ? 1 : 2, w + 12);
		}
	}

	void poly_param(const Ta_Dma *d, u32 block, u32 ppid, u32 format, u32 vb)
	{
		if (pplist < 0)
		{
			// (there is always a list here: see ta_main())
			fail = true;
			return;
		}
		TaWalkHeader *h = new_header(block, format, vb);
		if (h == NULL)
			return;
		const TA_PolyParam0 *pp0 = (const TA_PolyParam0 *)d;
		PolyParam &p = h->pp;
		p.isp = pp0->isp;
		p.tsp = pp0->tsp;
		p.tcw = pp0->tcw;
		p.pcw = pp0->pcw;
		p.tileclip = tileclip;
		p.texid = -1;
		if (p.pcw.Texture)
			p.texid = renderer->GetTexture(p.tsp, p.tcw);
		p.tsp1.full = -1;
		p.tcw1.full = -1;
		p.texid1 = -1;
		if (ppid == 3 || ppid == 4)
		{
			// (TA_PolyParam4A has them in the same place)
			const TA_PolyParam3 *pp3 = (const TA_PolyParam3 *)d;
			p.tsp1.full = pp3->tsp1.full;
			p.tcw1.full = pp3->tcw1.full;
			if (pp3->pcw.Texture)
				p.texid1 = renderer->GetTexture(pp3->tsp1, pp3->tcw1, 1);
		}
		start_pp(p);
		if (f.gpu)
		{
			face_param(block, ppid, data_blocks);
			f.face[hdr] = fc;
		}
	}

	void sprite_param(const Ta_Dma *d, u32 block)
	{
		if ((!has_pp || counted) && pplist < 0)
			return;	// (wldkickspw: dropped, and its vertices with it)
		TaWalkHeader *h = new_header(block, TA_WALK_SPRITE, 2);
		if (h == NULL)
			return;
		const TA_SpriteParam *spr = (const TA_SpriteParam *)d;
		PolyParam &p = h->pp;
		p.isp = spr->isp;
		p.tsp = spr->tsp;
		p.tcw = spr->tcw;
		p.pcw = spr->pcw;
		p.pcw.full |= PVR_PCW_SINGLE_RATE;
		p.tileclip = tileclip;
		p.texid = -1;
		if (p.pcw.Texture)
			p.texid = renderer->GetTexture(p.tsp, p.tcw);
		p.tcw1.full = -1;
		p.tsp1.full = -1;
		p.texid1 = -1;
		p.isp.CullMode ^= 1;
		start_pp(p);
		if (f.gpu)
		{
			fc.sprite[0] = spr->BaseCol;
			fc.sprite[1] = spr->OffsCol;
			f.face[hdr] = fc;
		}
	}

	// (strips) EndPolyStrip()
	void strip_end()
	{
		PolyParam &c = f.list[pplist][pp];
		c.count = f.verts - c.first;
		if (c.count > 0)
		{
			const PolyParam copy = c;
			PolyParam *d = append();
			*d = copy;
			d->first = f.verts;
			d->count = 0;
		}
	}

	// AppendSpriteVertexB()'s new polygon parameter
	void sprite_b()
	{
		if (strips)
		{
			const PolyParam copy = f.list[pplist][pp];
			PolyParam *d = append();
			*d = copy;
			d->first = f.verts;
			d->count = 0;
		}
		counted = false;
	}

	void end_modvol()
	{
		const int m = list == ListType_Opaque_Modifier_Volume ? 0 : list == ListType_Translucent_Modifier_Volume ? 1 : -1;
		if (m < 0 || f.mvos[m] == 0)
			return;
		ModifierVolumeParam *p = &f.mvo[m][f.mvos[m] - 1];
		p->count = f.modtrig - p->first;
		if (p->count == 0)
			f.mvos[m]--;
	}

	void start_modvol(const Ta_Dma *d)
	{
		end_modvol();
		const int m = list == ListType_Opaque_Modifier_Volume ? 0 : list == ListType_Translucent_Modifier_Volume ? 1 : -1;
		if (m < 0)
			return;
		if (f.mvos[m] == f.mvo_room)
		{
			fail = true;
			return;
		}
		const TA_ModVolParam *param = (const TA_ModVolParam *)d;
		ModifierVolumeParam *p = &f.mvo[m][f.mvos[m]++];
		p->isp.full = param->isp.full;
		p->isp.VolumeLast = param->pcw.Volume != 0;
		p->first = f.modtrig;
		p->count = TA_WALK_UNENDED;
		p->tileclip = tileclip;
	}

	void end_list()
	{
		if (has_pp && !counted)
		{
			// (PopLast())
			if (strips)
				used[pplist]--;
			pass_made = true;
		}
		has_pp = false;
		pplist = -1;
		if (list == ListType_Opaque_Modifier_Volume || list == ListType_Translucent_Modifier_Volume)
			end_modvol();
	}

	void list_start(u32 l)
	{
		list = l;
		if (pp_list(l) >= 0)
			pplist = pp_list(l);
		has_pp = false;
	}

	void add_run(u32 block, u32 blocks, u32 vertex, u32 header)
	{
		if (f.runs == f.run_room)
		{
			fail = true;
			return;
		}
		TaWalkRun *r = &f.run[f.runs++];
		r->block = block;
		r->blocks = blocks;
		r->vertex = vertex;
		r->header = header;
	}

	/* The vertex blocks from @pos up to the next entry @nb, in a render
	 * pass that ends at @end. A vertex of 64 bytes that the entry cuts in
	 * two takes the entry for its second half, as ta_main() does. */
	u32 vertices(u32 pos, u32 nb, u32 end)
	{
		const u32 n = nb - pos;
		u32 next = nb, a, blocks = n;

		if (kind == NONE)
			return nb;	// NullVertexData()
		if (kind == POLY && vblocks == 1)
		{
			add_run(pos, n, f.verts, hdr);
			if (strips)
				for (u32 b = pos; b < nb; b++)
				{
					f.verts++;
					if (eos(b))
						strip_end();
				}
			else
				f.verts += n;
			f.header[hdr].pp.count += n;
			pass_made = true;
			/* (gpu) A parameter where a strip has not ended: the full parse
			 * takes it for a vertex, the decode for what it is */
			if (f.gpu && nb < end && !eos(nb - 1))
				f.gpu_bad = true;
			return nb;
		}
		const u32 halves = (n + 1) / 2;	// first halves
		const bool cut = (n & 1) != 0;	// the last of them has its second half at nb
		if (cut)
		{
			if (nb < end)
			{
				if (ei < entries && entry[ei] == nb)
					ei++;
				next = nb + 1;
				blocks++;
			}
			else
				half = HALF_VERTEX;
		}
		if (kind == MODVOL)
		{
			const int m = list == ListType_Opaque_Modifier_Volume ? 0 : 1;
			add_run(pos, blocks, f.modtrig, TA_WALK_MODVOL | (f.mvos[m] ? f.mvos[m] - 1 : 0));
			f.modtrig += halves;
			return next;
		}
		if (kind == SPRITE)
		{
			if (!has_pp)
				return next;	// AppendSpriteVertexA() and B(): nothing
			add_run(pos, blocks, f.verts, hdr);
			for (a = 0; a < halves; a++)
			{
				if (strips)
					f.list[pplist][pp].count = 4;
				counted = true;
				f.verts += 4;
				f.header[hdr].pp.count += 4;
				if (a + 1 < halves || half != HALF_VERTEX)
					sprite_b();
			}
			pass_made = true;
			return next;
		}
		// 64-byte polygon vertices
		if (f.gpu && nb < end && !eos(cut ? nb - 1 : nb - 2))
			f.gpu_bad = true;	// (as above)
		add_run(pos, blocks, f.verts, hdr);
		if (strips)
			for (a = 0; a < halves; a++)
			{
				f.verts++;
				if (eos(pos + a * 2))
					strip_end();
			}
		else
			f.verts += halves;
		f.header[hdr].pp.count += halves;
		pass_made = true;
		return next;
	}

	void set_tile_clip(u32 xmin, u32 ymin, u32 xmax, u32 ymax)
	{
		tileclip = (tileclip & 0xF0000000) | xmin | (xmax << 6) | (ymin << 12) | (ymax << 17);
	}

	void tile_clip_mode(u32 mode)
	{
		tileclip = (tileclip & ~0xF0000000) | (mode << 28);
	}

	// The parameter at @pos, an entry
	u32 param(u32 pos, u32 end)
	{
		const Ta_Dma *d = (const Ta_Dma *)(root + (size_t)pos * 32);
		u32 uid, format, vb;

		ei++;
		switch (d->pcw.ParaType)
		{
		case ParamType_End_Of_List:
			if (list == ListType_None)
				list = d->pcw.ListType;
			else
				end_list();
			list = ListType_None;
			kind = NONE;
			return pos + 1;
		case ParamType_User_Tile_Clip:
			set_tile_clip(d->data_32[3] & 63, d->data_32[4] & 31, d->data_32[5] & 63, d->data_32[6] & 31);
			return pos + 1;
		case ParamType_Polygon_or_Modifier_Volume:
			tile_clip_mode(d->pcw.User_Clip);
			if (list == ListType_None && d->pcw.ListType > ListType_Punch_Through)
				return pos + 1;
			if (list == ListType_None)
				list_start(d->pcw.ListType);
			if (IsModVolList(list))
			{
				start_modvol(d);
				kind = MODVOL;
				vblocks = 2;
				return pos + 1;
			}
			uid = type_lut[d->pcw.obj_ctrl];
			if (uid == 0xFFFFFFFF)
				return pos + 1;
			format = (u8)uid;
			vb = format == 5 || format == 6 || (format >= 11 && format <= 14) ? 2 : 1;
			kind = POLY;
			vblocks = vb;
			poly_param(d, pos, (u8)(uid >> 8), format, vb);
			if ((uid >> 30) == SZ32)
				return pos + 1;
			if (pos + 1 < end)
			{
				if (ei < entries && entry[ei] == pos + 1)
					ei++;
				return pos + 2;
			}
			half = HALF_PARAM;
			return pos + 1;
		case ParamType_Sprite:
			tile_clip_mode(d->pcw.User_Clip);
			if (list == ListType_None && d->pcw.ListType > ListType_Punch_Through)
				return pos + 1;
			if (list == ListType_None)
				list_start(d->pcw.ListType);
			kind = SPRITE;
			vblocks = 2;
			sprite_param(d, pos);
			return pos + 1;
		case ParamType_Vertex_Parameter:
			// (the walk never notes one: the table is not this data's)
			fail = true;
			return end;
		default:
			// Object_List_Set, and the two there are none of
			return pos + 1;
		}
	}

	// ta_parse_vdrc()'s work for a render pass that ends here
	void pass_end(u32 pass, bool &empty_context)
	{
		u32 i, l;

		/* A list still open: the full parse has the polygon parameter it
		 * is filling in the pass's lists, with no vertices, and makes
		 * indices for it (make_index() links it to the strip before);
		 * and a modifier volume's count is not set. The GPU decode does
		 * not do such a frame. */
		if (list != ListType_None)
			f.gpu_bad = true;

		if (pass == 0)
		{
			// the first pass's opaque polygons are not blended
			f.bg.tsp.SrcInstr = 1;
			f.bg.tsp.DstInstr = 0;
			for (i = 0; i < f.headers; i++)
				if (f.header[i].list == ListType_Opaque)
				{
					f.header[i].pp.tsp.SrcInstr = 1;
					f.header[i].pp.tsp.DstInstr = 0;
				}
			if (strips)
				for (i = 0; i < used[0]; i++)
				{
					f.list[0][i].tsp.SrcInstr = 1;
					f.list[0][i].tsp.DstInstr = 0;
				}
		}
		bool empty_pass;
		if (pass == 0)
			empty_pass = used[0] == 0 && used[1] == 0 && used[2] == 0;
		else if (strips)
		{
			const RenderPass &last = f.pass[f.passes - 1];
			empty_pass = used[0] == last.op_count && used[1] == last.pt_count && used[2] == last.tr_count;
		}
		else
			empty_pass = !pass_made;
		pass_made = false;
		empty_context = empty_context && empty_pass;
		if (pass != 0 && empty_pass)
			return;

		RenderPass &r = f.pass[f.passes++];
		r.op_count = used[0];
		r.mvo_count = f.mvos[0];
		r.pt_count = used[1];
		r.tr_count = used[2];
		r.mvo_tr_count = f.mvos[1];
		getRegionSettings(pass, &r);

		if (strips)
			for (l = 0; l < 3; l++)
			{
				TaWalkStrips &s = f.snap;
				for (i = indexed[l]; i < used[l] && s.count[l] < s.room[l]; i++, s.count[l]++)
				{
					s.pp[l][s.count[l]] = f.list[l][i];
					s.header[l][s.count[l]] = f.list_header[l][i];
				}
				indexed[l] = used[l];
			}

		/* The draw groups: polygons one after another in a list, drawn the
		 * same way (poly_same_state()), are what make_index() joins; within
		 * a pass, and the translucent ones never. The background is the
		 * first opaque polygon of the first. */
		const PolyParam *last[2] = { f.passes == 1 ? &f.bg : NULL, NULL };
		u32 last_group[2] = { 0, 0 };
		for (i = pass_header; i < f.headers; i++)
		{
			TaWalkHeader &h = f.header[i];
			h.pass = (u8)(f.passes - 1);
			if (h.pp.count == 0)
				continue;
			l = (u32)pp_list(h.list);
			if (l == 2)
				h.group = next_group++;
			else
			{
				if (last[l] != NULL && poly_same_state(last[l], &h.pp))
					h.group = last_group[l];
				else
					h.group = next_group++;
				last[l] = &h.pp;
				last_group[l] = h.group;
			}
		}
		pass_header = f.headers;
	}

	bool run(const TA_context *ctx, u32 count)
	{
		const tad_context &tad = ctx->tad;
		bool empty_context = true;

		root = tad.thd_root;
		entry = ctx->walk;
		entries = count;
		type_lut = TaTypeLut::instance().table;
		list = ListType_None;
		pplist = -1;
		has_pp = counted = false;
		pp = hdr = 0;
		kind = NONE;
		vblocks = 1;
		half = WHOLE;
		tileclip = 0;
		set_tile_clip(0, 0, 39, 14);
		tile_clip_mode(0);
		indexed[0] = indexed[1] = indexed[2] = 0;
		pass_header = 0;
		pass_made = false;
		next_group = 1;
		fail = false;
		data_blocks = ctx->data_size / 32;
		f.gpu_bad = false;
		if (f.gpu)
		{
			// vdec_init()'s
			for (int i = 0; i < 4; i++)
				fc.col[i] = 0xFFFFFFFFu;
			for (int i = 0; i < 12; i++)
				fc.f[i] = 0x3F800000u;
			fc.sprite[0] = fc.sprite[1] = 0;
		}

		f.headers = f.runs = 0;
		f.mvos[0] = f.mvos[1] = 0;
		f.passes = 0;
		f.verts = 4;
		f.modtrig = 0;
		f.bg = f.bg_in;
		if (f.bg.pcw.Texture)
		{
			f.bg.texid = renderer->GetTexture(f.bg.tsp, f.bg.tcw);
			empty_context = false;
		}
		used[0] = 1;
		used[1] = used[2] = 0;
		if (strips)
		{
			f.list[0][0] = f.bg;
			f.list_header[0][0] = TA_WALK_BG;
			f.snap.count[0] = f.snap.count[1] = f.snap.count[2] = 0;
		}

		for (u32 pass = 0; pass <= tad.render_pass_count && !fail; pass++)
		{
			const u8 *from = pass == 0 ? tad.thd_root : tad.render_passes[pass - 1];
			const u8 *to = pass == tad.render_pass_count ? tad.End() : tad.render_passes[pass];
			const u32 start = (u32)(from - root) / 32;
			const u32 end = to > from ? (u32)(to - root) / 32 : start;
			u32 pos = start;
			u32 lo = 0, hi = entries;

			// the first entry from here on
			while (lo < hi)
			{
				const u32 mid = (lo + hi) / 2;
				if (entry[mid] < start)
					lo = mid + 1;
				else
					hi = mid;
			}
			ei = lo;
			if (half != WHOLE && pos < end)
			{
				// the second half of what the last pass ended in
				if (half == HALF_VERTEX && kind == SPRITE && has_pp)
					sprite_b();
				if (ei < entries && entry[ei] == pos)
					ei++;
				pos++;
				half = WHOLE;
			}
			while (pos < end && !fail)
			{
				const u32 nb = ei < entries && entry[ei] < end ? entry[ei] : end;
				pos = nb > pos ? vertices(pos, nb, end) : param(pos, end);
			}
			if (!fail)
				pass_end(pass, empty_context);
		}
		if (fail)
			return false;
		// (the data ends in the middle of a parameter or vertex)
		if (half != WHOLE)
			f.gpu_bad = true;

		f.rv = !empty_context;
		f.x_clip = f.x_clip_in;
		f.y_clip = f.y_clip_in;
		if (f.rv)
		{
			u32 xmin, xmax, ymin, ymax;
			walkRegionArray<false>(xmin, xmax, ymin, ymax);
			f.x_clip.min = ta_max(f.x_clip.min, xmin);
			f.x_clip.max = ta_min(f.x_clip.max, xmax + 31);
			f.y_clip.min = ta_max(f.y_clip.min, ymin);
			f.y_clip.max = ta_min(f.y_clip.max, ymax + 31);
		}
		return true;
	}
};

/* [0] the header pass's, [1] the check's, strip by strip; what the full
 * parse gave make_index(). Made by ta_walk_init() (when the check is turned
 * on), for frames of up to walk_data_size, and never grown. */
static TaWalkFrame walk_frame[2];
static TaWalkStrips walk_full;
static u32 walk_data_size;

void ta_walk_term(void)
{
	for (int i = 0; i < 2; i++)
	{
		TaWalkFrame &f = walk_frame[i];
		free(f.header);
		free(f.run);
		free(f.mvo[0]);
		free(f.mvo[1]);
		free(f.face);
		for (int l = 0; l < 3; l++)
		{
			free(f.list[l]);
			free(f.list_header[l]);
			free(f.snap.pp[l]);
			free(f.snap.header[l]);
		}
		memset(&f, 0, sizeof(f));
	}
	for (int l = 0; l < 3; l++)
		free(walk_full.pp[l]);
	memset(&walk_full, 0, sizeof(walk_full));
	walk_data_size = 0;
}

bool ta_walk_init(void)
{
	const u32 size = ta_data_size();
	const u32 scale = size / TA_DATA_SIZE;
	/* (the room of the lists in TA_context::Alloc()) */
	const u32 list_room[3] = { 16384 * scale, 5120 * scale, (STRIPS_AS_PPARAMS ? 10240 : 8192) * scale };
	bool ok = true;

	if (walk_data_size >= size)
		return true;
	ta_walk_term();
	for (int i = 0; i < 2; i++)
	{
		TaWalkFrame &f = walk_frame[i];
		f.header_room = TA_WALK_MAX * scale;
		f.run_room = TA_WALK_MAX * scale + 16;
		f.mvo_room = 4096 * scale;
		f.header = (TaWalkHeader *)malloc(f.header_room * sizeof(TaWalkHeader));
		f.run = (TaWalkRun *)malloc(f.run_room * sizeof(TaWalkRun));
		f.mvo[0] = (ModifierVolumeParam *)malloc(f.mvo_room * sizeof(ModifierVolumeParam));
		f.mvo[1] = (ModifierVolumeParam *)malloc(f.mvo_room * sizeof(ModifierVolumeParam));
		ok = ok && f.header && f.run && f.mvo[0] && f.mvo[1];
		if (i == 0)
		{
			f.face = (TaWalkFace *)malloc(f.header_room * sizeof(TaWalkFace));
			ok = ok && f.face;
		}
		if (i == 1)
			for (int l = 0; l < 3; l++)
			{
				f.list_room[l] = f.snap.room[l] = list_room[l];
				f.list[l] = (PolyParam *)malloc(list_room[l] * sizeof(PolyParam));
				f.list_header[l] = (u32 *)malloc(list_room[l] * sizeof(u32));
				f.snap.pp[l] = (PolyParam *)malloc(list_room[l] * sizeof(PolyParam));
				f.snap.header[l] = (u32 *)malloc(list_room[l] * sizeof(u32));
				ok = ok && f.list[l] && f.list_header[l] && f.snap.pp[l] && f.snap.header[l];
			}
	}
	for (int l = 0; l < 3; l++)
	{
		walk_full.room[l] = list_room[l];
		walk_full.pp[l] = (PolyParam *)malloc(list_room[l] * sizeof(PolyParam));
		ok = ok && walk_full.pp[l];
	}
	if (!ok)
	{
		ta_walk_term();
		WARN_LOG(PVR, "TA header pass: no memory for it");
		return false;
	}
	/* (touched now, so that no frame takes the page faults of their first
	 * use) */
	for (int i = 0; i < 2; i++)
	{
		TaWalkFrame &f = walk_frame[i];
		memset(f.header, 0, f.header_room * sizeof(TaWalkHeader));
		memset(f.run, 0, f.run_room * sizeof(TaWalkRun));
		memset(f.mvo[0], 0, f.mvo_room * sizeof(ModifierVolumeParam));
		memset(f.mvo[1], 0, f.mvo_room * sizeof(ModifierVolumeParam));
		if (f.face)
			memset(f.face, 0, f.header_room * sizeof(TaWalkFace));
		for (int l = 0; i == 1 && l < 3; l++)
		{
			memset(f.list[l], 0, f.list_room[l] * sizeof(PolyParam));
			memset(f.list_header[l], 0, f.list_room[l] * sizeof(u32));
			memset(f.snap.pp[l], 0, f.snap.room[l] * sizeof(PolyParam));
			memset(f.snap.header[l], 0, f.snap.room[l] * sizeof(u32));
		}
	}
	for (int l = 0; l < 3; l++)
		memset(walk_full.pp[l], 0, walk_full.room[l] * sizeof(PolyParam));
	walk_data_size = size;
	return true;
}

/* The frame's table, if it is to be trusted: how many entries */
static bool ta_walk_table(const TA_context *ctx, u32 *count)
{
	const tad_context &tad = ctx->tad;
	u32 n = ctx->walk_count;

	if (ctx->data_size > walk_data_size)
		return false;
	// (a frame with no data has nothing in its table, walked or not)
	if (tad.End() == tad.thd_root)
	{
		*count = 0;
		return true;
	}
	if (n == TA_WALK_INVALID || ctx->walk == NULL)
		return false;
	/* The lists were started again and the new data has nothing but
	 * vertices so far (all the rest would have been noted): the table is
	 * still the old data's. */
	if (ctx->walk_restart && tad.thd_data != tad.thd_root)
		n = 0;
	if (n != 0 && ctx->walk[n - 1] >= (u32)(tad.End() - tad.thd_root) / 32)
		return false;
	*count = n;
	return true;
}

template <bool strips>
static bool ta_walk_run(TA_context *ctx, TaWalkFrame &f, u32 count)
{
	TaWalker<strips> w(f);
	return w.run(ctx, count);
}

bool ta_walk_parse(TA_context *ctx)
{
	TaWalkFrame &f = walk_frame[0];
	u32 count;

	if (!ta_walk_table(ctx, &count))
		return false;
	f.bg_in = *ctx->rend.global_param_op.head();
	f.x_clip_in = ctx->rend.fb_X_CLIP;
	f.y_clip_in = ctx->rend.fb_Y_CLIP;
	return ta_walk_run<false>(ctx, f, count);
}

/* ---- The GPU decode's tables (rend/ta_gpu.h) ----
 *
 * The header pass, and what it found written as the GPU decode reads it:
 * the headers in the order make_index() goes through them (render pass,
 * then list, then as they came), each with its vertices' place in that
 * order, its format, its draw group's command slot, the words that say
 * whether two are drawn the same way, and the colours its vertices take
 * from it; the runs of vertex blocks; the modifier volume runs; the
 * segments (a list of a render pass); and the background's four vertices,
 * which FillBGP() made on this side. Nothing here reads a vertex. */
static bool ta_gpu_on;
static retro_atomic_int_t ta_gpu_want = RETRO_ATOMIC_INT_INITIALIZER(0);

void ta_set_gpu_wanted(bool on)
{
	retro_atomic_store_release_int(&ta_gpu_want, on ? 1 : 0);
}

bool ta_gpu_wanted(void)
{
	return retro_atomic_load_acquire_int(&ta_gpu_want) != 0;
}

void ta_gpu_sizes(TaGpuSizes *sz)
{
	const u32 scale = ta_data_size() / TA_DATA_SIZE;
	// (what TA_context::Alloc() gives each list, and the header pass's room)
	sz->verts = 4 * 1024 * 1024 * scale / sizeof(Vertex);
	sz->idx = 120 * 1024 * scale;
	sz->list[0] = 16384 * scale;
	sz->list[1] = 5120 * scale;
	sz->list[2] = (STRIPS_AS_PPARAMS ? 10240 : 8192) * scale;
	sz->modtrig = 16384 * scale;
	sz->headers = TA_WALK_MAX * scale + 1;
	sz->runs = TA_WALK_MAX * scale + 16;
	sz->table_words = TAG_T_WORDS + sz->runs * TAG_R_WORDS + sz->headers * TAG_H_WORDS
			+ sz->runs * TAG_M_WORDS + TAG_MAX_SEG * TAG_G_WORDS + 4 * TAG_V_WORDS
			+ TAG_SH_ENTRY + sz->headers;
}

bool ta_gpu_processing(void)
{
	return ta_gpu_on;
}

/* Whether any render pass the frame's data can have is sorted (the region
 * array's settings, which getRegionSettings() reads for each pass the
 * parse makes): a few words of video memory */
static bool ta_gpu_any_autosort(const TA_context *ctx)
{
	RenderPass r;
	for (u32 pass = 0; pass <= ctx->tad.render_pass_count; pass++)
	{
		getRegionSettings(pass, &r);
		if (r.autosort)
			return true;
	}
	return false;
}

/* ---- TA_GPU_SORT: the translucent triangles of the sorted render passes ----
 *
 * The per-triangle renderer draws a sorted pass's translucent triangles in
 * the order GenSorted() gives them, one draw for each run of them drawn the
 * same way, and only the CPU can choose the pipeline and texture of a draw.
 * So the order is worked out here, from the frame's own blocks: of each
 * translucent vertex of a sorted pass, only its position (whether it is an
 * infinite vertex, is_vertex_inf(), and its depth) and its end-of-strip
 * bit are read, and a sprite's fourth corner is solved as the parse solves
 * it. Each strip's indices are what make_index() gives it (the translucent
 * lists are never joined), its triangles are GenSorted()'s, with the
 * winding of the renderer's provoking vertex (the first, Vulkan's, with
 * TA_GPU_PROVOKE; else the last, OpenGL's), and their depths are each the
 * least of its three vertices'. A stable radix sort of those depths as
 * integers that order as the floats do (tag_fkey(); no vertex that makes
 * an index has a depth that is not a number) gives std::stable_sort()'s
 * order; the draws are made as GenSorted() makes them. The vertices,
 * colours, indices and the rest are the GPU's: the indices written here
 * name the GPU decode's vertices, by their number. Nothing
 * is allocated: the room, made by ta_gpu_sort_reserve(), is for as many
 * triangles as a frame can have indices. */
static struct TaGpuSortState
{
	u32 room;			// triangles
	u32 *tri;			// 4 words a triangle: its three indices, its header
	u64 *keys[2];			// depth key << 32 | triangle
	SortTrigDrawParam *run;
	u32 pass_first[TAG_MAX_PASS], pass_count[TAG_MAX_PASS];	// each pass's triangles (0: not sorted)
	u8 pass_buf[TAG_MAX_PASS];	// ...and which of keys[] has them sorted
	u32 pass_run[TAG_MAX_PASS], pass_runs[TAG_MAX_PASS];
	u32 tris;
	u64 ns;
	bool first_provoking;		// (TA_GPU_PROVOKE) GenSorted()'s firstProvoking
	/* Each sorted pass's depth prepass (rend/sorter.h): the triangles of
	 * its polygons that can hide what is behind them, by polygon and the
	 * polygons by what they are drawn with (porder: triangle numbers, ptris
	 * of them; their indices go after the sorted ones), and its draws */
	u32 *porder;
	u32 ptris;
	SortTrigDrawParam *prun;
	u32 pass_prun[TAG_MAX_PASS], pass_pruns[TAG_MAX_PASS];
	u32 hroom;			// headers the next three have room for
	u32 *hcount;			// a header's triangles, then where they go (all 0 between frames)
	u32 *htouched;
	std::pair<PrepassKey, u32> *hkey;
	/* The prepass's checks (TAG_K_PREPASS): each strip of the sorted
	 * passes, as TaGpuSortStrip made it - its first vertex, its vertices
	 * and its first triangle - (sroom of them; past that, no checks this
	 * frame), each triangle's place among the prepass's, and the checks:
	 * the strips in the prepass whose alpha is their vertices', for the GPU
	 * to look at them (TAG_PC_WORDS each) */
	u32 *strip;
	u32 strips, sroom;
	bool strips_full;
	u32 *tpos;
	u32 *check;
	u32 checks;
} gsort;

bool ta_gpu_sort_reserve(bool on)
{
	TaGpuSortState &g = gsort;
	TaGpuSizes sz;
	if (on)
	{
		ta_gpu_sizes(&sz);
		if (g.room >= sz.idx)
			return true;
	}
	free(g.tri);
	free(g.keys[0]);
	free(g.keys[1]);
	free(g.run);
	free(g.porder);
	free(g.prun);
	free(g.hcount);
	free(g.htouched);
	free(g.hkey);
	free(g.strip);
	free(g.tpos);
	free(g.check);
	memset(&g, 0, sizeof(g));
	if (!on)
		return true;
	/* (a frame's indices are at most its room, and a triangle takes an
	 * index or more) */
	const u32 room = sz.idx;
	g.tri = (u32 *)malloc((size_t)room * 4 * sizeof(u32));
	g.keys[0] = (u64 *)malloc((size_t)room * sizeof(u64));
	g.keys[1] = (u64 *)malloc((size_t)room * sizeof(u64));
	g.run = (SortTrigDrawParam *)malloc((size_t)room * sizeof(SortTrigDrawParam));
	const u32 hroom = sz.headers;
	g.porder = (u32 *)malloc((size_t)room * sizeof(u32));
	g.prun = (SortTrigDrawParam *)malloc((size_t)hroom * sizeof(SortTrigDrawParam));
	g.hcount = (u32 *)calloc(hroom, sizeof(u32));
	g.htouched = (u32 *)malloc((size_t)hroom * sizeof(u32));
	g.hkey = (std::pair<PrepassKey, u32> *)malloc((size_t)hroom * sizeof(std::pair<PrepassKey, u32>));
	// (a translucent strip is a polygon parameter of its own: as many as the list has room for)
	const u32 sroom = sz.list[2];
	g.strip = (u32 *)malloc((size_t)sroom * 3 * sizeof(u32));
	g.tpos = (u32 *)malloc((size_t)room * sizeof(u32));
	g.check = (u32 *)malloc((size_t)sroom * TAG_PC_WORDS * sizeof(u32));
	if (g.tri == NULL || g.keys[0] == NULL || g.keys[1] == NULL || g.run == NULL
			|| g.porder == NULL || g.prun == NULL || g.hcount == NULL || g.htouched == NULL || g.hkey == NULL
			|| g.strip == NULL || g.tpos == NULL || g.check == NULL)
	{
		ta_gpu_sort_reserve(false);
		WARN_LOG(PVR, "TA processing: no memory for sorting translucent triangles");
		return false;
	}
	// (touched now, so that no frame takes the page faults of their first use)
	memset(g.tri, 0, (size_t)room * 4 * sizeof(u32));
	memset(g.keys[0], 0, (size_t)room * sizeof(u64));
	memset(g.keys[1], 0, (size_t)room * sizeof(u64));
	memset(g.run, 0, (size_t)room * sizeof(SortTrigDrawParam));
	memset(g.porder, 0, (size_t)room * sizeof(u32));
	memset(g.prun, 0, (size_t)hroom * sizeof(SortTrigDrawParam));
	memset(g.htouched, 0, (size_t)hroom * sizeof(u32));
	memset((void *)g.hkey, 0, (size_t)hroom * sizeof(std::pair<PrepassKey, u32>));
	memset(g.strip, 0, (size_t)sroom * 3 * sizeof(u32));
	memset(g.tpos, 0, (size_t)room * sizeof(u32));
	memset(g.check, 0, (size_t)sroom * TAG_PC_WORDS * sizeof(u32));
	g.sroom = sroom;
	g.hroom = hroom;
	g.room = room;
	return true;
}

const SortTrigDrawParam *ta_gpu_sorted(u32 pass, u32 *count)
{
	if (pass >= TAG_MAX_PASS || gsort.pass_runs[pass] == 0)
	{
		*count = 0;
		return NULL;
	}
	*count = gsort.pass_runs[pass];
	return gsort.run + gsort.pass_run[pass];
}

const SortTrigDrawParam *ta_gpu_prepass(u32 pass, u32 *count)
{
	if (pass >= TAG_MAX_PASS || gsort.pass_pruns[pass] == 0)
	{
		*count = 0;
		return NULL;
	}
	*count = gsort.pass_pruns[pass];
	return gsort.prun + gsort.pass_prun[pass];
}

u32 ta_gpu_sorted_indices(void)
{
	return (gsort.tris + gsort.ptris) * 3;
}

u32 ta_gpu_prepass_checks(void)
{
	return gsort.checks;
}

u32 ta_gpu_sorted_words(void)
{
	return ta_gpu_sorted_indices() + (gsort.checks != 0 ? 1 + gsort.checks * TAG_PC_WORDS : 0);
}

void ta_gpu_sorted_write(u32 *dst)
{
	const TaGpuSortState &g = gsort;
	for (u32 p = 0; p < TAG_MAX_PASS; p++)
	{
		const u64 *k = g.keys[g.pass_buf[p]] + g.pass_first[p];
		for (u32 n = g.pass_count[p]; n != 0; n--, k++, dst += 3)
		{
			const u32 *t = &g.tri[(u32)*k * 4];
			dst[0] = t[0];
			dst[1] = t[1];
			dst[2] = t[2];
		}
	}
	for (u32 i = 0; i < g.ptris; i++, dst += 3)
	{
		const u32 *t = &g.tri[g.porder[i] * 4];
		dst[0] = t[0];
		dst[1] = t[1];
		dst[2] = t[2];
	}
	// (the prepass's checks: TAG_T_PREP)
	if (g.checks != 0)
	{
		dst[0] = g.checks;
		memcpy(dst + 1, g.check, (size_t)g.checks * TAG_PC_WORDS * sizeof(u32));
	}
}

u64 ta_gpu_sorted_ns(void)
{
	return gsort.ns;
}

/* make_index() of one strip at a time, an index at a time (no strip is
 * joined to another in a translucent list), with each index's depth key,
 * and GenSorted()'s triangles of the indices as they come */
struct TaGpuSortStrip
{
	TaGpuSortState &g;
	u32 hdr;		// the strip's header
	u32 count;		// its indices so far
	int last_good;		// the last kept vertex's place, -1 for none yet
	u32 last_good_key;
	bool dupe;		// the next kept vertex goes in twice (a link)
	bool link;		// infinite vertices have come since the last kept one
	u32 i;			// the vertex's place in the strip
	u32 h_place[2], h_key[2];	// the two indices before
	bool full;		// more triangles than the room (cannot be: see ta_gpu_sort_reserve())

	TaGpuSortStrip(TaGpuSortState &state) : g(state), full(false) {}

	void start(u32 header)
	{
		hdr = header;
		count = 0;
		last_good = -1;
		dupe = false;
		link = false;
		i = 0;
	}

	// (the prepass's checks) the strip's vertex @place: its first makes its record
	void note(u32 place)
	{
		if (i == 0)
		{
			if (g.strips == g.sroom)
			{
				g.strips_full = true;
				return;
			}
			u32 *s = &g.strip[g.strips++ * 3];
			s[0] = place;
			s[1] = 0;
			s[2] = g.tris;
		}
		if (!g.strips_full)
			g.strip[(g.strips - 1) * 3 + 1] = place - g.strip[(g.strips - 1) * 3] + 1;
	}

	void put(u32 place, u32 key)
	{
		if (count >= 2)
		{
			const u32 t = g.tris;
			if (t == g.room)
				full = true;
			else
			{
				/* GenSorted(), firstProvoking: every other triangle starts
				 * with the vertex that holds its colour; without it, the
				 * first two of every other one change places */
				u32 *d = &g.tri[t * 4];
				if (((count - 2) & 1) && g.first_provoking)
				{
					d[0] = h_place[0];
					d[1] = place;
					d[2] = h_place[1];
				}
				else if ((count - 2) & 1)
				{
					d[0] = h_place[1];
					d[1] = h_place[0];
					d[2] = place;
				}
				else
				{
					d[0] = h_place[0];
					d[1] = h_place[1];
					d[2] = place;
				}
				d[3] = hdr;
				u32 key3 = h_key[0] < h_key[1] ? h_key[0] : h_key[1];
				if (key < key3)
					key3 = key;
				g.keys[0][t] = ((u64)key3 << 32) | t;
				g.tris = t + 1;
			}
		}
		h_place[0] = h_place[1];
		h_key[0] = h_key[1];
		h_place[1] = place;
		h_key[1] = key;
		count++;
	}

	// the strip's next vertex, number @place (make_index()'s loop, its look ahead for the next kept vertex turned round)
	void vertex(u32 place, u32 x, u32 y, u32 z)
	{
		note(place);
		if (tag_inf(x, y, z) != 0)
			link = true;
		else
		{
			if (link && last_good >= 0)
			{
				put((u32)last_good, last_good_key);
				dupe = true;
			}
			link = false;
			const u32 key = tag_fkey(z);
			last_good = (int)place;
			last_good_key = key;
			if (dupe)
			{
				put(place, key);
				dupe = false;
			}
			if ((i ^ count) & 1)
				put(place, key);
			put(place, key);
		}
		i++;
	}
};

/* The translucent strips of the sorted render passes, from their blocks,
 * in the order the parse makes them: the runs as they came, and each
 * header's strips in its runs */
static bool ta_gpu_sort_strips(const TaWalkFrame &f, const u8 *root, const bool *sorted)
{
	TaGpuSortState &g = gsort;
	TaGpuSortStrip s(g);
	u32 cur_pass = TAG_MAX_PASS;
	u32 open_hdr = TA_WALK_NO_GROUP;

	for (u32 i = 0; i < f.runs; i++)
	{
		const TaWalkRun &run = f.run[i];
		if (run.header & TA_WALK_MODVOL)
			continue;
		const TaWalkHeader &h = f.header[run.header];
		if (h.list != ListType_Translucent || !sorted[h.pass])
			continue;
		if (h.pass != cur_pass)
		{
			cur_pass = h.pass;
			g.pass_first[cur_pass] = g.tris;
		}
		// (its vertices: up to the next run of vertices, or the frame's last)
		u32 j = i + 1, end = f.verts;
		while (j < f.runs && (f.run[j].header & TA_WALK_MODVOL))
			j++;
		if (j < f.runs)
			end = f.run[j].vertex;
		const u32 n = end - run.vertex;
		// (the GPU decode's vertex buffer has them in the order they came, as the parse's)
		const u32 place = run.vertex;
		if (open_hdr != run.header)
		{
			// (a strip does not go on from one header to the next: the header pass leaves such a frame to the parse)
			s.start(run.header);
			open_hdr = run.header;
		}
		if (h.format == TA_WALK_SPRITE)
		{
			for (u32 k = 0; k + 4 <= n; k += 4)
			{
				const u32 *w = (const u32 *)(root + (size_t)(run.block + 2 * (k >> 2)) * 32);
				/* the corners as AppendSpriteVertexA() and B() put them, and
				 * the first's depth as the parse works it out */
				Vertex c[4];
				memset(c, 0, sizeof(c));
				memcpy(&c[2].x, &w[1], 12);
				memcpy(&c[3].x, &w[4], 12);
				memcpy(&c[1].x, &w[7], 12);
				memcpy(&c[0].x, &w[10], 8);
				FifoSplitter::CaclulateSpritePlane(c);
				u32 z0;
				memcpy(&z0, &c[0].z, 4);
				s.start(run.header);
				s.vertex(place + k, w[10], w[11], z0);
				s.vertex(place + k + 1, w[7], w[8], w[9]);
				s.vertex(place + k + 2, w[1], w[2], w[3]);
				s.vertex(place + k + 3, w[4], w[5], w[6]);
			}
			open_hdr = TA_WALK_NO_GROUP;
		}
		else
		{
			const u32 vb = h.vblocks;
			const u32 *w = (const u32 *)(root + (size_t)run.block * 32);
			for (u32 k = 0; k < n; k++, w += vb * 8)
			{
				s.vertex(place + k, w[1], w[2], w[3]);
				if (((const PCW *)w)->EndOfStrip)
					s.start(run.header);
			}
		}
		g.pass_count[cur_pass] = g.tris - g.pass_first[cur_pass];
	}
	return !s.full;
}

/* A stable sort of @n keys from @k[0] by their top 32 bits, a digit of 11,
 * 11 and 10 bits at a time, from the lowest; a digit all of them share
 * takes no pass. Which of the two has them in the end. (A few hundred are
 * sorted whole, the triangle's number in the low half breaking ties: the
 * same order, without the passes' fixed cost.) */
static u32 ta_gpu_sort_keys(u64 *k[2], u32 first, u32 n)
{
	static u32 hist[3][2048];
	static const u32 shift[3] = { 32, 43, 54 };
	u32 at = 0;
	if (n <= 256)
	{
		std::sort(k[0] + first, k[0] + first + n);
		return 0;
	}
	memset(hist, 0, sizeof(hist));
	const u64 *a = k[0] + first;
	for (u32 i = 0; i < n; i++)
	{
		const u64 v = a[i];
		hist[0][(v >> 32) & 2047]++;
		hist[1][(v >> 43) & 2047]++;
		hist[2][(v >> 54) & 1023]++;
	}
	for (u32 d = 0; d < 3; d++)
	{
		const u64 *src = k[at] + first;
		if (hist[d][(src[0] >> shift[d]) & 2047] == n)
			continue;
		u32 sum = 0;
		for (u32 b = 0; b < 2048; b++)
		{
			const u32 c = hist[d][b];
			hist[d][b] = sum;
			sum += c;
		}
		u64 *dst = k[at ^ 1] + first;
		for (u32 i = 0; i < n; i++)
		{
			const u64 v = src[i];
			dst[hist[d][(v >> shift[d]) & 2047]++] = v;
		}
		at ^= 1;
	}
	return at;
}

/* The prepass's checks (TAG_K_PREPASS): each strip with triangles in the
 * prepass whose alpha is its vertices' (PrepassAlphaFromVertices()), for
 * the GPU to look at the vertices of and to leave out if none has an alpha
 * of 255, as GenPrepass() does. A strip's triangles there are one after
 * the other (its header's are, in the order they were made). */
static void ta_gpu_prepass_make_checks(const TaWalkFrame &f)
{
	TaGpuSortState &g = gsort;
	g.checks = 0;
	if (g.strips_full)
		return;
	for (u32 k = 0; k < g.strips; k++)
	{
		const u32 *s = &g.strip[k * 3];
		const u32 tend = k + 1 < g.strips ? g.strip[(k + 1) * 3 + 2] : g.tris;
		u32 first = TAG_NONE, n = 0;
		for (u32 t = s[2]; t < tend; t++)
		{
			const u32 *d = &g.tri[t * 4];
			if (d[0] == d[1] || d[1] == d[2] || d[0] == d[2])
				continue;
			const u32 at = g.tpos[t];
			if (at == TAG_NONE)
				break;	// (not in the prepass: none of the strip's is)
			if (first == TAG_NONE)
				first = at;
			else if (at != first + n)
			{
				first = TAG_NONE;	// (cannot be: no check, it is drawn)
				break;
			}
			n++;
		}
		if (first == TAG_NONE || n == 0 || !PrepassAlphaFromVertices(&f.header[g.tri[s[2] * 4 + 3]].pp))
			continue;
		u32 *c = &g.check[g.checks++ * TAG_PC_WORDS];
		c[TAG_PC_V] = s[0];
		c[TAG_PC_NV] = s[1];
		c[TAG_PC_T] = g.tris + first;
		c[TAG_PC_NT] = n;
	}
	/* (in the sorted indices' room, which is six words a triangle a frame
	 * can have: three a triangle sorted and drawn in the prepass take all
	 * of it only where no strip is - see ta_gpu_sort_reserve()) */
	if (ta_gpu_sorted_words() > g.room * 6)
		g.checks = 0;
}

/* The depth prepass of each sorted pass, as GenPrepass() makes it but for
 * the vertices, which are the GPU's: the polygons whose alpha is their
 * vertices' are drawn, and the GPU makes nothing of those of them none of
 * whose vertices has an alpha of 255 (ta_gpu_prepass_checks()). The
 * triangles are the sorted ones, before they were sorted. */
static void ta_gpu_prepass_frame(const TaWalkFrame &f)
{
	TaGpuSortState &g = gsort;
	const u32 none = ~0u;
	u32 npr = 0;
	g.ptris = 0;
	memset(g.pass_pruns, 0, sizeof(g.pass_pruns));
	// (no triangle in the prepass yet: TAG_NONE)
	memset(g.tpos, 0xFF, (size_t)g.tris * sizeof(u32));
	if (f.headers > g.hroom)
		return;
	for (u32 p = 0; p < f.passes && p < TAG_MAX_PASS; p++)
	{
		g.pass_prun[p] = npr;
		const u32 n = g.pass_count[p];
		const bool last = p == f.passes - 1;
		if (n == 0 || !PrepassWanted(last))
			continue;
		const u32 t0 = g.pass_first[p];
		// the polygons that can hide, and their triangles' counts
		u32 nh = 0, nt = 0;
		for (u32 t = t0; t < t0 + n; t++)
		{
			const u32 *d = &g.tri[t * 4];
			if (d[0] == d[1] || d[1] == d[2] || d[0] == d[2])
				continue;
			u32 &c = g.hcount[d[3]];
			if (c == 0)
			{
				g.htouched[nt++] = d[3];
				const PolyParam *pp = &f.header[d[3]].pp;
				if (!PrepassCandidate(pp, last))
				{
					c = none;
					continue;
				}
				g.hkey[nh++] = std::make_pair(PrepassMakeKey(pp, 0), d[3]);
			}
			else if (c == none)
				continue;
			c++;
		}
		std::sort(g.hkey, g.hkey + nh,
				[](const std::pair<PrepassKey, u32>& a, const std::pair<PrepassKey, u32>& b) {
					return a.first < b.first || (a.first == b.first && a.second < b.second);
				});
		// where each one's triangles go, and the draws
		u32 at = g.ptris;
		for (u32 k = 0; k < nh; k++)
		{
			const u32 h = g.hkey[k].second;
			const u32 c = g.hcount[h];
			g.hcount[h] = at;
			if (k == 0 || !(g.hkey[k].first == g.hkey[k - 1].first))
			{
				SortTrigDrawParam &r = g.prun[npr++];
				r.ppid = &f.header[h].pp;
				r.first = (g.tris + at) * 3;
				r.count = 0;
			}
			g.prun[npr - 1].count += c * 3;
			at += c;
		}
		for (u32 t = t0; t < t0 + n; t++)
		{
			const u32 *d = &g.tri[t * 4];
			if (d[0] == d[1] || d[1] == d[2] || d[0] == d[2])
				continue;
			u32 &o = g.hcount[d[3]];
			if (o != none)
			{
				g.tpos[t] = o;
				g.porder[o++] = t;
			}
		}
		for (u32 k = 0; k < nt; k++)
			g.hcount[g.htouched[k]] = 0;
		g.ptris = at;
		g.pass_pruns[p] = npr - g.pass_prun[p];
	}
}

/* The sorted passes of the frame the header pass made (before its lists
 * are made into draw groups), with @seg_v each segment's vertices: false
 * if they do not fit (which cannot be) */
static bool ta_gpu_sort_frame(TaWalkFrame &f, const u8 *root)
{
	TaGpuSortState &g = gsort;
	bool sorted[TAG_MAX_PASS];
	u32 p;
	const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

	g.tris = 0;
	g.ptris = 0;
	g.strips = 0;
	g.strips_full = false;
	g.checks = 0;
	memset(g.pass_pruns, 0, sizeof(g.pass_pruns));
	memset(g.pass_first, 0, sizeof(g.pass_first));
	memset(g.pass_count, 0, sizeof(g.pass_count));
	memset(g.pass_buf, 0, sizeof(g.pass_buf));
	bool any = false;
	for (p = 0; p < TAG_MAX_PASS; p++)
	{
		sorted[p] = p < f.passes && f.pass[p].autosort;
		any = any || sorted[p];
	}
	if (!any)
		return true;
	if (!ta_gpu_sort_strips(f, root, sorted))
		return false;

	// each pass: sorted, and its draws made as GenSorted() makes them
	u32 nrun = 0;
	for (p = 0; p < f.passes; p++)
	{
		const u32 n = g.pass_count[p];
		g.pass_run[p] = nrun;
		if (n == 0)
			continue;
		g.pass_buf[p] = (u8)ta_gpu_sort_keys(g.keys, g.pass_first[p], n);
		const u64 *k = g.keys[g.pass_buf[p]] + g.pass_first[p];
		u32 cur = g.tri[(u32)k[0] * 4 + 3];
		u32 start = 0;
		for (u32 j = 1; j <= n; j++)
		{
			u32 h = 0;
			if (j < n)
			{
				h = g.tri[(u32)k[j] * 4 + 3];
				// (another strip of the same polygon, or one drawn the same way: the same draw)
				if (h == cur || SortSameDraw(&f.header[h].pp, &f.header[cur].pp))
					continue;
			}
			SortTrigDrawParam &r = g.run[nrun++];
			r.ppid = &f.header[cur].pp;
			r.first = (g.pass_first[p] + start) * 3;
			r.count = (j - start) * 3;
			cur = h;
			start = j;
		}
		g.pass_runs[p] = nrun - g.pass_run[p];
	}
	ta_gpu_prepass_frame(f);
	ta_gpu_prepass_make_checks(f);
	g.ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	return true;
}

/* TA_GPU_SHADOWS: what each render pass's opaque modifier volumes cover,
 * from the triangles' blocks, as ModVolBounds() has it from the parse's */
static void ta_gpu_shadow_areas(const TaWalkFrame &f, const u8 *root, f32 (*area)[4])
{
	u32 r = 0, prev = 0;
	for (u32 p = 0; p < f.passes; p++)
	{
		ScreenBounds b = { 1e30f, 1e30f, -1e30f, -1e30f };
		for (u32 m = prev; m < f.pass[p].mvo_count; m++)
		{
			const ModifierVolumeParam &mp = f.mvo[0][m];
			for (u32 t = mp.first; t < mp.first + mp.count; t++)
			{
				// (the runs of both lists' volumes' triangles, which are numbered together, in their order)
				while (r < f.runs && (!(f.run[r].header & TA_WALK_MODVOL) || f.run[r].vertex + (f.run[r].blocks + 1) / 2 <= t))
					r++;
				if (r == f.runs)
					break;
				const u32 *w = (const u32 *)(root + (size_t)(f.run[r].block + 2 * (t - f.run[r].vertex)) * 32);
				float c[9];
				memcpy(c, &w[1], sizeof(c));
				b.add(c[0], c[1]);
				b.add(c[3], c[4]);
				b.add(c[6], c[7]);
			}
		}
		prev = f.pass[p].mvo_count;
		area[p][0] = b.min_x;
		area[p][1] = b.min_y;
		area[p][2] = b.max_x;
		area[p][3] = b.max_y;
	}
}

/* Where flat strips are looked for (SSAA), each header's strips: whether
 * all of them are flat, in its pp's single-rate bit, as the full parse
 * finds it from its vertices. From the frame's own blocks, as the sort
 * reads them: of each vertex only its depth word and its end-of-strip
 * bit, the AND and the OR of the depths' bits over a strip
 * (PVR_FLAT_RATE_BIT()). A sprite's bit is set already, and the
 * background's. The draw groups are then given their strips' bits
 * together, as flat_groups() gives the parse's. */
static u64 gflat_ns;	// the last frame's (ta_gpu_flat_ns())

u64 ta_gpu_flat_ns(void)
{
	return gflat_ns;
}

static void ta_gpu_flat_headers(TaWalkFrame &f, const u8 *root)
{
	const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	u32 open_hdr = TA_WALK_NO_GROUP;
	u32 z_and = PVR_FLAT_AND_INIT, z_or = PVR_FLAT_OR_INIT;

	for (u32 i = 0; i < f.headers; i++)
		if (f.header[i].format != TA_WALK_SPRITE)
			f.header[i].pp.pcw.full |= PVR_PCW_SINGLE_RATE;
	for (u32 i = 0; i < f.runs; i++)
	{
		const TaWalkRun &run = f.run[i];
		if (run.header & TA_WALK_MODVOL)
			continue;
		TaWalkHeader &h = f.header[run.header];
		if (h.format == TA_WALK_SPRITE)
			continue;
		// (its vertices: up to the next run of vertices, or the frame's last)
		u32 j = i + 1, end = f.verts;
		while (j < f.runs && (f.run[j].header & TA_WALK_MODVOL))
			j++;
		if (j < f.runs)
			end = f.run[j].vertex;
		const u32 n = end - run.vertex;
		if (open_hdr != run.header)
		{
			// (a strip does not go on from one header to the next: the header pass leaves such a frame to the parse)
			open_hdr = run.header;
			z_and = PVR_FLAT_AND_INIT;
			z_or = PVR_FLAT_OR_INIT;
		}
		const u32 stride = h.vblocks * 8;
		const u32 *w = (const u32 *)(root + (size_t)run.block * 32);
		for (u32 k = 0; k < n; k++, w += stride)
		{
			z_and &= w[3];
			z_or |= w[3];
			if (((const PCW *)w)->EndOfStrip)
			{
				h.pp.pcw.full &= ~PVR_PCW_SINGLE_RATE | PVR_FLAT_RATE_BIT(z_and, z_or);
				z_and = PVR_FLAT_AND_INIT;
				z_or = PVR_FLAT_OR_INIT;
			}
		}
	}
	gflat_ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

bool ta_gpu_prepare(TA_context *ctx, const TaGpuLayout *layout, u32 *table, u32 table_room, TaGpuFrame *out, u32 flags)
{
	TaWalkFrame &f = walk_frame[0];
	u32 count, i, l;
	const bool draw = (flags & TA_GPU_DRAW) != 0;

	out->why = TA_GPU_WHY_TABLE;
	out->shadows = 0;
	// (nothing sorted, until this frame is)
	gsort.tris = 0;
	gsort.ptris = 0;
	gsort.ns = 0;
	memset(gsort.pass_count, 0, sizeof(gsort.pass_count));
	memset(gsort.pass_runs, 0, sizeof(gsort.pass_runs));
	memset(gsort.pass_pruns, 0, sizeof(gsort.pass_pruns));
	if (!ta_gpu_on || !ta_walk_table(ctx, &count))
		return false;
	if (draw)
	{
		/* What only the full parse makes: a frame it passes over (frame
		 * skipping), whose bookkeeping it keeps; the flat strips' and the
		 * region array's table, which are made from the vertices */
		out->why = TA_GPU_WHY_PARSE;
		if (!(ctx->rend.isRTT || 0 == ((ta_parse_cnt + 1) % (settings.pvr.ta_skip + 1)))
				|| region_table_wanted)
			return false;
		/* A renderer that sorts translucent polygons on the CPU sorts them
		 * from the CPU's vertices: it has the full parse for a frame that
		 * may have a sorted pass */
		out->why = TA_GPU_WHY_SORT;
		if (((flags & TA_GPU_NO_SORT) || ((flags & TA_GPU_SORT) && gsort.room == 0)) && ta_gpu_any_autosort(ctx))
			return false;
	}
	out->why = TA_GPU_WHY_WALK;
	f.bg_in = *ctx->rend.global_param_op.head();
	f.x_clip_in = ctx->rend.fb_X_CLIP;
	f.y_clip_in = ctx->rend.fb_Y_CLIP;
	f.gpu = true;
	const bool ok = ta_walk_run<false>(ctx, f, count);
	f.gpu = false;
	if (!ok || f.gpu_bad || f.passes > TAG_MAX_PASS)
		return false;

	TaGpuSizes sz;
	ta_gpu_sizes(&sz);
	/* What the full parse overruns with is left to it: it drops the frame
	 * then, as it always has */
	if (f.verts > (u32)ctx->rend.verts.size || f.verts > sz.verts
			|| f.modtrig > (u32)ctx->rend.modtrig.size || f.modtrig > sz.modtrig)
		return false;

	// the headers with vertices, by segment, and the vertices of each
	const u32 nseg = f.passes * 3;
	u32 seg_hdr[TAG_MAX_SEG], seg_v[TAG_MAX_SEG], seg_last[TAG_MAX_SEG], seg_slot[TAG_MAX_SEG];
	memset(seg_hdr, 0, sizeof(seg_hdr));
	memset(seg_v, 0, sizeof(seg_v));
	seg_hdr[0] = 1;
	seg_v[0] = 4;
	u32 nhdr = 1, nruns = 0, nmvruns = 0;
	for (i = 0; i < f.headers; i++)
	{
		const TaWalkHeader &h = f.header[i];
		if (h.pp.count == 0)
			continue;
		const u32 seg = h.pass * 3 + (u32)TaWalker<false>::pp_list(h.list);
		seg_hdr[seg]++;
		seg_v[seg] += h.pp.count;
		nhdr++;
	}
	for (i = 0; i < f.runs; i++)
		if (f.run[i].header & TA_WALK_MODVOL)
			nmvruns++;
		else
			nruns++;

	const u32 off_run = TAG_T_WORDS;
	const u32 off_hdr = off_run + nruns * TAG_R_WORDS;
	const u32 off_mvrun = off_hdr + nhdr * TAG_H_WORDS;
	const u32 off_seg = off_mvrun + nmvruns * TAG_M_WORDS;
	const u32 off_bg = off_seg + nseg * TAG_G_WORDS;
	const u32 off_shd = off_bg + 4 * TAG_V_WORDS;
	const bool shadows = draw && (flags & TA_GPU_SHADOWS) != 0;
	// (an entry for each draw group at most, which is a header at most)
	const u32 words = off_shd + (shadows ? TAG_SH_ENTRY + nhdr : 0);
	if (words > table_room || nhdr > sz.headers)
		return false;

	rend_context &rc = ctx->rend;
	if (draw)
	{
		/* The full parse drops a frame that runs out of room for its
		 * indices or polygon parameters, and the screen keeps the last
		 * picture; the decode knows that only on the GPU, which can then
		 * leave out the draws but not keep the picture. So the frames that
		 * might run out are left to the full parse, by what can be known
		 * here: a list has one more polygon parameter than strips while it
		 * is being filled (and the opaque list the background's), and a
		 * strip has a vertex at least - or, where that is too many, as many
		 * strips as the vertices' end-of-strip bits say, which is one word
		 * read a vertex, and every fourth corner of a sprite; make_index()
		 * puts at most four indices a vertex - one for the vertex, one for
		 * the parity, and the two that link a strip, or a run of infinite
		 * vertices, to what came before. */
		out->why = TA_GPU_WHY_ROOM;
		u32 lv[3] = { 0, 0, 0 };
		for (i = 0; i < f.headers; i++)
			if (f.header[i].pp.count != 0)
				lv[TaWalker<false>::pp_list(f.header[i].list)] += f.header[i].pp.count;
		lv[0]++;	// (the background)
		const u32 lroom[3] = { (u32)rc.global_param_op.size, (u32)rc.global_param_pt.size, (u32)rc.global_param_tr.size };
		if ((u64)f.verts * 4 > (u64)rc.idx.size
				|| f.mvos[0] > (u32)rc.global_param_mvo.size || f.mvos[1] > (u32)rc.global_param_mvo_tr.size)
			return false;
		if ((u64)lv[0] + 1 > lroom[0] || (u64)lv[1] + 1 > lroom[1] || (u64)lv[2] + 1 > lroom[2])
		{
			u32 ls[3] = { 1, 0, 0 };
			const u8 *root = ctx->tad.thd_root;
			for (i = 0; i < f.runs; i++)
			{
				const TaWalkRun &run = f.run[i];
				if (run.header & TA_WALK_MODVOL)
					continue;
				u32 j = i + 1, end = f.verts;
				while (j < f.runs && (f.run[j].header & TA_WALK_MODVOL))
					j++;
				if (j < f.runs)
					end = f.run[j].vertex;
				const TaWalkHeader &h = f.header[run.header];
				const u32 n = end - run.vertex;
				l = (u32)TaWalker<false>::pp_list(h.list);
				if (h.format == TA_WALK_SPRITE)
					ls[l] += n / 4;
				else
					for (u32 k = 0; k < n; k++)
						ls[l] += ((const PCW *)(root + (size_t)(run.block + k * h.vblocks) * 32))->EndOfStrip;
			}
			if ((u64)ls[0] + 1 > lroom[0] || (u64)ls[1] + 1 > lroom[1] || (u64)ls[2] + 1 > lroom[2])
				return false;
		}
		/* The sorted passes' translucent triangles, in the order the
		 * per-triangle renderer draws them */
		out->why = TA_GPU_WHY_SORT;
		gsort.first_provoking = (flags & TA_GPU_PROVOKE) != 0;
		if ((flags & TA_GPU_SORT) && !ta_gpu_sort_frame(f, ctx->tad.thd_root))
			return false;
		if (flat_strips_wanted)
			ta_gpu_flat_headers(f, ctx->tad.thd_root);
		/* Drawn from here on: the lists hold a polygon parameter for each
		 * draw group (see TA_GPU_DRAW) */
		rc.global_param_op.Clear();
		rc.global_param_pt.Clear();
		rc.global_param_tr.Clear();
	}
	out->why = TA_GPU_WHY_NONE;

	// where each segment's headers and vertices start, in index order
	u32 seg_next_h[TAG_MAX_SEG], seg_next_v[TAG_MAX_SEG];
	{
		u32 h = 0, v = 0;
		for (i = 0; i < nseg; i++)
		{
			u32 *g = &table[off_seg + i * TAG_G_WORDS];
			seg_next_h[i] = h;
			seg_next_v[i] = v;
			g[TAG_G_VSTART] = v;
			g[TAG_G_VEND] = v + seg_v[i];
			g[2] = g[3] = 0;
			h += seg_hdr[i];
			v += seg_v[i];
			seg_last[i] = TAG_NONE;
			seg_slot[i] = TAG_NONE;
		}
	}

	/* A header's record. Its slot: a command for each draw group of the
	 * opaque and punch-through lists. The translucent strips are each their
	 * own polygon parameter, with a record of their own; for drawing, the
	 * translucent headers one after another in a segment that are drawn
	 * the same way (poly_same_state()) are a draw group too, whose records
	 * the GPU copies to a place of the group's in the translucent draw
	 * records: a word for how many there are, then room for one a vertex,
	 * which is the most strips the group can have (TAG_H_TRB, TAG_H_TRV).
	 * With TA_GPU_DRAW each draw group is a polygon parameter of the lists
	 * here, in the lists' order, which is the parse's. */
	u32 nslots = 0, ntrd = 0;
	const PolyParam *seg_tr[TAG_MAX_SEG];
	u32 seg_trb[TAG_MAX_SEG], seg_trv[TAG_MAX_SEG], seg_groups[TAG_MAX_SEG];
	for (i = 0; i < nseg; i++)
	{
		seg_tr[i] = NULL;
		seg_trb[i] = seg_trv[i] = TAG_NONE;
		seg_groups[i] = 0;
	}
	List<PolyParam> *lists[3] = { &rc.global_param_op, &rc.global_param_pt, &rc.global_param_tr };
	auto put_header = [&](u32 id, const PolyParam &pp, u32 first, u32 cnt, u32 vstart, u32 fmt, u32 vb,
			u32 list, u32 seg, u32 group, const TaWalkFace *face) {
		u32 *w = &table[off_hdr + id * TAG_H_WORDS];
		u32 slot = TAG_NONE;
		bool fresh;
		if (list != 2)
		{
			fresh = seg_last[seg] != group;
			if (fresh)
			{
				seg_last[seg] = group;
				seg_slot[seg] = nslots++;
			}
			slot = seg_slot[seg];
		}
		else
		{
			fresh = seg_tr[seg] == NULL || !poly_same_state(seg_tr[seg], &pp);
			if (fresh)
			{
				seg_tr[seg] = &pp;
				seg_trb[seg] = ntrd++;
				seg_trv[seg] = vstart;
			}
			ntrd += cnt;
		}
		if (draw)
		{
			// first: the command's slot, or the group's place in the translucent draw records; count: its vertices
			if (fresh)
			{
				PolyParam *d = lists[list]->Append();
				*d = pp;
				d->first = list != 2 ? slot : seg_trb[seg];
				d->count = cnt;
				seg_groups[seg]++;
			}
			else
				lists[list]->LastPtr()->count += cnt;
			// (a group is drawn at a single rate where all of its strips are flat: see ta_gpu_flat_headers())
			if (!fresh && flat_strips_wanted)
				lists[list]->LastPtr()->pcw.full &= pp.pcw.full | ~PVR_PCW_SINGLE_RATE;
		}
		w[TAG_H_VFIRST] = first;
		w[TAG_H_VCOUNT] = cnt;
		w[TAG_H_VSTART] = vstart;
		w[TAG_H_INFO] = TAG_INFO(fmt, vb, list, seg, pp.pcw.Texture);
		w[TAG_H_SLOT] = slot;
		w[TAG_H_STATE + TAG_S_TSP] = pp.tsp.full;
		w[TAG_H_STATE + TAG_S_TCW] = pp.tcw.full;
		w[TAG_H_STATE + TAG_S_PCW] = pp.pcw.full & ~PVR_PCW_SINGLE_RATE;
		w[TAG_H_STATE + TAG_S_ISP] = pp.isp.full;
		w[TAG_H_STATE + TAG_S_TSP1] = pp.tsp1.full;
		w[TAG_H_STATE + TAG_S_TCW1] = pp.tcw1.full;
		w[TAG_H_STATE + TAG_S_CLIP] = pp.tileclip;
		if (face != NULL)
		{
			memcpy(&w[TAG_H_FCOL], face->col, sizeof(face->col));
			memcpy(&w[TAG_H_FF], face->f, sizeof(face->f));
			memcpy(&w[TAG_H_SCOL], face->sprite, sizeof(face->sprite));
		}
		else
			memset(&w[TAG_H_FCOL], 0, (TAG_H_TRB - TAG_H_FCOL) * sizeof(u32));
		w[TAG_H_TRB] = list == 2 ? seg_trb[seg] : TAG_NONE;
		w[TAG_H_TRV] = list == 2 ? seg_trv[seg] : TAG_NONE;
		return id;
	};

	// the background, first of the first pass's opaque list (its group is 0)
	put_header(seg_next_h[0]++, f.bg, 0, 4, 0, TAG_FMT_BG, 1, 0, 0, 0, NULL);
	seg_next_v[0] += 4;
	/* the others, as they came (which is their order within a segment);
	 * the runs are given the headers' new numbers through the old ones'
	 * group words, which are not needed after this */
	for (i = 0; i < f.headers; i++)
	{
		TaWalkHeader &h = f.header[i];
		if (h.pp.count == 0)
			continue;
		l = (u32)TaWalker<false>::pp_list(h.list);
		const u32 seg = h.pass * 3 + l;
		const u32 id = put_header(seg_next_h[seg]++, h.pp, h.pp.first, h.pp.count, seg_next_v[seg],
				h.format, h.vblocks, l, seg, h.group, &f.face[i]);
		seg_next_v[seg] += h.pp.count;
		h.group = id;
	}
	{
		u32 r = 0, m = 0;
		for (i = 0; i < f.runs; i++)
		{
			const TaWalkRun &run = f.run[i];
			if (run.header & TA_WALK_MODVOL)
			{
				u32 *w = &table[off_mvrun + m++ * TAG_M_WORDS];
				w[TAG_M_BLOCK] = run.block;
				w[TAG_M_FIRST] = run.vertex;
				w[TAG_M_COUNT] = (run.blocks + 1) / 2;
				w[3] = 0;
			}
			else
			{
				u32 *w = &table[off_run + r++ * TAG_R_WORDS];
				w[TAG_R_BLOCK] = run.block;
				w[TAG_R_VERTEX] = run.vertex;
				w[TAG_R_COUNT] = 0;
				w[TAG_R_HDR] = f.header[run.header].group;
			}
		}
	}
	memcpy(&table[off_bg], ctx->rend.verts.head(), 4 * sizeof(Vertex));

	u32 *t = table;
	t[TAG_T_NVERTS] = f.verts;
	t[TAG_T_NRUNS] = nruns;
	t[TAG_T_NHDR] = nhdr;
	t[TAG_T_NMVRUN] = nmvruns;
	t[TAG_T_NMOD] = f.modtrig;
	t[TAG_T_NSEG] = nseg;
	t[TAG_T_NSLOT] = nslots;
	t[TAG_T_FLAGS] = (screen_height > 480 ? TAG_FLAG_BLEED : 0) | ((flags & TA_GPU_PROVOKE) ? TAG_FLAG_PROVOKE : 0)
			| ((flags & TA_GPU_INSTANCE) ? TAG_FLAG_INSTANCE : 0);
	t[TAG_T_ROOM_IDX] = (u32)ctx->rend.idx.size;
	t[TAG_T_ROOM_OP] = (u32)ctx->rend.global_param_op.size;
	t[TAG_T_ROOM_PT] = (u32)ctx->rend.global_param_pt.size;
	t[TAG_T_ROOM_TR] = (u32)ctx->rend.global_param_tr.size;
	t[TAG_T_OFF_RUN] = off_run;
	t[TAG_T_OFF_HDR] = off_hdr;
	t[TAG_T_OFF_MVRUN] = off_mvrun;
	t[TAG_T_OFF_SEG] = off_seg;
	t[TAG_T_OFF_BG] = off_bg;
	memcpy(&t[TAG_T_TA_BASE], layout->words, sizeof(layout->words));
	t[TAG_T_NPASS] = f.passes;
	t[TAG_T_ROOM_VTX] = sz.verts;
	t[TAG_T_OFF_SHD] = off_shd;
	t[TAG_T_NSHD] = 0;
	t[TAG_T_SHD_CMD] = sz.headers;
	t[TAG_T_PREP] = (flags & TA_GPU_SORT) && gsort.checks != 0 ? ta_gpu_sorted_indices() : TAG_NONE;

	out->table_words = words;
	out->verts = f.verts;
	out->modtrig = f.modtrig;
	out->headers = nhdr;
	out->slots = nslots;
	out->segments = nseg;
	out->passes = f.passes;
	memcpy(out->pass, f.pass, sizeof(out->pass));
	out->x_clip = f.x_clip;
	out->y_clip = f.y_clip;
	out->rv = f.rv;
	out->trd = ntrd;

	if (draw)
	{
		/* The rest of what the full parse leaves for the renderer: the
		 * render passes, with each list's draw groups as its count, the
		 * modifier volumes, the clipping rectangle. The vertices, indices
		 * and modifier volume triangles are the GPU's; fZ_max too, which
		 * the Vulkan renderers do not use. */
		rc.render_passes.Clear();
		u32 groups[3] = { 0, 0, 0 };
		for (u32 p = 0; p < f.passes; p++)
		{
			RenderPass *rp = rc.render_passes.Append();
			*rp = f.pass[p];
			for (l = 0; l < 3; l++)
				groups[l] += seg_groups[p * 3 + l];
			rp->op_count = groups[0];
			rp->pt_count = groups[1];
			rp->tr_count = groups[2];
		}
		rc.global_param_mvo.Clear();
		rc.global_param_mvo_tr.Clear();
		if (f.mvos[0] != 0)
			memcpy(rc.global_param_mvo.Append(f.mvos[0]), f.mvo[0], f.mvos[0] * sizeof(ModifierVolumeParam));
		if (f.mvos[1] != 0)
			memcpy(rc.global_param_mvo_tr.Append(f.mvos[1]), f.mvo[1], f.mvos[1] * sizeof(ModifierVolumeParam));
		rc.fb_X_CLIP = f.x_clip;
		rc.fb_Y_CLIP = f.y_clip;
		rc.gpu = true;
		rc.gpu_modtrig = f.modtrig;
		ta_parse_cnt++;
		/* The draw groups the per-triangle renderer draws again for
		 * shadows (Drawer::DrawShadowed()), each with its render pass's
		 * volumes' bounds, for the GPU to say which reach into them */
		if (shadows)
		{
			u32 *sh = &table[off_shd];
			u32 nsh = 0, prev[2] = { 0, 0 };
			ta_gpu_shadow_areas(f, ctx->tad.thd_root, rc.gpu_shadow_area);
			for (u32 p = 0; p < f.passes; p++)
			{
				memcpy(&sh[TAG_SH_AREA + p * 4], rc.gpu_shadow_area[p], 4 * sizeof(u32));
				const ScreenBounds area = { rc.gpu_shadow_area[p][0], rc.gpu_shadow_area[p][1], rc.gpu_shadow_area[p][2], rc.gpu_shadow_area[p][3] };
				const RenderPass &rp = rc.render_passes.head()[p];
				const u32 end[2] = { rp.op_count, rp.pt_count };
				for (l = 0; l < 2; l++)
				{
					for (i = prev[l]; i < end[l] && !area.empty(); i++)
					{
						const PolyParam &pp = lists[l]->head()[i];
						if (CanDrawShadowed(l == 0 ? ListType_Opaque : ListType_Punch_Through, &pp))
							sh[TAG_SH_ENTRY + nsh++] = TAG_SH(pp.first, p);
					}
					prev[l] = end[l];
				}
			}
			t[TAG_T_NSHD] = nsh;
			out->shadows = nsh;
		}
	}
	return true;
}

void ta_set_gpu_processing(bool on)
{
	if (on == ta_gpu_on && (!on || walk_data_size >= ta_data_size()))
		return;
	if (on && !ta_walk_init())
		on = false;
	ta_gpu_on = on;
	if (!on)
		ta_gpu_sort_reserve(false);
	if (!on && !ta_walk_checking)
		ta_walk_term();
	ta_set_walk(on || ta_walk_checking);
}

/* ---- The check: the header pass beside the full parse, every frame ---- */

typedef std::chrono::steady_clock walk_clock;

static u64 walk_ns(walk_clock::time_point a, walk_clock::time_point b)
{
	return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

// Times, with a histogram of 256 ns steps (up to 16.7 ms) for the 99th percentile
#define WALK_HIST 65536
struct TaWalkTimes
{
	u64 sum;
	u64 max;
	u32 n;
	u32 hist[WALK_HIST];

	void add(u64 ns)
	{
		u64 b = ns / 256;
		sum += ns;
		if (ns > max)
			max = ns;
		hist[b < WALK_HIST ? b : WALK_HIST - 1]++;
		n++;
	}
	double p99_us() const
	{
		u32 over = 0, b = WALK_HIST;
		while (b > 0 && over * 100 < n)
			over += hist[--b];
		return b * 0.256;
	}
};

static struct TaWalkStats
{
	u32 checked, same, differ, no_table, full_overrun, no_room, skipped;
	u64 headers, entries, verts;
	TaWalkTimes full, pass;
} *walk_stats;

// (ta_walk_full_strips(): where make_index() of a render pass would be)
static void ta_walk_snap_full(u32 op_from, u32 pt_from, u32 tr_from)
{
	const List<PolyParam> *lists[3] = { &vd_rc.global_param_op, &vd_rc.global_param_pt, &vd_rc.global_param_tr };
	const u32 from[3] = { op_from, pt_from, tr_from };

	for (int l = 0; l < 3; l++)
	{
		const u32 used = (u32)lists[l]->used();
		for (u32 i = from[l]; i < used && walk_full.count[l] < walk_full.room[l]; i++)
			walk_full.pp[l][walk_full.count[l]++] = lists[l]->head()[i];
	}
}

/* Whether two polygon parameters are the same, but for the depth the
 * renderer works out (zvZ), and the single-rate bit of strips where flat
 * strips are looked for (the full parse sets it from the vertices) */
static bool walk_pp_same(const PolyParam &a, const PolyParam &b)
{
	const u32 pcw_mask = flat_strips_wanted ? ~PVR_PCW_SINGLE_RATE : ~0u;
	return a.first == b.first && a.count == b.count && a.texid == b.texid
			&& a.tsp.full == b.tsp.full && a.tcw.full == b.tcw.full
			&& ((a.pcw.full ^ b.pcw.full) & pcw_mask) == 0 && a.isp.full == b.isp.full
			&& a.tileclip == b.tileclip && a.tsp1.full == b.tsp1.full
			&& a.tcw1.full == b.tcw1.full && a.texid1 == b.texid1;
}

/* What differs between the full parse (vd_rc as it left it, walk_full)
 * and the check's frame @s and the header pass's @h: NULL for nothing */
static const char *ta_walk_compare(const TaWalkFrame &s, const TaWalkFrame &h, bool rv, char *why, size_t room)
{
	static const char *const names[3] = { "opaque", "punch-through", "translucent" };
	u32 l, i;

	if (rv != s.rv)
		return "whether there is anything to draw";
	if ((u32)vd_rc.verts.used() != s.verts)
	{
		snprintf(why, room, "vertices: %d, %u", vd_rc.verts.used(), s.verts);
		return why;
	}
	if ((u32)vd_rc.modtrig.used() != s.modtrig)
	{
		snprintf(why, room, "modifier volume triangles: %d, %u", vd_rc.modtrig.used(), s.modtrig);
		return why;
	}
	if ((u32)vd_rc.render_passes.used() != s.passes)
	{
		snprintf(why, room, "render passes: %d, %u", vd_rc.render_passes.used(), s.passes);
		return why;
	}
	for (i = 0; i < s.passes; i++)
	{
		const RenderPass &a = vd_rc.render_passes.head()[i];
		const RenderPass &b = s.pass[i];
		if (a.autosort != b.autosort || a.z_clear != b.z_clear || a.mv_op_tr_shared != b.mv_op_tr_shared
				|| a.op_count != b.op_count || a.mvo_count != b.mvo_count || a.pt_count != b.pt_count
				|| a.tr_count != b.tr_count || a.mvo_tr_count != b.mvo_tr_count)
		{
			snprintf(why, room, "render pass %u: op %u/%u pt %u/%u tr %u/%u mvo %u/%u mvo_tr %u/%u",
					i, a.op_count, b.op_count, a.pt_count, b.pt_count, a.tr_count, b.tr_count,
					a.mvo_count, b.mvo_count, a.mvo_tr_count, b.mvo_tr_count);
			return why;
		}
	}
	for (l = 0; l < 3; l++)
	{
		if (walk_full.count[l] != s.snap.count[l])
		{
			snprintf(why, room, "%s polygons: %u, %u", names[l], walk_full.count[l], s.snap.count[l]);
			return why;
		}
		for (i = 0; i < s.snap.count[l]; i++)
			if (!walk_pp_same(walk_full.pp[l][i], s.snap.pp[l][i]))
			{
				const PolyParam &a = walk_full.pp[l][i], &b = s.snap.pp[l][i];
				snprintf(why, room, "%s polygon %u (header %d): first %u/%u count %u/%u pcw %08x/%08x isp %08x/%08x tsp %08x/%08x tcw %08x/%08x clip %08x/%08x",
						names[l], i, (int)s.snap.header[l][i], a.first, b.first, a.count, b.count,
						a.pcw.full, b.pcw.full, a.isp.full, b.isp.full, a.tsp.full, b.tsp.full,
						a.tcw.full, b.tcw.full, a.tileclip, b.tileclip);
				return why;
			}
	}
	/* Each strip is drawn as its header says (the header pass's own
	 * polygon parameters, which have no strips) */
	for (l = 0; l < 3; l++)
		for (i = 0; i < s.snap.count[l]; i++)
		{
			const u32 hi = s.snap.header[l][i];
			PolyParam a = s.snap.pp[l][i];
			PolyParam b = hi == TA_WALK_BG ? s.bg : s.header[hi].pp;
			a.first = b.first = 0;
			a.count = b.count = 0;
			if (!walk_pp_same(a, b))
			{
				snprintf(why, room, "%s polygon %u and its header %d", names[l], i, (int)hi);
				return why;
			}
		}
	const List<ModifierVolumeParam> *mv[2] = { &vd_rc.global_param_mvo, &vd_rc.global_param_mvo_tr };
	for (l = 0; l < 2; l++)
	{
		if ((u32)mv[l]->used() != s.mvos[l])
		{
			snprintf(why, room, "modifier volumes (%u): %d, %u", l, mv[l]->used(), s.mvos[l]);
			return why;
		}
		for (i = 0; i < s.mvos[l]; i++)
		{
			const ModifierVolumeParam &a = mv[l]->head()[i], &b = s.mvo[l][i];
			if (a.first != b.first || (b.count != TA_WALK_UNENDED && a.count != b.count)
					|| a.isp.full != b.isp.full || a.tileclip != b.tileclip)
			{
				snprintf(why, room, "modifier volume %u (%u)", i, l);
				return why;
			}
		}
	}
	if (rv && (vd_rc.fb_X_CLIP.full != s.x_clip.full || vd_rc.fb_Y_CLIP.full != s.y_clip.full))
		return "clipping rectangle";

	/* The draw groups: each strip with the one before in the same pass
	 * and list is joined by make_index()'s rule exactly when their headers
	 * are of a group (the translucent ones are never joined) */
	for (l = 0; l < 2; l++)
	{
		u32 at = 0;
		for (u32 p = 0; p < s.passes; p++)
		{
			const int to = (int)(l == 0 ? s.pass[p].op_count : s.pass[p].pt_count);
			const int from = p == 0 ? 0 : (int)(l == 0 ? s.pass[p - 1].op_count : s.pass[p - 1].pt_count);
			const u32 n = to > from ? (u32)(to - from) : 0;
			for (i = 1; i < n && at + i < s.snap.count[l]; i++)
			{
				const u32 ha = s.snap.header[l][at + i - 1], hb = s.snap.header[l][at + i];
				const u32 ga = ha == TA_WALK_BG ? 0 : s.header[ha].group;
				const u32 gb = hb == TA_WALK_BG ? 0 : s.header[hb].group;
				const bool joined = poly_same_state(&s.snap.pp[l][at + i - 1], &s.snap.pp[l][at + i]);
				if (s.snap.pp[l][at + i - 1].count == 0 || s.snap.pp[l][at + i].count == 0)
					continue;	// (no vertices: in no group)
				if (joined != (ga == gb))
				{
					snprintf(why, room, "draw groups, %s polygon %u", names[l], at + i);
					return why;
				}
			}
			at += n;
		}
	}

	/* And the header pass by itself, which never saw a vertex, made the
	 * same headers, runs, volumes and passes as the check's */
	if (h.headers != s.headers || memcmp(h.header, s.header, h.headers * sizeof(TaWalkHeader)) != 0)
		return "the header pass's headers";
	if (h.runs != s.runs || memcmp(h.run, s.run, h.runs * sizeof(TaWalkRun)) != 0)
		return "the header pass's vertex runs";
	if (h.passes != s.passes || h.verts != s.verts || h.modtrig != s.modtrig || h.rv != s.rv
			|| h.mvos[0] != s.mvos[0] || h.mvos[1] != s.mvos[1])
		return "the header pass's totals";
	for (i = 0; i < h.passes; i++)
		if (h.pass[i].autosort != s.pass[i].autosort || h.pass[i].z_clear != s.pass[i].z_clear
				|| h.pass[i].mv_op_tr_shared != s.pass[i].mv_op_tr_shared
				|| h.pass[i].mvo_count != s.pass[i].mvo_count || h.pass[i].mvo_tr_count != s.pass[i].mvo_tr_count)
			return "the header pass's render passes";
	if (h.rv && (h.x_clip.full != s.x_clip.full || h.y_clip.full != s.y_clip.full))
		return "the header pass's clipping rectangle";
	return NULL;
}

static void ta_walk_log(void)
{
	const TaWalkStats &st = *walk_stats;
	const double nf = st.full.n ? (double)st.full.n : 1.0;
	const double np = st.pass.n ? (double)st.pass.n : 1.0;
	NOTICE_LOG(PVR, "TA header pass check: %u frames compared, %u the same, %u different; not compared: %u without a table, %u overrun, %u out of room, %u skipped",
			st.checked, st.same, st.differ, st.no_table, st.full_overrun, st.no_room, st.skipped);
	NOTICE_LOG(PVR, "TA header pass check: full parse mean %.1f us, p99 %.1f us, max %.1f us; header pass mean %.1f us, p99 %.1f us, max %.1f us; per frame %.0f headers, %.0f entries, %.0f vertices",
			st.full.sum / nf / 1000.0, st.full.p99_us(), st.full.max / 1000.0,
			st.pass.sum / np / 1000.0, st.pass.p99_us(), st.pass.max / 1000.0,
			st.headers / np, st.entries / np, st.verts / np);
}

// (a frame has been through the check, compared or not)
static void ta_walk_tick(void)
{
	const TaWalkStats &st = *walk_stats;
	const u32 frames = st.checked + st.no_table + st.full_overrun + st.no_room + st.skipped;
	if (frames == 30 || frames % 300 == 0)
		ta_walk_log();
}

static bool ta_walk_check_frame(TA_context *ctx)
{
	TaWalkStats &st = *walk_stats;
	TaWalkFrame &h = walk_frame[0], &s = walk_frame[1];
	u32 count;

	/* Frames the full parse passes over (frame skipping) and frames with no
	 * table to trust are parsed as ever */
	if (!(ctx->rend.isRTT || 0 == ((ta_parse_cnt + 1) % (settings.pvr.ta_skip + 1))))
	{
		st.skipped++;
		ta_walk_tick();
		return ta_parse_vdrc_full(ctx);
	}
	if (!ta_walk_table(ctx, &count))
	{
		st.no_table++;
		ta_walk_tick();
		return ta_parse_vdrc_full(ctx);
	}
	h.bg_in = s.bg_in = *ctx->rend.global_param_op.head();
	h.x_clip_in = s.x_clip_in = ctx->rend.fb_X_CLIP;
	h.y_clip_in = s.y_clip_in = ctx->rend.fb_Y_CLIP;

	/* The lists strip by strip first: what textures there are to convert
	 * are then converted, and neither of the two timed finds any */
	walk_full.count[0] = walk_full.count[1] = walk_full.count[2] = 0;
	ta_walk_full_strips(ctx);

	walk_clock::time_point t0 = walk_clock::now();
	const bool hp_ok = ta_walk_run<false>(ctx, h, count);
	walk_clock::time_point t1 = walk_clock::now();
	const bool rv = ta_parse_vdrc_full(ctx);
	walk_clock::time_point t2 = walk_clock::now();

	if (ctx->rend.Overrun)
	{
		st.full_overrun++;
		ta_walk_tick();
		return rv;
	}
	if (!hp_ok || !ta_walk_run<true>(ctx, s, count))
	{
		st.no_room++;
		ta_walk_tick();
		return rv;
	}
	st.full.add(walk_ns(t1, t2));
	st.pass.add(walk_ns(t0, t1));
	st.headers += h.headers;
	st.entries += count;
	st.verts += h.verts;
	st.checked++;

	char why[256];
	const char *differ = ta_walk_compare(s, h, rv, why, sizeof(why));
	if (differ == NULL)
		st.same++;
	else if (st.differ++ < 20)
		WARN_LOG(PVR, "TA header pass check: frame %u differs from the full parse: %s", st.checked, differ);
	ta_walk_tick();
	return rv;
}

void ta_set_walk_check(bool on)
{
	if (on == ta_walk_checking && (!on || walk_data_size >= ta_data_size()))
		return;
	if (on)
	{
		if (!ta_walk_init())
			return;
		if (walk_stats == NULL)
			walk_stats = (TaWalkStats *)calloc(1, sizeof(TaWalkStats));
		if (walk_stats == NULL)
			return;
	}
	else
	{
		if (walk_stats != NULL && walk_stats->checked != 0)
			ta_walk_log();
		// (the GPU decode's header pass works in the same memory)
		if (!ta_gpu_on)
			ta_walk_term();
		free(walk_stats);
		walk_stats = NULL;
	}
	ta_walk_checking = on;
	ta_set_walk(on || ta_gpu_on);
}
