// The per-triangle renderer's modifier volumes (shaders.cpp)

layout (location = 0) out vec4 FragColor;

layout (push_constant) uniform pushBlock
{
	float sp_ShaderColor;
} pushConstants;

// (the depth is the position's: position.glsl; the colour is the final pass's tint)
void main()
{
	FragColor = vec4(0.0, 0.0, 0.0, pushConstants.sp_ShaderColor);
}
