/* SSAA in the Vulkan raster renderers (both of them).
 *
 * With SSAA on, a frame is drawn into colour, depth and stencil
 * attachments of N samples a pixel (VulkanContext::SsaaSamples()), made
 * when the renderer starts or changes size and never stored where a frame
 * has one render pass: their contents go when the pass ends, and on a
 * device with memory that is only backed when used they take none. The
 * last subpass resolves them to the picture with the shared rule
 * (rend/tile/pvr_resolve.h, in integers), not the device's resolve.
 *
 * Polygons are shaded once per sample, at the sample, but for the ones
 * drawn at a single rate (rend/tile/pvr_tile.h): sprites, the background
 * and, where the decoder looks for them, flat strips. Those are shaded
 * once per pixel, at its centre, as with SSAA off; a pixel all of whose
 * samples they cover resolves to exactly what SSAA off draws. */
#pragma once
#include "vulkan_context.h"
#include "commandpool.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/tile/pvr_tile.h"
// (SsaaSingleRate(): which polygons are drawn at a single rate)
#include "rend/ssaa_rate.h"

// a pipeline's multisample state: @samples a pixel, shaded at each sample or once a pixel
static inline vk::PipelineMultisampleStateCreateInfo SsaaMultisample(u32 samples, bool perSample)
{
	vk::PipelineMultisampleStateCreateInfo info;
	info.rasterizationSamples = (vk::SampleCountFlagBits)samples;
	info.sampleShadingEnable = perSample && samples > 1;
	info.minSampleShading = 1.f;
	return info;
}

/* The resolve of an attachment of N samples into the subpass's single
 * sample colour attachment, as its input attachment 0: a triangle over
 * the whole framebuffer. Everything made in Init(). */
class SsaaResolve
{
public:
	void Init(vk::RenderPass renderPass, u32 subpass, u32 samples, vk::ImageView samplesView);
	void Draw(vk::CommandBuffer cmd, vk::Extent2D extent) const;
	bool Ready() const { return (bool)pipeline; }
	/* The attachment of samples made again: a new descriptor set for it, the
	 * old one kept until the frames that use it are done */
	void SetSamplesView(vk::ImageView samplesView, CommandPool *commandPool);
	void Term()
	{
		pipeline.reset();
		descSet.reset();
		pipelineLayout.reset();
		setLayout.reset();
		vertexShader.reset();
		fragmentShader.reset();
		samples = 0;
	}

private:
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueDescriptorSet descSet;
	vk::UniqueShaderModule vertexShader;
	vk::UniqueShaderModule fragmentShader;
	vk::UniquePipeline pipeline;
	u32 samples = 0;
};
