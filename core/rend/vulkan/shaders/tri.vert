// The per-triangle renderer's polygons (shaders.cpp). Made with pp_Gouraud
// defined, 0 or 1: tools/rend/kernels_spv.sh.

#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth
#endif

layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 normal_matrix;
} uniformBuffer;

layout (location = 0) in vec4         in_pos;
layout (location = 1) in uvec4        in_base;
layout (location = 2) in uvec4        in_offs;
layout (location = 3) in vec2 in_uv;

layout (location = 0) INTERPOLATION out lowp vec4 vtx_base;
layout (location = 1) INTERPOLATION out lowp vec4 vtx_offs;
layout (location = 2)               out vec2 vtx_uv;

void main()
{
	vtx_base = vec4(in_base) / 255.0;
	vtx_offs = vec4(in_offs) / 255.0;
	vtx_uv = in_uv;
	vec4 vpos = uniformBuffer.normal_matrix * in_pos;
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w;
	vpos.xy *= vpos.w;
	gl_Position = vpos;
}
