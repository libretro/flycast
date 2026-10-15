// Modifier volumes, of both renderers (shaders.cpp, oit_shaders.cpp)

layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 normal_matrix;
} uniformBuffer;

layout (location = 0) in vec4         in_pos;

void main()
{
	vec4 vpos = in_pos;
	if (vpos.z < 0.0 || vpos.z > 3.4e37)
	{
		gl_Position = vec4(0.0, 0.0, 1.0, 1.0 / vpos.z);
		return;
	}

	vpos = uniformBuffer.normal_matrix * vpos;
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w;
	vpos.xy *= vpos.w; 
	gl_Position = vpos;
}
