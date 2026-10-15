/* SSAA's resolve (quad.cpp), after rend/tile/pvr_lane.h and pvr_resolve.h
 * and pso_key.h. Each sample as the unorm8 it is held in (the attachment
 * is RGBA8: packUnorm4x8() gives back its bytes), summed two channels a
 * word, and (sum + N/2) / N per channel - which is the value written */
layout (constant_id = PSO_SC_SSAA_SAMPLES) const int SSAA_SAMPLES = 4;
layout (constant_id = PSO_SC_SSAA_LOG2N) const uint SSAA_LOG2N = 2u;

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInputMS samples;
layout (location = 0) out vec4 FragColor;

void main()
{
	uint rb = 0u;
	uint ga = 0u;
	for (int s = 0; s < SSAA_SAMPLES; s++)
	{
		uint c = packUnorm4x8(subpassLoad(samples, s));
		rb += pvr_resolve_rb(c);
		ga += pvr_resolve_ga(c);
	}
	FragColor = unpackUnorm4x8(pvr_resolve_join(rb, ga, SSAA_LOG2N));
}
