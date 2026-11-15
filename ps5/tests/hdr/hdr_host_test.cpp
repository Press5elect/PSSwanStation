/*
 * PSSwanStation - HDR output's last steps on the PC: OGBox's hdr_host_test.cpp (Press5elect/OGBox
 * ps5/tests/hdr-ogbox, OGBox 0.0.9) without its adjust.frag part, reading PSSwanStation's shaders.
 * Copyright (C) 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect"; see NOTICE.
 *
 * On a software Vulkan driver (lavapipe), as display.cpp's hdrRecord records them: two layers of
 * the swapchain's format (the picture, the menus over clear 0) through hdr.frag into an
 * A2R10G10B10 image, copied into an image of the framebuffers' format (B8G8R8A8, as the console's
 * are) and read back. Each pixel's three 10-bit codes, taken from the 32-bit words where VideoOut's
 * HDR10 type reads them (B in bits 0-9, G 10-19, R 20-29), have to be hdr.frag's maths done here in
 * double precision, within a code: the gamma 2.2 decoding, BT.709 to BT.2020, the white level, the
 * highlights' curve, the menus' blend, PQ.
 *   hdr_host_test <folder with fsr_vertex.spv and hdr.spv>
 */
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s: %d\n", #x, (int)r_); std::exit(2); } } while (0)

static const uint32_t W = 400, H = 300;   // rows 0-5 the test pixels (grey ramp, colours, menus half / fully over,
                                          // ...), the rest black: the test screen's patterns are in the middle

static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t family;

static uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties p;
	vkGetPhysicalDeviceMemoryProperties(gpu, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	std::fprintf(stderr, "no memory type\n");
	std::exit(2);
}

struct Image {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
};

static Image makeImage(VkFormat format, VkImageUsageFlags usage)
{
	Image im{};
	VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = format;
	ci.extent = { W, H, 1 };
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	VKC(vkCreateImage(device, &ci, nullptr, &im.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, im.image, &req);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VKC(vkAllocateMemory(device, &ai, nullptr, &im.memory));
	VKC(vkBindImageMemory(device, im.image, im.memory, 0));
	if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
		VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = im.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = format;
		vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VKC(vkCreateImageView(device, &vi, nullptr, &im.view));
	}
	return im;
}

struct Buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
};

static Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
	Buffer b{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = usage;
	VKC(vkCreateBuffer(device, &ci, nullptr, &b.buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, b.buffer, &req);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKC(vkAllocateMemory(device, &ai, nullptr, &b.memory));
	VKC(vkBindBufferMemory(device, b.buffer, b.memory, 0));
	VKC(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
	return b;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkImageLayout from,
	VkImageLayout to, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
	VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static VkShaderModule shader(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	std::vector<char> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (code.empty()) {
		std::fprintf(stderr, "no %s\n", path.c_str());
		std::exit(2);
	}
	VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = code.size();
	ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
	VkShaderModule m;
	VKC(vkCreateShaderModule(device, &ci, nullptr, &m));
	return m;
}

/* ---- hdr.frag's maths, in double precision --------------------------------------------------- */
static double pqCode(double nits)
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
	return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2) * 1023.0;
}

struct Push {
	float white, peak, srgb, highlights;
	float cal, level, unused0, unused1;   // the test screen: 1 the highlights' pattern, 2 the white level's
};

/* the codes R, G, B hdr.frag gives for a picture pixel p (B, G, R, A bytes as stored) and a menu
 * pixel u (premultiplied) */
static void expect(const uint8_t *p, const uint8_t *u, const Push &k, double out[3])
{
	const double pic[3] = { p[2] / 255.0, p[1] / 255.0, p[0] / 255.0 };
	const double ui[3] = { u[2] / 255.0, u[1] / 255.0, u[0] / 255.0 }, ua = u[3] / 255.0;
	const double white = std::max<double>(k.white, 1.0);
	double gain = 1.0;
	if (k.highlights > 0.5 && k.peak > white) {
		const double l = 0.2126 * std::pow(pic[0], 2.2) + 0.7152 * std::pow(pic[1], 2.2) + 0.0722 * std::pow(pic[2], 2.2);
		if (l > 0.5) {
			const double t = (std::min(l, 1.0) - 0.5) / 0.5;
			gain = (l + (k.peak / white - 1.0) * t * t) / l;
		}
	}
	double light[3];
	for (int i = 0; i < 3; i++) {
		light[i] = std::pow(ui[i] + pic[i] * (1.0 - ua), 2.2) * (gain + (1.0 - gain) * ua);
	}
	const double m[3][3] = { { 0.6274, 0.3293, 0.0433 }, { 0.0691, 0.9195, 0.0114 }, { 0.0164, 0.0880, 0.8956 } };
	for (int i = 0; i < 3; i++) {
		const double v = m[i][0] * light[0] + m[i][1] * light[1] + m[i][2] * light[2];
		out[i] = pqCode(std::max(v, 0.0) * white);
	}
}

int main(int argc, char **argv)
{
	const std::string dir = argc > 1 ? argv[1] : "./";
	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	const char *validation = "VK_LAYER_KHRONOS_validation";
	uint32_t layerCount = 0;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
	std::vector<VkLayerProperties> layers(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
	bool validate = false;
	for (const auto &l : layers) {
		validate = validate || std::strcmp(l.layerName, validation) == 0;
	}
	if (validate) {
		ici.enabledLayerCount = 1;
		ici.ppEnabledLayerNames = &validation;
	}
	std::printf("hdr on the PC: the validation layer %s\n", validate ? "on (its messages, if any, above the results)" : "not installed");
	VKC(vkCreateInstance(&ici, nullptr, &instance));
	uint32_t n = 1;
	VKC(vkEnumeratePhysicalDevices(instance, &n, &gpu) == VK_INCOMPLETE ? VK_SUCCESS : VK_SUCCESS);
	if (n == 0) {
		std::printf("hdr on the PC: NOT RUN (no Vulkan device)\n");
		return 0;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(gpu, &props);
	std::printf("hdr on the PC: %s\n", props.deviceName);
	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = family = 0;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	f13.dynamicRendering = VK_TRUE;
	VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.pNext = &f13;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VKC(vkCreateDevice(gpu, &dci, nullptr, &device));
	vkGetDeviceQueue(device, family, 0, &queue);

	/* the layers: rows of test pixels (B, G, R, A as the swapchain's B8G8R8A8 stores them) */
	std::vector<uint8_t> pic(W * H * 4), ui(W * H * 4, 0);
	for (uint32_t y = 6; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			pic[(y * W + x) * 4 + 3] = 255;
		}
	}
	for (uint32_t x = 0; x < 256; x++) {
		const uint8_t v = static_cast<uint8_t>(x);
		auto set = [&](std::vector<uint8_t> &img, uint32_t y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
			uint8_t *p = &img[(y * W + x) * 4];
			p[0] = b; p[1] = g; p[2] = r; p[3] = a;
		};
		set(pic, 0, v, v, v, 255);                                                  // grey ramp
		const int c = x % 6;                                                        // colours at ramp levels
		set(pic, 1, c == 2 || c == 4 || c == 5 ? v : 0, c == 1 || c == 3 || c == 4 ? v : 0, c == 0 || c == 3 || c == 5 ? v : 0, 255);
		set(pic, 2, v, static_cast<uint8_t>(255 - v), static_cast<uint8_t>((v * 7) & 255), 255);   // mixed
		set(pic, 3, 255, 255, 255, 255);                                            // full white under the menus
		set(ui, 3, static_cast<uint8_t>(v * 200 / 255), static_cast<uint8_t>(v * 200 / 255), static_cast<uint8_t>(v * 200 / 255), v); // menus' cover 0..1
		set(pic, 4, v, v, v, 255);
		set(ui, 4, 0, 0, 0, 128);                                                   // a half-dark panel over the ramp
		set(pic, 5, 0, 0, 0, 255);
		set(ui, 5, v, v, v, 255);                                                   // opaque menus: the white level, no gain
	}
	const VkFormat layerFormat = VK_FORMAT_B8G8R8A8_UNORM, ten = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	Image picture = makeImage(layerFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	Image menus = makeImage(layerFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	Image target = makeImage(ten, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	Image frame = makeImage(layerFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	Buffer upPic = makeBuffer(pic.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT), upUi = makeBuffer(ui.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	Buffer down = makeBuffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	std::memcpy(upPic.mapped, pic.data(), pic.size());
	std::memcpy(upUi.mapped, ui.data(), ui.size());

	VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler sampler;
	VKC(vkCreateSampler(device, &sci, nullptr, &sampler));
	VkDescriptorSetLayoutBinding bindings[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
	};
	VkDescriptorSetLayoutCreateInfo dlci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dlci.bindingCount = 2;
	dlci.pBindings = bindings;
	VkDescriptorSetLayout setLayout;
	VKC(vkCreateDescriptorSetLayout(device, &dlci, nullptr, &setLayout));
	VkPushConstantRange push{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
	VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &push;
	VkPipelineLayout layout;
	VKC(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
	VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
	VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	VkDescriptorPool pool;
	VKC(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsai.descriptorPool = pool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout;
	VkDescriptorSet set;
	VKC(vkAllocateDescriptorSets(device, &dsai, &set));
	VkDescriptorImageInfo infos[2] = { { sampler, picture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ sampler, menus.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet writes[2] = {};
	for (int i = 0; i < 2; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = static_cast<uint32_t>(i);
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(dir + "fsr_vertex.spv"), "main", nullptr },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(dir + "hdr.spv"), "main", nullptr },
	};
	VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	rci.colorAttachmentCount = 1;
	rci.pColorAttachmentFormats = &ten;
	VkGraphicsPipelineCreateInfo gci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gci.pNext = &rci;
	gci.stageCount = 2;
	gci.pStages = stages;
	gci.pVertexInputState = &vin;
	gci.pInputAssemblyState = &ia;
	gci.pViewportState = &vp;
	gci.pRasterizationState = &rs;
	gci.pMultisampleState = &ms;
	gci.pColorBlendState = &cb;
	gci.pDynamicState = &ds;
	gci.layout = layout;
	VkPipeline pipeline;
	VKC(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gci, nullptr, &pipeline));

	VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = family;
	VkCommandPool cmdPool;
	VKC(vkCreateCommandPool(device, &cpci, nullptr, &cmdPool));
	VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cbai.commandPool = cmdPool;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	VKC(vkAllocateCommandBuffers(device, &cbai, &cmd));

	const struct {
		const char *name;
		Push k;
	} cases[] = {
		{ "white 203, highlights 1000", { 203.0f, 1000.0f, 0.0f, 1.0f, 0, 0, 0, 0 } },
		{ "white 203, highlights off", { 203.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0 } },
		{ "white 300, highlights 2000", { 300.0f, 2000.0f, 0.0f, 1.0f, 0, 0, 0, 0 } },
		{ "white 203, no game picture (the menus)", { 203.0f, 1000.0f, 0.0f, 0.0f, 0, 0, 0, 0 } },
		{ "test screen: highlights at 600 nits", { 203.0f, 1000.0f, 0.0f, 0.0f, 1, 600, 0, 0 } },
		{ "test screen: white level 250 nits", { 250.0f, 1000.0f, 0.0f, 0.0f, 2, 250, 0, 0 } },
	};
	int failed = 0;
	for (const auto &c : cases) {
		VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VKC(vkResetCommandBuffer(cmd, 0));
		VKC(vkBeginCommandBuffer(cmd, &bi));
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { W, H, 1 };
		for (const auto &up : { std::make_pair(upPic.buffer, picture.image), std::make_pair(upUi.buffer, menus.image) }) {
			barrier(cmd, up.second, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			vkCmdCopyBufferToImage(cmd, up.first, up.second, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			barrier(cmd, up.second, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}
		/* resolveHdr's steps, as ogbox_main.cpp records them */
		barrier(cmd, target.image, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		att.imageView = target.view;
		att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = { { 0, 0 }, { W, H } };
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &att;
		vkCmdBeginRendering(cmd, &ri);
		VkViewport viewport{ 0, 0, float(W), float(H), 0, 1 };
		VkRect2D scissor{ { 0, 0 }, { W, H } };
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &c.k);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);
		barrier(cmd, target.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		barrier(cmd, frame.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkImageCopy copy{};
		copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.extent = { W, H, 1 };
		vkCmdCopyImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		/* (the console presents the framebuffer here; the test reads it back) */
		barrier(cmd, frame.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyImageToBuffer(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.buffer, 1, &region);
		VKC(vkEndCommandBuffer(cmd));
		VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
		VKC(vkQueueWaitIdle(queue));

		const uint32_t *words = static_cast<const uint32_t *>(down.mapped);
		double worst = 0.0;
		int bad = 0, at = -1;
		const uint32_t rows = c.k.cal > 0.5f ? 6 : H;   // the test screen: the rows outside its pattern
		for (uint32_t y = 0; y < rows; y++) {
			for (uint32_t x = 0; x < W; x++) {
				const uint32_t w = words[y * W + x];
				const double got[3] = { double((w >> 20) & 0x3FF), double((w >> 10) & 0x3FF), double(w & 0x3FF) };
				double want[3];
				expect(&pic[(y * W + x) * 4], &ui[(y * W + x) * 4], c.k, want);
				for (int i = 0; i < 3; i++) {
					const double d = std::fabs(got[i] - want[i]);
					if (d > worst) {
						worst = d;
					}
					if (d > 1.0) {
						bad++;
						if (at < 0) {
							at = static_cast<int>(y * W + x);
						}
					}
				}
			}
		}
		/* landmarks: full white with and without the highlights, the menus' white, black */
		const uint32_t wWhite = words[0 * W + 255], wBlack = words[0 * W + 0], wMenu = words[5 * W + 255];
		std::printf("  %-40s worst %.2f codes, %d off by more than one%s; white R=%u (PQ %.0f nits), menus' white %u, black %u\n", c.name,
			worst, bad, bad ? " FAIL" : "", (wWhite >> 20) & 0x3FF,
			10000.0 * std::pow(std::max(std::pow(((wWhite >> 20) & 0x3FF) / 1023.0, 1.0 / 78.84375) - 0.8359375, 0.0) /
				(18.8515625 - 18.6875 * std::pow(((wWhite >> 20) & 0x3FF) / 1023.0, 1.0 / 78.84375)), 1.0 / 0.1593017578125),
			(wMenu >> 20) & 0x3FF, (wBlack >> 20) & 0x3FF);
		if (bad) {
			const uint32_t w = words[at];
			double want[3];
			expect(&pic[at * 4], &ui[at * 4], c.k, want);
			std::printf("    first at row %d x %d: got %u %u %u, want %.1f %.1f %.1f\n", at / int(W), at % int(W), (w >> 20) & 0x3FF,
				(w >> 10) & 0x3FF, w & 0x3FF, want[0], want[1], want[2]);
		}
		/* the test screen's pattern: code by code at its landmarks */
		auto code = [&](uint32_t x, uint32_t y) { return double((words[y * W + x] >> 20) & 0x3FF); };
		auto landmark = [&](const char *what, uint32_t x, uint32_t y, double nits) {
			const double got = code(x, y), want = pqCode(nits);
			const bool ok = std::fabs(got - want) <= 1.0 && code(x, y) == double((words[y * W + x] >> 10) & 0x3FF) &&
				code(x, y) == double(words[y * W + x] & 0x3FF);
			std::printf("    %-46s %4.0f, want %6.1f (%g nits)%s\n", what, got, want, nits, ok ? "" : " FAIL");
			bad += !ok;
		};
		if (c.k.cal > 0.5f && c.k.cal < 1.5f) {
			const float side = 0.40f * H;
			landmark("the X in the middle", W / 2, H / 2, c.k.level);
			landmark("the square beside it", W / 2 + static_cast<uint32_t>(0.4f * side), H / 2, 10000.0);
			landmark("outside the square", W / 2 + static_cast<uint32_t>(0.6f * side), H / 2, 0.0);
		} else if (c.k.cal > 1.5f) {
			const float cw = 0.64f * W, ch = 0.40f * H, x0 = (W - cw) * 0.5f, y0 = (H - ch) * 0.5f;
			for (int k : { 0, 2, 5, 10 }) {
				char what[64];
				std::snprintf(what, sizeof(what), "grey step %d of 10", k);
				landmark(what, static_cast<uint32_t>(x0 + (k + 0.5f) / 11.0f * cw), static_cast<uint32_t>(y0 + 0.2f * ch),
					std::pow(k / 10.0, 2.2) * c.k.level);
			}
			landmark("the white patch", W / 2, static_cast<uint32_t>(y0 + 0.7f * ch), c.k.level);
		}
		failed += bad != 0;
	}
	std::printf("hdr on the PC: %s\n", failed ? "FAILED" : "PASS (every pixel within a code of hdr.frag's maths in double precision)");
	return failed ? 1 : 0;
}
