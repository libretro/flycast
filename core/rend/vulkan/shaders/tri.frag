// The per-triangle renderer's polygons (shaders.cpp). Made with pp_Gouraud
// (0 or 1) and pp_SampleRate (0 without SSAA; with it, 1 shaded at each
// sample, 2 once a pixel) defined, after pso_key.h: tools/rend/kernels_spv.sh.
// What else a polygon needs are the bits of the key (pso_key.h).

layout (constant_id = PSO_SC_KEY) const int sc_key = -1;
layout (constant_id = PSO_SC_SSAA_SAMPLES) const int SSAA_SAMPLES = 1;

#define PI 3.1415926

layout (location = 0) out vec4 FragColor;
#define gl_FragColor FragColor

/* SSAA: shaded at each sample, its inputs taken there (1); or once a
 * pixel, its inputs taken at the pixel's centre as without SSAA (2) -
 * where else they are taken when not said is the device's choice */
#if pp_SampleRate == 1
#define AT_SAMPLE sample
#else
#define AT_SAMPLE
#endif
#if pp_SampleRate == 2
// (or, where it covers only some of the samples, at a place it covers, as without SSAA it would only be drawn where it covers the centre)
#define AT_CENTRE(v) (gl_SampleMaskIn[0] == ((1 << SSAA_SAMPLES) - 1) ? interpolateAtOffset(v, vec2(0.0)) : interpolateAtCentroid(v))
#else
#define AT_CENTRE(v) (v)
#endif
#if pp_SampleRate == 2
/* ...and a texture is filtered with the derivatives of its coordinates at
 * the pixels' centres, as without SSAA. Where the polygon covers only some
 * of a pixel's samples its coordinates are taken at their centroid, so the
 * differences across a 2x2 block of pixels with such a pixel in it (along
 * every edge between two triangles) are not the texture's footprint: they
 * hold part of a half pixel's step in another direction, and anisotropic
 * filtering took its texels along that wrong line, far across the texture
 * - a dotted line of speckles on each edge. */
vec4 uvGrad;	// the coordinates' dFdx and dFdy at the centres (main())
#define TEX(t, coords, g) textureGrad(t, coords, (g).xy, (g).zw)
#else
#define TEX(t, coords, g) texture(t, coords)
#endif
// (a flat input is not interpolated, and is not to be asked for at a place)
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

layout (push_constant) uniform pushBlock
{
	vec4 clipTest;
	float trilinearAlpha;
	float palette_index;
	int key;	// the bits, when sc_key is -1
} pushConstants;

#define KEY (sc_key >= 0 ? sc_key : pushConstants.key)
#define KEY_BIT(n) ((KEY >> (n)) & 1)
#define cp_AlphaTest KEY_BIT(TRI_SC_ALPHA_TEST)
#define pp_ClipInside KEY_BIT(TRI_SC_CLIP_INSIDE)
#define pp_UseAlpha KEY_BIT(TRI_SC_USE_ALPHA)
#define pp_Texture KEY_BIT(TRI_SC_TEXTURE)
#define pp_IgnoreTexA KEY_BIT(TRI_SC_IGNORE_TEX_A)
#define pp_ShadInstr ((KEY >> TRI_SC_SHAD_INSTR) & 3)
#define pp_Offset KEY_BIT(TRI_SC_OFFSET)
#define pp_FogCtrl ((KEY >> TRI_SC_FOG_CTRL) & 3)
#define pp_BumpMap KEY_BIT(TRI_SC_BUMP_MAP)
#define ColorClamping KEY_BIT(TRI_SC_CLAMPING)
#define pp_TriLinear KEY_BIT(TRI_SC_TRILINEAR)
#define pp_Palette KEY_BIT(TRI_SC_PALETTE)
#define pp_Shadowed KEY_BIT(TRI_SC_SHADOWED)

layout (set = 1, binding = 0) uniform sampler2D tex;
layout (set = 0, binding = 3) uniform sampler2D palette;

// Vertex input
layout (location = 0) INTERPOLATION in lowp vec4 in_vtx_base;
layout (location = 1) INTERPOLATION in lowp vec4 in_vtx_offs;
layout (location = 2) AT_SAMPLE     in vec2 in_vtx_uv;
#define vtx_base AT_CENTRE_SMOOTH(in_vtx_base)
#define vtx_offs AT_CENTRE_SMOOTH(in_vtx_offs)
#define vtx_uv AT_CENTRE(in_vtx_uv)

layout (set = 0, binding = 2) uniform sampler2D fog_table;

