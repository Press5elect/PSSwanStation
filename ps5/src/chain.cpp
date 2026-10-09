/*
	PSSwanStation - libretro slang shader presets, run on the game's picture.

	SPDX-License-Identifier: GPL-3.0-or-later

	A preset of github.com/libretro/slang-shaders (crt-guest-advanced, by
	guest(r), GPL-2.0-or-later) is made into SPIR-V when the title is built
	(ps5/tools/make-chains.py): each pass's two stages, and what each reads,
	found in the SPIR-V by name. This runs it the way RetroArch's slang runtime
	does, as far as such presets need:

	  - each pass draws a quad into a picture of its own, sized from the pass
	    before ("source"), the screen's picture ("viewport") or in pixels
	    ("absolute"), in the format it asks for;
	  - it reads, by name, the chain's input (Original, OriginalHistory0), the
	    pass before (Source), any pass before by number or alias (PassOutputN),
	    a pass's own last picture (PassFeedbackN, <alias>Feedback) and the
	    preset's lookup pictures; a picture is sampled as the pass that took it
	    first asks (linear or nearest, its wrap mode), with mipmaps when that
	    pass asks for them;
	  - its push constants and uniform block are filled by name: MVP, the sizes
	    (OutputSize, SourceSize, OriginalSize, FinalViewportSize, <alias>Size,
	    PassOutputSizeN...), FrameCount and the preset's parameters.

	OriginalHistoryN beyond 0 is the current picture too: the title keeps no
	older ones.
*/
#include "chain.h"
#include "fe.h"

#include "common/vulkan/context.h"
#include "common/vulkan/staging_buffer.h"
#include "common/vulkan/texture.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

