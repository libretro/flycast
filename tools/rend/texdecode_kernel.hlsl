// The kernel of rend/texdecode.h for Direct3D, as the Direct3D renderers
// will have it: checked to compile by texdecode_test.sh
#define TEXD_SHADER_HLSL
ByteAddressBuffer ring : register(t0);
RWTexture2D<uint> dst : register(u0);
cbuffer Level : register(b0) { uint lv_base, lv_data, lv_pal, lv_wh, lv_sr, lv_mode; };
#define TEXD_WORD(i) ring.Load((lv_base + (i)) * 4)
#include "texdecode.h"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
   if (id.x >= (lv_wh & 0xFFFFu) || id.y >= (lv_wh >> 16))
      return;
   dst[id.xy] = texd_texel(lv_data, lv_pal, lv_sr, lv_mode, id.x, id.y);
}
