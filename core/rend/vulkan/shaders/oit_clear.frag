// The per-pixel renderer's lists emptied (oit_shaders.cpp), after oit_header.glsl

void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);

	// Reset pointers
	imageStore(abufferPointerImg, coords, uvec4(EOL));
}