// The fog coefficient for a depth, worked out the way the PowerVR2 does it.
// The depth times the fog density, between 1 and just under 256, is taken
// apart as a float: its exponent and the top four bits of its mantissa
// pick one of the 128 table entries, and the next eight bits of mantissa
// blend that entry with the following one. The blend is done in whole
// numbers out of 256 and so is the result, which is returned over 256.
// Splitting the float is done by comparing and halving, which is exact
// and costs less than the log2() and pow() this used to take.
float fog_mode2(float w)
{
	float z = w * uniformBuffer.sp_FOG_DENSITY;
	// Not clamp(): where the depth is infinite and the density zero their
	// product is not a number, and that has to come out as 1 like anything
	// else below 1. The texture lookup this used to end in swallowed it.
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
	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25
	float next = floor(texture(fog_table, vec2(u, 0.25)).r * 255.0 + 0.5);
	float cur = floor(texture(fog_table, vec2(u, 0.75)).r * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

vec4 colorClamp(vec4 col)
{
	if (ColorClamping == 1)
		return clamp(col, uniformBuffer.colorClampMin, uniformBuffer.colorClampMax);
	return col;
}

vec4 palettePixel(sampler2D tex, vec2 coords)
{
	// palette_index is where the middle of the palette's first colour is in the row of 1024
	vec4 c = vec4(TEX(tex, coords, uvGrad).r * (255.0 / 1024.0) + pushConstants.palette_index, 0.5, 0.0, 0.0);
	return texture(palette, c.xy);
}

void main()
{
#if pp_SampleRate == 2
	// (first, while nothing is discarded: derivatives are defined only then)
	if (pp_Texture == 1)
	{
		const vec2 c = interpolateAtOffset(in_vtx_uv, vec2(0.0));
		uvGrad = vec4(dFdx(c), dFdy(c));
	}
#endif
	// Clip inside the box
	if (pp_ClipInside == 1)
	{
		if (gl_FragCoord.x >= pushConstants.clipTest.x && gl_FragCoord.x <= pushConstants.clipTest.z
				&& gl_FragCoord.y >= pushConstants.clipTest.y && gl_FragCoord.y <= pushConstants.clipTest.w)
			discard;
	}

	vec4 color = vtx_base;
	if (pp_UseAlpha == 0)
		color.a = 1.0;
	vec4 offset = vtx_offs;
	if (pp_Shadowed == 1)
	{
		// A pixel inside a modifier volume: what the polygon is shaded with,
		// its base and offset colours, is scaled before the texture is
		// combined with them. The texture itself is not.
		color.rgb *= uniformBuffer.shade_scale_factor;
		offset = vec4(vtx_offs.rgb * uniformBuffer.shade_scale_factor, vtx_offs.a);
	}
	if (pp_Texture == 1)
	{
		vec4 texcol;
		if (pp_Palette == 0)
			texcol = TEX(tex, vtx_uv, uvGrad);
		else
			texcol = palettePixel(tex, vtx_uv);

		if (pp_BumpMap == 1)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);
		}
		else if (pp_IgnoreTexA == 1)
			texcol.a = 1.0;

		if (pp_ShadInstr == 0)
			color = texcol;
		else if (pp_ShadInstr == 1)
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		else if (pp_ShadInstr == 2)
			color.rgb = mix(color.rgb, texcol.rgb, texcol.a);
		else
			color *= texcol;

		if (pp_Offset == 1 && pp_BumpMap == 0)
			color.rgb += offset.rgb;
	}

	color = colorClamp(color);

	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha. This is a mode
	// of the fog unit, which comes after the texture has been combined in,
	// so it is the combined colour that is replaced: a textured polygon
	// comes out in plain fog colour. It used to be done to the base colour,
	// before texturing, and the texture was then applied on top.
	if (pp_FogCtrl == 3)
		color = vec4(uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));
	if (pp_FogCtrl == 0)
	{
		float fog = fog_mode2(gl_FragCoord.w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + uniformBuffer.sp_FOG_COL_RAM.rgb * fog;
	}
	if (pp_FogCtrl == 1 && pp_Offset == 1 && pp_BumpMap == 0)
	{
		float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + uniformBuffer.sp_FOG_COL_VERT.rgb * fog;
	}

	if (pp_TriLinear == 1)
		color *= pushConstants.trilinearAlpha;

	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	if (cp_AlphaTest == 1)
	{
		if (uniformBuffer.cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	}

	gl_FragColor = color;
}