namespace fe::chain
{

namespace
{

constexpr unsigned Ring = 8;			// sets and uniform data per pass, going round
constexpr uint32_t UboStride = 512;		// the most a pass's uniform block may be, aligned

enum Semantic
{
	SemNone, SemParam, SemMvp, SemOutputSize, SemSourceSize, SemOriginalSize, SemViewportSize, SemFrameCount,
	SemFrameDirection, SemOne, SemZero, SemPassOutputSize, SemPassFeedbackSize, SemLutSize,
	SemOriginalFps, SemFrameTimeDelta
};

// The game's frame rate, for the newer presets that ask for it (OriginalFPS,
// FrameTimeDelta).
float contentFps = 60.f;
enum TexKind { TexOriginal, TexSource, TexPassOutput, TexPassFeedback, TexLut };
enum Scale { ScaleSource, ScaleViewport, ScaleAbsolute };

struct Member
{
	bool push = false;
	uint32_t offset = 0;
	std::string kind, name;
	Semantic semantic = SemNone;
	int index = 0;
};

struct TexRef
{
	uint32_t binding = 0;
	TexKind kind = TexOriginal;
	int index = 0;
};

// A picture a pass draws into, and samples from later.
struct Image
{
	Vulkan::Texture texture;
	VkImageView level0 = VK_NULL_HANDLE;	// the framebuffer's view, when it has mipmaps
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	int width = 0, height = 0;
	uint32_t levels = 1;
};

struct Pass
{
	std::string vert, frag;
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	bool linear = false, mipmapInput = false;
	VkSamplerAddressMode wrap = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	int frameMod = 0;
	std::string alias;
	Scale xType = ScaleSource, yType = ScaleSource;
	float x = 1, y = 1;
	uint32_t pushSize = 0;
	int uboBinding = -1;
	uint32_t uboSize = 0;
	std::vector<Member> members;
	std::vector<std::pair<uint32_t, std::string>> samplers;
	std::vector<TexRef> textures;
	bool feedback = false;			// a later pass (or this) reads its last picture
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkDescriptorSet sets[Ring] = {};
	Image output, previous;
	bool previousValid = false;
};

struct Lut
{
	std::string name, file;
	bool linear = false, mipmap = false;
	VkSamplerAddressMode wrap = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	Vulkan::Texture texture;
	bool uploaded = false;
};

struct Param
{
	std::string name;
	float preset = 0, low = 0, high = 0, value = 0;
};

VkSamplerAddressMode wrapMode(const std::string& name)
{
	if (name == "clamp_to_edge")
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (name == "repeat")
		return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	if (name == "mirrored_repeat")
		return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
}

VkFormat formatNamed(const std::string& name)
{
	static const std::map<std::string, VkFormat> formats = {
		{ "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM }, { "R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB },
		{ "R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT }, { "R32G32B32A32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT },
		{ "A2B10G10R10_UNORM_PACK32", VK_FORMAT_A2B10G10R10_UNORM_PACK32 }, { "R16G16_SFLOAT", VK_FORMAT_R16G16_SFLOAT },
		{ "R32_SFLOAT", VK_FORMAT_R32_SFLOAT }, { "R8_UNORM", VK_FORMAT_R8_UNORM }, { "R16_SFLOAT", VK_FORMAT_R16_SFLOAT },
		{ "R32G32_SFLOAT", VK_FORMAT_R32G32_SFLOAT },
	};
	const auto it = formats.find(name);
	return it != formats.end() ? it->second : VK_FORMAT_UNDEFINED;
}

Scale scaleNamed(const std::string& name)
{
	return name == "viewport" ? ScaleViewport : name == "absolute" ? ScaleAbsolute : ScaleSource;
}

// "Name<number>" as the number, when `text` starts with `prefix` and ends in one.
bool numbered(const std::string& text, const char *prefix, int& number)
{
	const size_t n = strlen(prefix);
	if (text.size() <= n || text.compare(0, n, prefix) != 0)
		return false;
	for (size_t i = n; i < text.size(); i++)
		if (text[i] < '0' || text[i] > '9')
			return false;
	number = atoi(text.c_str() + n);
	return true;
}

} // namespace

struct Chain
{
	std::string folder, name;
	std::vector<Param> params;
	std::vector<Lut> luts;
	std::vector<Pass> passes;
	std::map<VkFormat, VkRenderPass> renderPasses;
	std::map<uint32_t, VkSampler> samplers;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	Vulkan::StagingBuffer vertices, uniforms, lutStaging;
	uint32_t uniformStride = UboStride;
	unsigned slot = 0;
	uint32_t frameCount = 0;
	int lastW = 0, lastH = 0, lastOutW = 0, lastOutH = 0;
	bool failed = false;
};

namespace
{

bool parse(Chain& c)
{
	std::vector<uint8_t> data;
	if (!readFile(c.folder + "/chain.txt", data))
		return false;
	std::istringstream in(std::string(data.begin(), data.end()));
	std::string line;
	while (std::getline(in, line))
	{
		std::istringstream words(line);
		std::string what;
		words >> what;
		if (what == "chain")
			words >> c.name;
		else if (what == "param")
		{
			Param p;
			words >> p.name >> p.preset >> p.low >> p.high;
			p.value = p.preset;
			c.params.push_back(p);
		}
		else if (what == "lut")
		{
			Lut l;
			int linear = 0, mipmap = 0;
			std::string wrap;
			words >> l.name >> l.file >> linear >> mipmap >> wrap;
			l.linear = linear != 0;
			l.mipmap = mipmap != 0;
			l.wrap = wrapMode(wrap);
			c.luts.push_back(std::move(l));
		}
		else if (what == "pass")
		{
			Pass p;
			int index = 0, linear = 0, mipmap = 0;
			std::string format, wrap, xType, yType;
			words >> index >> p.vert >> p.frag >> format >> linear >> wrap >> mipmap >> p.frameMod >> p.alias >> xType >> p.x
					>> yType >> p.y;
			p.format = formatNamed(format);
			p.linear = linear != 0;
			p.wrap = wrapMode(wrap);
			p.mipmapInput = mipmap != 0;
			p.xType = scaleNamed(xType);
			p.yType = scaleNamed(yType);
			if (p.alias == "-")
				p.alias.clear();
			if (p.format == VK_FORMAT_UNDEFINED || words.fail())
				return false;
			c.passes.push_back(std::move(p));
		}
		else if (c.passes.empty())
			continue;
		else if (what == "push")
			words >> c.passes.back().pushSize;
		else if (what == "ubo")
			words >> c.passes.back().uboBinding >> c.passes.back().uboSize;
		else if (what == "member")
		{
			Member m;
			std::string where;
			words >> where >> m.offset >> m.kind >> m.name;
			m.push = where == "push";
			c.passes.back().members.push_back(m);
		}
		else if (what == "sampler")
		{
			uint32_t binding = 0;
			std::string name;
			words >> binding >> name;
			c.passes.back().samplers.push_back({ binding, name });
		}
	}
	return !c.passes.empty();
}

// Who each name a pass reads is.
void resolve(Chain& c)
{
	auto passNamed = [&](const std::string& name, size_t before) {
		for (size_t i = 0; i < before && i < c.passes.size(); i++)
			if (!c.passes[i].alias.empty() && c.passes[i].alias == name)
				return (int)i;
		return -1;
	};
	auto lutNamed = [&](const std::string& name) {
		for (size_t i = 0; i < c.luts.size(); i++)
			if (c.luts[i].name == name)
				return (int)i;
		return -1;
	};
	auto paramNamed = [&](const std::string& name) {
		for (size_t i = 0; i < c.params.size(); i++)
			if (c.params[i].name == name)
				return (int)i;
		return -1;
	};
	for (size_t p = 0; p < c.passes.size(); p++)
	{
		Pass& pass = c.passes[p];
		for (Member& m : pass.members)
		{
			int n = 0;
			const std::string& name = m.name;
			if (name == "MVP")
				m.semantic = SemMvp;
			else if (name == "OutputSize")
				m.semantic = SemOutputSize;
			else if (name == "SourceSize")
				m.semantic = SemSourceSize;
			else if (name == "OriginalSize" || numbered(name, "OriginalHistorySize", n))
				m.semantic = SemOriginalSize;
			else if (name == "FinalViewportSize")
				m.semantic = SemViewportSize;
			else if (name == "FrameCount")
				m.semantic = SemFrameCount;
			else if (name == "FrameDirection" || name == "TotalSubFrames" || name == "CurrentSubFrame")
				m.semantic = SemOne;
			else if (name == "Rotation")
				m.semantic = SemZero;
			else if (name == "OriginalFPS")
				m.semantic = SemOriginalFps;
			else if (name == "FrameTimeDelta")
				m.semantic = SemFrameTimeDelta;
			else if (numbered(name, "PassOutputSize", n))
			{
				m.semantic = SemPassOutputSize;
				m.index = n;
			}
			else if (numbered(name, "PassFeedbackSize", n))
			{
				m.semantic = SemPassFeedbackSize;
				m.index = n;
			}
			else if ((m.index = paramNamed(name)) >= 0)
				m.semantic = SemParam;
			else
			{
				m.semantic = SemNone;
				// <alias>Size, <alias>FeedbackSize, <lut>Size
				const size_t size = name.size();
				if (size > 4 && name.compare(size - 4, 4, "Size") == 0)
				{
					const std::string base = name.substr(0, size - 4);
					int which = -1;
					if (base.size() > 8 && base.compare(base.size() - 8, 8, "Feedback") == 0
							&& (which = passNamed(base.substr(0, base.size() - 8), c.passes.size())) >= 0)
					{
						m.semantic = SemPassFeedbackSize;
						m.index = which;
					}
					else if ((which = passNamed(base, c.passes.size())) >= 0)
					{
						m.semantic = SemPassOutputSize;
						m.index = which;
					}
					else if ((which = lutNamed(base)) >= 0)
					{
						m.semantic = SemLutSize;
						m.index = which;
					}
				}
			}
		}
		for (const auto& [binding, name] : pass.samplers)
		{
			TexRef ref;
			ref.binding = binding;
			int n = 0, which = -1;
			if (name == "Source")
				ref.kind = p == 0 ? TexOriginal : TexSource;
			else if (name == "Original" || numbered(name, "OriginalHistory", n))
				ref.kind = TexOriginal;
			else if (numbered(name, "PassOutput", n) && n < (int)p)
			{
				ref.kind = TexPassOutput;
				ref.index = n;
			}
			else if (numbered(name, "PassFeedback", n) && n < (int)c.passes.size())
			{
				ref.kind = TexPassFeedback;
				ref.index = n;
			}
			else if (name.size() > 8 && name.compare(name.size() - 8, 8, "Feedback") == 0
					&& (which = passNamed(name.substr(0, name.size() - 8), c.passes.size())) >= 0)
			{
				ref.kind = TexPassFeedback;
				ref.index = which;
			}
			else if ((which = passNamed(name, p)) >= 0)
			{
				ref.kind = TexPassOutput;
				ref.index = which;
			}
			else if ((which = lutNamed(name)) >= 0)
			{
				ref.kind = TexLut;
				ref.index = which;
			}
			else
				diag::mark("chain: %s: pass %d reads %s, which nothing makes: it gets the input", c.name.c_str(), (int)p,
						name.c_str());
			if (ref.kind == TexSource)
			{
				ref.kind = TexPassOutput;
				ref.index = (int)p - 1;
			}
			if (ref.kind == TexPassFeedback)
				c.passes[ref.index].feedback = true;
			pass.textures.push_back(ref);
		}
	}
}

VkRenderPass renderPassFor(Chain& c, VkFormat format)
{
	const auto it = c.renderPasses.find(format);
	if (it != c.renderPasses.end())
		return it->second;
	VkAttachmentDescription attachment{};
	attachment.format = format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	VkSubpassDependency around[2] = {};
	around[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	around[0].dstSubpass = 0;
	around[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	around[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	around[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	around[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	around[1].srcSubpass = 0;
	around[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	around[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	around[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
			| VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	around[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	around[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.attachmentCount = 1;
	info.pAttachments = &attachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = around;
	VkRenderPass pass = VK_NULL_HANDLE;
	if (vkCreateRenderPass(g_vulkan_context->GetDevice(), &info, nullptr, &pass) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	c.renderPasses[format] = pass;
	return pass;
}

VkSampler samplerFor(Chain& c, bool linear, bool mipmap, VkSamplerAddressMode wrap)
{
	const uint32_t key = (linear ? 1u : 0u) | (mipmap ? 2u : 0u) | ((uint32_t)wrap << 2);
	const auto it = c.samplers.find(key);
	if (it != c.samplers.end())
		return it->second;
	VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	info.magFilter = info.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = info.addressModeV = info.addressModeW = wrap;
	info.maxLod = mipmap ? VK_LOD_CLAMP_NONE : 0.f;
	info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	info.maxAnisotropy = 1.f;
	VkSampler sampler = VK_NULL_HANDLE;
	vkCreateSampler(g_vulkan_context->GetDevice(), &info, nullptr, &sampler);
	c.samplers[key] = sampler;
	return sampler;
}

VkShaderModule moduleFrom(const std::string& path)
{
	std::vector<uint8_t> data;
	if (!readFile(path, data) || data.size() < 20 || data.size() % 4 != 0)
		return VK_NULL_HANDLE;
	VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = data.size();
	info.pCode = reinterpret_cast<const uint32_t *>(data.data());
	VkShaderModule module = VK_NULL_HANDLE;
	vkCreateShaderModule(g_vulkan_context->GetDevice(), &info, nullptr, &module);
	return module;
}

bool build(Chain& c)
{
	VkDevice device = g_vulkan_context->GetDevice();
	uint32_t uboCount = 0, imageCount = 0;
	for (const Pass& pass : c.passes)
	{
		uboCount += pass.uboBinding >= 0 ? 1 : 0;
		imageCount += (uint32_t)pass.samplers.size();
	}
	std::vector<VkDescriptorPoolSize> sizes;
	if (uboCount > 0)
		sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uboCount * Ring });
	if (imageCount > 0)
		sizes.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount * Ring });
	VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.maxSets = (uint32_t)c.passes.size() * Ring;
	poolInfo.poolSizeCount = (uint32_t)sizes.size();
	poolInfo.pPoolSizes = sizes.data();
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &c.pool) != VK_SUCCESS)
		return false;
	const uint32_t align = std::max<uint32_t>((uint32_t)g_vulkan_context->GetDeviceLimits().minUniformBufferOffsetAlignment, 16);
	c.uniformStride = (UboStride + align - 1) / align * align;
	for (const Pass& pass : c.passes)
		if (pass.uboSize > UboStride)
		{
			diag::mark("chain: %s: a uniform block of %u bytes is more than this runner keeps", c.name.c_str(), pass.uboSize);
			return false;
		}
	if (!c.uniforms.Create(Vulkan::StagingBuffer::Type::Upload, (VkDeviceSize)c.uniformStride * c.passes.size() * Ring,
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !c.uniforms.Map())
		return false;
	// A quad over the picture: position (x, y, 0, 1), then where it samples.
	static const float quad[4][6] = {
		{ 0, 0, 0, 1, 0, 0 }, { 1, 0, 0, 1, 1, 0 }, { 0, 1, 0, 1, 0, 1 }, { 1, 1, 0, 1, 1, 1 },
	};
	if (!c.vertices.Create(Vulkan::StagingBuffer::Type::Upload, sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
			|| !c.vertices.Map())
		return false;
	memcpy(c.vertices.GetMapPointer(), quad, sizeof(quad));
	c.vertices.FlushCPUCache();

	for (size_t p = 0; p < c.passes.size(); p++)
	{
		Pass& pass = c.passes[p];
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		if (pass.uboBinding >= 0)
		{
			VkDescriptorSetLayoutBinding b{};
			b.binding = (uint32_t)pass.uboBinding;
			b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			b.descriptorCount = 1;
			b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			bindings.push_back(b);
		}
		for (const TexRef& ref : pass.textures)
		{
			VkDescriptorSetLayoutBinding b{};
			b.binding = ref.binding;
			b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			b.descriptorCount = 1;
			b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			bindings.push_back(b);
		}
		VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		layoutInfo.bindingCount = (uint32_t)bindings.size();
		layoutInfo.pBindings = bindings.data();
		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &pass.setLayout) != VK_SUCCESS)
			return false;
		VkPushConstantRange range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pass.pushSize };
		VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pipelineLayout.setLayoutCount = 1;
		pipelineLayout.pSetLayouts = &pass.setLayout;
		pipelineLayout.pushConstantRangeCount = pass.pushSize > 0 ? 1 : 0;
		pipelineLayout.pPushConstantRanges = &range;
		if (vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &pass.layout) != VK_SUCCESS)
			return false;
		VkDescriptorSetLayout layouts[Ring];
		for (VkDescriptorSetLayout& layout : layouts)
			layout = pass.setLayout;
		VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocate.descriptorPool = c.pool;
		allocate.descriptorSetCount = Ring;
		allocate.pSetLayouts = layouts;
		if (vkAllocateDescriptorSets(device, &allocate, pass.sets) != VK_SUCCESS)
			return false;

		const VkShaderModule vertex = moduleFrom(c.folder + "/" + pass.vert);
		const VkShaderModule fragment = moduleFrom(c.folder + "/" + pass.frag);
		const VkRenderPass renderPass = renderPassFor(c, pass.format);
		if (vertex != VK_NULL_HANDLE && fragment != VK_NULL_HANDLE && renderPass != VK_NULL_HANDLE)
		{
			VkPipelineShaderStageCreateInfo stages[2] = {};
			stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			stages[0].module = vertex;
			stages[0].pName = "main";
			stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[1].module = fragment;
			stages[1].pName = "main";
			VkVertexInputBindingDescription binding{ 0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
			VkVertexInputAttributeDescription attributes[2] = {
				{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
				{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float) },
			};
			VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vertexInput.vertexBindingDescriptionCount = 1;
			vertexInput.pVertexBindingDescriptions = &binding;
			vertexInput.vertexAttributeDescriptionCount = 2;
			vertexInput.pVertexAttributeDescriptions = attributes;
			VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			viewport.viewportCount = 1;
			viewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			raster.polygonMode = VK_POLYGON_MODE_FILL;
			raster.cullMode = VK_CULL_MODE_NONE;
			raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			raster.lineWidth = 1.f;
			VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineColorBlendAttachmentState attachment{};
			attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
					| VK_COLOR_COMPONENT_A_BIT;
			VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			blend.attachmentCount = 1;
			blend.pAttachments = &attachment;
			const VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dynamic.dynamicStateCount = 2;
			dynamic.pDynamicStates = dynamicStates;
			VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			info.stageCount = 2;
			info.pStages = stages;
			info.pVertexInputState = &vertexInput;
			info.pInputAssemblyState = &assembly;
			info.pViewportState = &viewport;
			info.pRasterizationState = &raster;
			info.pMultisampleState = &multisample;
			info.pColorBlendState = &blend;
			info.pDynamicState = &dynamic;
			info.layout = pass.layout;
			info.renderPass = renderPass;
			vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pass.pipeline);
		}
		for (VkShaderModule module : { vertex, fragment })
			if (module != VK_NULL_HANDLE)
				vkDestroyShaderModule(device, module, nullptr);
		if (pass.pipeline == VK_NULL_HANDLE)
		{
			diag::mark("chain: %s: pass %d (%s) could not be made", c.name.c_str(), (int)p, pass.frag.c_str());
			return false;
		}
	}
	return true;
}

void destroyImage(Image& image, bool defer)
{
	if (image.framebuffer != VK_NULL_HANDLE)
	{
		if (defer)
			g_vulkan_context->DeferFramebufferDestruction(image.framebuffer);
		else
			vkDestroyFramebuffer(g_vulkan_context->GetDevice(), image.framebuffer, nullptr);
	}
	if (image.level0 != VK_NULL_HANDLE)
	{
		if (defer)
			g_vulkan_context->DeferImageViewDestruction(image.level0);
		else
			vkDestroyImageView(g_vulkan_context->GetDevice(), image.level0, nullptr);
	}
	image.texture.Destroy(defer);
	image = Image();
}

// Cleared to transparent black and left readable: what a pass reads of a
// picture not drawn yet (the first frame's feedback).
void clearImage(VkCommandBuffer cmd, Image& image)
{
	image.texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkClearColorValue black{};
	const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, image.levels, 0, 1 };
	vkCmdClearColorImage(cmd, image.texture.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	image.texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool ensureImage(Chain& c, VkCommandBuffer cmd, Image& image, int width, int height, uint32_t levels, VkFormat format)
{
	if (image.texture.IsValid() && image.width == width && image.height == height && image.levels == levels
			&& image.texture.GetFormat() == format)
		return true;
	destroyImage(image, true);
	if (!image.texture.Create((uint32_t)width, (uint32_t)height, levels, 1, format, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
						| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		return false;
	VkImageView attach = image.texture.GetView();
	if (levels > 1)
	{
		VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = image.texture.GetImage();
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (vkCreateImageView(g_vulkan_context->GetDevice(), &viewInfo, nullptr, &image.level0) != VK_SUCCESS)
			return false;
		attach = image.level0;
	}
	VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	info.renderPass = renderPassFor(c, format);
	info.attachmentCount = 1;
	info.pAttachments = &attach;
	info.width = (uint32_t)width;
	info.height = (uint32_t)height;
	info.layers = 1;
	if (info.renderPass == VK_NULL_HANDLE
			|| vkCreateFramebuffer(g_vulkan_context->GetDevice(), &info, nullptr, &image.framebuffer) != VK_SUCCESS)
		return false;
	image.width = width;
	image.height = height;
	image.levels = levels;
	clearImage(cmd, image);
	return true;
}

// The levels below the first, each half the one above.
void makeMipmaps(VkCommandBuffer cmd, Image& image)
{
	if (image.levels <= 1)
		return;
	VkImage handle = image.texture.GetImage();
	image.texture.TransitionSubresourcesToLayout(cmd, 0, 1, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	image.texture.TransitionSubresourcesToLayout(cmd, 1, image.levels - 1, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	for (uint32_t level = 1; level < image.levels; level++)
	{
		VkImageBlit blit{};
		blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
		blit.srcOffsets[1] = { (int32_t)image.texture.GetMipWidth(level - 1), (int32_t)image.texture.GetMipHeight(level - 1), 1 };
		blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
		blit.dstOffsets[1] = { (int32_t)image.texture.GetMipWidth(level), (int32_t)image.texture.GetMipHeight(level), 1 };
		vkCmdBlitImage(cmd, handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
				VK_FILTER_LINEAR);
		image.texture.TransitionSubresourcesToLayout(cmd, level, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	}
	image.texture.TransitionSubresourcesToLayout(cmd, 0, image.levels, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	image.texture.OverrideImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool uploadLuts(Chain& c, VkCommandBuffer cmd)
{
	std::vector<std::vector<uint8_t>> pixels(c.luts.size());
	std::vector<int> widths(c.luts.size()), heights(c.luts.size());
	size_t total = 0;
	for (size_t i = 0; i < c.luts.size(); i++)
	{
		std::vector<uint8_t> file;
		int w = 0, h = 0, channels = 0;
		stbi_uc *data = readFile(c.folder + "/" + c.luts[i].file, file)
				? stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &channels, 4) : nullptr;
		if (data == nullptr)
		{
			diag::mark("chain: %s: the picture %s cannot be read", c.name.c_str(), c.luts[i].file.c_str());
			return false;
		}
		pixels[i].assign(data, data + (size_t)w * h * 4);
		stbi_image_free(data);
		widths[i] = w;
		heights[i] = h;
		total += pixels[i].size();
	}
	if (total == 0)
		return true;
	if (!c.lutStaging.Create(Vulkan::StagingBuffer::Type::Upload, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !c.lutStaging.Map())
		return false;
	size_t offset = 0;
	for (size_t i = 0; i < c.luts.size(); i++)
	{
		Lut& lut = c.luts[i];
		memcpy(c.lutStaging.GetMapPointer() + offset, pixels[i].data(), pixels[i].size());
		if (!lut.texture.Create((uint32_t)widths[i], (uint32_t)heights[i], 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
			return false;
		lut.texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		lut.texture.UpdateFromBuffer(cmd, 0, 0, 0, 0, (uint32_t)widths[i], (uint32_t)heights[i], c.lutStaging.GetBuffer(),
				(uint32_t)offset, (uint32_t)widths[i]);
		lut.texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		lut.uploaded = true;
		offset += pixels[i].size();
	}
	c.lutStaging.FlushCPUCache();
	return true;
}

int scaled(Scale type, float factor, int source, int viewport)
{
	const float base = type == ScaleSource ? (float)source : type == ScaleViewport ? (float)viewport : 1.f;
	return std::clamp((int)std::lround(base * factor), 1, 8192);
}

} // namespace

Chain *load(const std::string& folder)
{
	auto c = std::make_unique<Chain>();
	c->folder = folder;
	if (!parse(*c))
	{
		diag::mark("chain: %s/chain.txt cannot be read", folder.c_str());
		return nullptr;
	}
	resolve(*c);
	if (!build(*c))
	{
		diag::mark("chain: %s: the driver refused a part of it", c->name.c_str());
		destroy(c.release());
		return nullptr;
	}
	diag::mark("chain: %s: %d passes, %d parameters, %d pictures", c->name.c_str(), (int)c->passes.size(),
			(int)c->params.size(), (int)c->luts.size());
	return c.release();
}

void destroy(Chain *c)
{
	if (c == nullptr)
		return;
	g_vulkan_context->WaitForGPUIdle();
	VkDevice device = g_vulkan_context->GetDevice();
	for (Pass& pass : c->passes)
	{
		destroyImage(pass.output, false);
		destroyImage(pass.previous, false);
		if (pass.pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(device, pass.pipeline, nullptr);
		if (pass.layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(device, pass.layout, nullptr);
		if (pass.setLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(device, pass.setLayout, nullptr);
	}
	for (Lut& lut : c->luts)
		lut.texture.Destroy(false);
	for (auto& [format, pass] : c->renderPasses)
		vkDestroyRenderPass(device, pass, nullptr);
	for (auto& [key, sampler] : c->samplers)
		if (sampler != VK_NULL_HANDLE)
			vkDestroySampler(device, sampler, nullptr);
	if (c->pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(device, c->pool, nullptr);
	c->vertices.Destroy(false);
	c->uniforms.Destroy(false);
	c->lutStaging.Destroy(false);
	delete c;
}

void resetParameters(Chain *c)
{
	if (c != nullptr)
		for (Param& p : c->params)
			p.value = p.preset;
}

bool setParameter(Chain *c, const std::string& name, float value)
{
	if (c != nullptr)
		for (Param& p : c->params)
			if (p.name == name)
			{
				p.value = std::clamp(value, std::min(p.low, p.high), std::max(p.low, p.high));
				return true;
			}
	return false;
}

void setContentRate(float fps)
{
	if (fps > 1.f && fps < 1000.f)
		contentFps = fps;
}

void forget(Chain *c)
{
	if (c == nullptr)
		return;
	for (Pass& pass : c->passes)
		pass.previousValid = false;
	c->frameCount = 0;
}

VkImageView run(Chain *c, VkCommandBuffer cmd, VkImageView input, VkImageLayout inputLayout, int width, int height,
		int outWidth, int outHeight)
{
	if (c == nullptr || c->failed || input == VK_NULL_HANDLE || width < 1 || height < 1 || outWidth < 1 || outHeight < 1)
		return VK_NULL_HANDLE;
	if (!c->luts.empty() && !c->luts[0].uploaded && !uploadLuts(*c, cmd))
	{
		c->failed = true;
		return VK_NULL_HANDLE;
	}
	const size_t count = c->passes.size();
	// The sizes, from the input on.
	std::vector<int> w(count), h(count);
	for (size_t p = 0; p < count; p++)
	{
		const int sourceW = p == 0 ? width : w[p - 1], sourceH = p == 0 ? height : h[p - 1];
		w[p] = scaled(c->passes[p].xType, c->passes[p].x, sourceW, outWidth);
		h[p] = scaled(c->passes[p].yType, c->passes[p].y, sourceH, outHeight);
	}
	// Each picture, with mipmaps when the next pass asks for them; a pass whose
	// last picture is read keeps it.
	for (size_t p = 0; p < count; p++)
	{
		Pass& pass = c->passes[p];
		const bool mips = p + 1 < count && c->passes[p + 1].mipmapInput;
		const uint32_t levels = mips ? (uint32_t)std::floor(std::log2((double)std::max(w[p], h[p]))) + 1 : 1;
		if (pass.feedback)
		{
			std::swap(pass.output, pass.previous);
			if (!ensureImage(*c, cmd, pass.previous, w[p], h[p], levels, pass.format))
				return VK_NULL_HANDLE;
			if (pass.previous.width != pass.output.width || pass.previous.height != pass.output.height)
				pass.previousValid = false;
		}
		if (!ensureImage(*c, cmd, pass.output, w[p], h[p], levels, pass.format))
		{
			diag::mark("chain: %s: no picture of %d x %d for pass %d", c->name.c_str(), w[p], h[p], (int)p);
			c->failed = true;
			return VK_NULL_HANDLE;
		}
	}
	if (c->lastW != width || c->lastH != height || c->lastOutW != outWidth || c->lastOutH != outHeight)
	{
		diag::mark("chain: %s: %d x %d to %d x %d", c->name.c_str(), width, height, outWidth, outHeight);
		c->lastW = width;
		c->lastH = height;
		c->lastOutW = outWidth;
		c->lastOutH = outHeight;
	}
	VkDevice device = g_vulkan_context->GetDevice();
	const unsigned slot = c->slot++ % Ring;
	static const float mvp[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1 };
	auto size4 = [](int sw, int sh, float out[4]) {
		out[0] = (float)sw;
		out[1] = (float)sh;
		out[2] = 1.f / (float)sw;
		out[3] = 1.f / (float)sh;
	};
	for (size_t p = 0; p < count; p++)
	{
		Pass& pass = c->passes[p];
		// The values, by name.
		uint8_t push[256] = {};
		const uint32_t uboOffset = (uint32_t)((slot * count + p) * c->uniformStride);
		uint8_t *ubo = reinterpret_cast<uint8_t *>(c->uniforms.GetMapPointer()) + uboOffset;
		memset(ubo, 0, pass.uboSize);
		const int sourceW = p == 0 ? width : w[p - 1], sourceH = p == 0 ? height : h[p - 1];
		for (const Member& m : pass.members)
		{
			uint8_t *at = (m.push ? push : ubo) + m.offset;
			if (m.offset + (m.kind == "mat4" ? 64u : m.kind == "vec4" ? 16u : 4u) > (m.push ? sizeof(push) : pass.uboSize))
				continue;
			float v4[4] = {};
			switch (m.semantic)
			{
			case SemMvp:
				memcpy(at, mvp, sizeof(mvp));
				continue;
			case SemOutputSize: size4(w[p], h[p], v4); break;
			case SemSourceSize: size4(sourceW, sourceH, v4); break;
			case SemOriginalSize: size4(width, height, v4); break;
			case SemViewportSize: size4(outWidth, outHeight, v4); break;
			case SemPassOutputSize:
			case SemPassFeedbackSize:
				if (m.index >= 0 && m.index < (int)count)
					size4(w[m.index], h[m.index], v4);
				break;
			case SemLutSize:
				if (m.index >= 0 && m.index < (int)c->luts.size())
					size4((int)c->luts[m.index].texture.GetWidth(), (int)c->luts[m.index].texture.GetHeight(), v4);
				break;
			case SemFrameCount:
			{
				const uint32_t frame = pass.frameMod > 0 ? c->frameCount % (uint32_t)pass.frameMod : c->frameCount;
				if (m.kind == "float")
					v4[0] = (float)frame;
				else
				{
					memcpy(at, &frame, 4);
					continue;
				}
				break;
			}
			case SemOne:
				if (m.kind == "float")
					v4[0] = 1.f;
				else
				{
					const int32_t one = 1;
					memcpy(at, &one, 4);
					continue;
				}
				break;
			case SemParam: v4[0] = c->params[m.index].value; break;
			case SemOriginalFps: v4[0] = contentFps; break;
			case SemFrameTimeDelta:
			{
				// RetroArch's: the time from one frame to the next, in microseconds.
				const uint32_t micro = (uint32_t)std::lround(1e6 / std::max(contentFps, 1.f));
				if (m.kind == "float")
					v4[0] = (float)micro;
				else
				{
					memcpy(at, &micro, 4);
					continue;
				}
				break;
			}
			default: break;
			}
			memcpy(at, v4, m.kind == "vec4" ? 16 : 4);
		}
		// The pictures it reads.
		const VkDescriptorSet set = pass.sets[slot];
		std::vector<VkDescriptorImageInfo> images(pass.textures.size());
		std::vector<VkWriteDescriptorSet> writes;
		VkDescriptorBufferInfo buffer{ c->uniforms.GetBuffer(), uboOffset, std::max<uint32_t>(pass.uboSize, 16) };
		if (pass.uboBinding >= 0)
		{
			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = set;
			write.dstBinding = (uint32_t)pass.uboBinding;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			write.pBufferInfo = &buffer;
			writes.push_back(write);
		}
		for (size_t t = 0; t < pass.textures.size(); t++)
		{
			const TexRef& ref = pass.textures[t];
			VkImageView view = input;
			VkImageLayout layout = inputLayout;
			// As the pass that took it first asks: the one after its maker.
			const Pass *taker = &pass;
			bool mips = false;
			switch (ref.kind)
			{
			case TexOriginal:
				taker = &c->passes[0];
				break;
			case TexPassOutput:
				view = c->passes[ref.index].output.texture.GetView();
				layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				taker = &c->passes[std::min<size_t>(ref.index + 1, count - 1)];
				mips = c->passes[ref.index].output.levels > 1;
				break;
			case TexPassFeedback:
			{
				Pass& maker = c->passes[ref.index];
				const Image& image = maker.feedback && maker.previous.texture.IsValid() ? maker.previous : maker.output;
				view = image.texture.GetView();
				layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				taker = &c->passes[std::min<size_t>(ref.index + 1, count - 1)];
				mips = image.levels > 1;
				break;
			}
			case TexLut:
			{
				const Lut& lut = c->luts[ref.index];
				view = lut.texture.GetView();
				layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				images[t] = { samplerFor(*c, lut.linear, false, lut.wrap), view, layout };
				break;
			}
			default:
				break;
			}
			if (ref.kind != TexLut)
				images[t] = { samplerFor(*c, taker->linear, mips, taker->wrap), view, layout };
			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = set;
			write.dstBinding = ref.binding;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = &images[t];
			writes.push_back(write);
		}
		vkUpdateDescriptorSets(device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

		VkClearValue clear{};
		const VkExtent2D extent{ (uint32_t)w[p], (uint32_t)h[p] };
		VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		begin.renderPass = renderPassFor(*c, pass.format);
		begin.framebuffer = pass.output.framebuffer;
		begin.renderArea = { { 0, 0 }, extent };
		begin.clearValueCount = 1;
		begin.pClearValues = &clear;
		vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
		const VkViewport viewport{ 0.f, 0.f, (float)extent.width, (float)extent.height, 0.f, 1.f };
		const VkRect2D scissor{ { 0, 0 }, extent };
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layout, 0, 1, &set, 0, nullptr);
		if (pass.pushSize > 0)
			vkCmdPushConstants(cmd, pass.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
					std::min<uint32_t>(pass.pushSize, sizeof(push)), push);
		const VkBuffer vertexBuffer = c->vertices.GetBuffer();
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &zero);
		vkCmdDraw(cmd, 4, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		pass.output.texture.OverrideImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		makeMipmaps(cmd, pass.output);
		pass.previousValid = true;
	}
	c->uniforms.FlushCPUCache();
	c->frameCount++;
	return c->passes.back().output.texture.GetView();
}

}
