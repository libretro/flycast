// The per-triangle renderer's modifier volumes (shaders.cpp)

layout (location = 0) out vec4 FragColor;

layout (push_constant) uniform pushBlock
{
	float sp_ShaderColor;
} pushConstants;

void main()
{
	float w = gl_FragCoord.w * 100000.0;
	gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;
	FragColor = vec4(0.0, 0.0, 0.0, pushConstants.sp_ShaderColor);
}
