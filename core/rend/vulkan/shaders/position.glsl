// How every vertex shader of the raster renderers places a vertex: in
// front of each of them (tools/rend/kernels_spv.sh), so that they all
// place it by the same expression, from the same uniforms, and the
// position is invariant - the depth is the position's (rend/depth_scale.h),
// and a pass that tests EQUAL against another's depth is exact only if
// every pipeline computes the same position.

invariant gl_Position;

layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 normal_matrix;
	float depth_scale;	// 1/fZ_max (a frame decoded on the GPU: the GPU's word)
	float depth_mul;	// what the matrix scales z by, undone for the depth
} uniformBuffer;

// (x*w, y*w, S, w): a depth of S*z, 1/w as the chip has it, up to 1 at fZ_max
vec4 pvr_position(vec4 pos)
{
	vec4 vpos = uniformBuffer.normal_matrix * pos;
	vpos.w = 1.0 / vpos.z;
	vpos.xy *= vpos.w;
	// (a z beyond fZ_max is at depth 1, not clipped)
	vpos.z = min(uniformBuffer.depth_scale * uniformBuffer.depth_mul, vpos.w);
	return vpos;
}
