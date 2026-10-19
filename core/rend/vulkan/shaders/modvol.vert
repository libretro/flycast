// Modifier volumes, of both renderers (shaders.cpp, oit_shaders.cpp), after
// position.glsl

layout (location = 0) in vec4         in_pos;

void main()
{
	vec4 vpos = in_pos;
	if (vpos.z < 0.0 || vpos.z > 3.4e37)
	{
		gl_Position = vec4(0.0, 0.0, 1.0, 1.0 / vpos.z);
		return;
	}

	gl_Position = pvr_position(vpos);
}
