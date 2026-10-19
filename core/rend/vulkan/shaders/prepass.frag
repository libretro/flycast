// The per-triangle renderer's depth prepass of a sorted translucent list
// (rend/sorter.h; shaders.cpp): the polygon's alpha as the colour pass
// has it, and nothing drawn where it is below the least that hides (the
// depth is the position's: position.glsl). Made with pp_Texture, pp_Gouraud and
// pp_SampleRate defined, after pso_key.h: tools/rend/kernels_spv.sh.

layout (constant_id = PSO_SC_SSAA_SAMPLES) const int SSAA_SAMPLES = 1;
#define PI 3.1415926

#if pp_SampleRate == 1
#define AT_SAMPLE sample
#else
#define AT_SAMPLE
#endif
#if pp_SampleRate == 2
#define AT_CENTRE(v) (gl_SampleMaskIn[0] == ((1 << SSAA_SAMPLES) - 1) ? interpolateAtOffset(v, vec2(0.0)) : interpolateAtCentroid(v))
#else
#define AT_CENTRE(v) (v)
#endif
#if pp_SampleRate == 2 && pp_Texture == 1
// (a texture's derivatives at the pixels' centres: tri.frag)
vec4 uvGrad;
#define TEX(t, coords, g) textureGrad(t, coords, (g).xy, (g).zw)
#else
#define TEX(t, coords, g) texture(t, coords)
#endif
#if pp_Gouraud == 1
#define AT_CENTRE_SMOOTH(v) AT_CENTRE(v)
#else
#define AT_CENTRE_SMOOTH(v) (v)
#endif
#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth AT_SAMPLE
#endif

layout (std140, set = 0, binding = 1) uniform FragmentShaderUniforms
{
	vec4 colorClampMin;
	vec4 colorClampMax;
	vec4 sp_FOG_COL_RAM;
	vec4 sp_FOG_COL_VERT;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
} uniformBuffer;

// (the flags where the colour pass has the trilinear alpha: PREPASS_* in pso_key.h)
layout (push_constant) uniform pushBlock
{
	vec4 clipTest;
	float flags;
	float palette_index;
} pushConstants;

#if pp_Texture == 1
layout (set = 1, binding = 0) uniform sampler2D tex;
layout (set = 0, binding = 3) uniform sampler2D palette;
#endif

layout (location = 0) INTERPOLATION in lowp vec4 in_vtx_base;
layout (location = 1) INTERPOLATION in lowp vec4 in_vtx_offs;
layout (location = 2) AT_SAMPLE     in vec2 in_vtx_uv;
#define vtx_base AT_CENTRE_SMOOTH(in_vtx_base)
#define vtx_offs AT_CENTRE_SMOOTH(in_vtx_offs)
#define vtx_uv AT_CENTRE(in_vtx_uv)

layout (set = 0, binding = 2) uniform sampler2D fog_table;

float fog_mode2(float w)
{
	float z = w * uniformBuffer.sp_FOG_DENSITY;
	if (!(z >= 1.0))
		z = 1.0;
	if (z > 255.9999)
		z = 255.9999;
	float e = 0.0;
	if (z >= 16.0) { z *= 0.0625; e = 4.0; }
	if (z >= 4.0) { z *= 0.25; e += 2.0; }
	if (z >= 2.0) { z *= 0.5; e += 1.0; }
	float t = (z - 1.0) * 16.0;
	float i = floor(t);
	float blend = floor((t - i) * 256.0);
	float u = (e * 16.0 + i + 0.5) / 128.0;
	float next = floor(texture(fog_table, vec2(u, 0.25)).r * 255.0 + 0.5);
	float cur = floor(texture(fog_table, vec2(u, 0.75)).r * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

#if pp_Texture == 1
vec4 palettePixel(sampler2D tex, vec2 coords)
{
	vec4 c = vec4(TEX(tex, coords, uvGrad).r * (255.0 / 1024.0) + pushConstants.palette_index, 0.5, 0.0, 0.0);
	return texture(palette, c.xy);
}
#endif

void main()
{
#if pp_SampleRate == 2 && pp_Texture == 1
	// (first, while nothing is discarded: derivatives are defined only then)
	const vec2 c = interpolateAtOffset(in_vtx_uv, vec2(0.0));
	uvGrad = vec4(dFdx(c), dFdy(c));
#endif
	uint f = uint(pushConstants.flags);
	if ((f & PREPASS_CLIP_INSIDE) != 0u)
	{
		if (gl_FragCoord.x >= pushConstants.clipTest.x && gl_FragCoord.x <= pushConstants.clipTest.z
				&& gl_FragCoord.y >= pushConstants.clipTest.y && gl_FragCoord.y <= pushConstants.clipTest.w)
			discard;
	}

	vec4 color = vtx_base;
	if ((f & PREPASS_USE_ALPHA) == 0u)
		color.a = 1.0;
	#define offset vtx_offs
#if pp_Texture == 1
	{
		vec4 texcol;
		if ((f & PREPASS_PALETTE) == 0u)
			texcol = TEX(tex, vtx_uv, uvGrad);
		else
			texcol = palettePixel(tex, vtx_uv);

		if ((f & PREPASS_BUMP_MAP) != 0u)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);
		}
		else if ((f & PREPASS_IGNORE_TEX_A) != 0u)
			texcol.a = 1.0;
		uint instr = (f / PREPASS_SHAD_INSTR) & 3u;
		if (instr == 0u)
			color = texcol;
		else if (instr == 1u)
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		else if (instr == 3u)
			color *= texcol;
	}
#endif
	if ((f & PREPASS_CLAMP) != 0u)
		color = clamp(color, uniformBuffer.colorClampMin, uniformBuffer.colorClampMax);
	if ((f & PREPASS_FOG_ALPHA) != 0u)
		color = vec4(uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));

	if ((f & PREPASS_ALWAYS) == 0u && color.a < PREPASS_ALPHA_MIN_GLSL)
		discard;
}
