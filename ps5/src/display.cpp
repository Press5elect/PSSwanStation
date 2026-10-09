/*
	PSSwanStation - the display: Vulkan on RADV, the swapchain, Dear ImGui.

	SPDX-License-Identifier: GPL-3.0-or-later

	The console has no Vulkan loader and no window system. RADV (PS5_Vulkan /
	PS5_Mesa) is linked into the title; its ICD entry point resolves every
	command (dep/vulkan-loader), and the picture goes to a VK_KHR_display plane
	surface on the console's one display, as PS5_Vulkan's own titles and
	PSFlyCast drive it: the display's 4K mode with the highest refresh rate the
	driver offers, a FIFO swapchain, B8G8R8A8. The rate is 59.94 Hz unless
	sce_sys/param.json declares the 120 Hz output (the "Display output"
	setting writes that, storage.cpp) and the display takes it: the driver
	then runs the output at 119.88 Hz for the whole run and offers that mode
	first. Should the output be at 119.88 Hz while the setting asks for 60 (the
	file was changed by hand), each picture is shown for two refreshes.

	The device is the emulator's own Vulkan context (src/common/vulkan/context),
	made here once and kept for the whole run: the interface draws with it, and
	the emulator's hardware renderer finds it there when a game starts
	(host.cpp answers the core's libretro Vulkan interface from it). A frame is
	the emulator's commands, submitted by the emulator, then one render pass of
	ImGui into the swapchain image: the game's picture is an ImGui image under
	the menus.

	Besides the screen there are two small pictures the title draws into
	itself, with the same ImGui pipeline: the game's picture alone, read back
	for the pictures kept with save states (capture), and
	a few pixels of it blended over time, which stretched over the screen is
	the light a 4:3 picture throws on the bars beside it (ambient).

	With "Scaling" on FSR 1 the game's picture is not stretched by ImGui's
	bilinear filter but by AMD's FidelityFX Super Resolution 1.0 (upscale): two
	draws of one triangle with pipelines of their own, EASU into a picture the
	size the game has on the screen and RCAS from that into a second, which
	ImGui then draws pixel for pixel. The shaders are AMD's headers
	(ps5/third_party/fsr, MIT) compiled ahead of time (fsr_spirv.inc). Should
	any of it fail, that is a line in the log and the usual stretch.

	Frame generation (generated) draws pictures between each two of the
	game's. The picture is kept (two of them, the one before and this one); a
	small picture of how bright each part is, and three smaller ones of that,
	are made of each; how every part moved from the one frame to the other is
	looked for from the smallest of those to the largest (fg_search.frag) and
	put in order (fg_tidy.frag); for the moment being drawn, each place of the
	picture then keeps the movement under which the two frames agree
	(fg_choose.frag); and the picture in between takes each pixel from the
	nearer of the two frames, moved (fg_blend.frag). Which moment is drawn is
	host.cpp's business: it counts the pictures the game really draws. The
	shaders are this title's own (ps5/shaders/fg_*, compiled into
	fg_spirv.inc).

	On a PC (the host build) the surface is VK_EXT_headless_surface and frames
	can be saved as PNGs: that is how the interface is checked without a console.
*/
#include "display.h"
#include "chain.h"
#include "slangimport.h"
#include "fe.h"

#include "common/vulkan/context.h"
#include "common/vulkan/staging_buffer.h"
#include "common/vulkan/staging_texture.h"
#include "common/vulkan/texture.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace Vulkan
{
bool LoadVulkanLibrary();
void UnloadVulkanLibrary();
bool LoadVulkanInstanceFunctions(VkInstance instance);
}

namespace fe::display
{

struct Texture
{
	Vulkan::Texture texture;
	Vulkan::StagingTexture staging;
	VkDescriptorSet set = VK_NULL_HANDLE;
	int width = 0, height = 0;
	bool dynamic = false;
};

namespace
{

VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice gpu = VK_NULL_HANDLE;
VkSurfaceKHR surface = VK_NULL_HANDLE;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;
VkFormat swapFormat = VK_FORMAT_UNDEFINED;
VkExtent2D extent{};
float refresh = 60.f;
bool canReadBack = false;
VkRenderPass renderPass = VK_NULL_HANDLE;
std::vector<VkImage> images;
std::vector<VkImageView> views;
std::vector<VkFramebuffer> framebuffers;
std::vector<VkSemaphore> acquireSemaphores, renderSemaphores;
size_t semaphoreIndex = 0;
uint32_t imageIndex = 0;
bool frameOpen = false;
uint64_t frames = 0;
std::string gpuName;
// What the output really refreshes at, when that is not the mode's rate.
float outputRate = 0;

// The emulator's picture, wrapped for ImGui.
VkImageView wrappedView = VK_NULL_HANDLE;
VkDescriptorSet wrappedSet = VK_NULL_HANDLE;
int wrappedLayout = 0;
// The software renderer's pictures, by what ImGui knows them as: FSR reads
// the image itself.
std::map<void *, VkImageView> dynamicViews;

// A picture the title draws into: the capture's and the ambient light's.
struct Target
{
	Vulkan::Texture texture;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;		// for ImGui; none for a picture only shaders read
	int width = 0, height = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	bool drawn = false;
};
Target captureTarget, ambientTarget;
// The swapchain's render pass again, for those: one that clears first and one
// that draws over what is there; both leave the picture readable by a shader.
VkRenderPass targetClearPass = VK_NULL_HANDLE, targetBlendPass = VK_NULL_HANDLE;
// The passes of the pictures the title's own shaders draw: one of the
// swapchain's format, and one for pictures of numbers (how the game's picture
// moved) in 16-bit floats. Both clear first and leave the picture readable.
VkRenderPass shaderPass = VK_NULL_HANDLE, floatPass = VK_NULL_HANDLE;
constexpr VkFormat FloatFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
void fsrShutdown();
void generationShutdown();
void lookShutdown();
// Descriptor sets ImGui must not lose before the frames that used them are
// drawn: freed two frames later.
struct Retired
{
	VkDescriptorSet set;
	uint64_t frame;
};
std::vector<Retired> retired;

void retire(VkDescriptorSet set)
{
	if (set != VK_NULL_HANDLE)
		retired.push_back({ set, frames });
}

void freeRetired(bool all)
{
	for (size_t i = 0; i < retired.size();)
	{
		if (all || frames > retired[i].frame + 3)
		{
			ImGui_ImplVulkan_RemoveTexture(retired[i].set);
			retired.erase(retired.begin() + i);
		}
		else
			i++;
	}
}

bool fail(const char *what, VkResult result)
{
	diag::mark("vulkan: %s failed: %d", what, (int)result);
	return false;
}

bool createInstance()
{
	if (!Vulkan::LoadVulkanLibrary())
	{
		diag::mark("vulkan: no driver entry points");
		return false;
	}
	const char *extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(SWANSTATION_PS5)
		VK_KHR_DISPLAY_EXTENSION_NAME,
#else
		"VK_EXT_headless_surface",
#endif
	};
	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = AppName;
	app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app.pEngineName = AppName;
	app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	// 1.3 (the driver offers 1.4 on the console): room for compute work that
	// needs more than 1.0's features.
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	info.pApplicationInfo = &app;
	info.enabledExtensionCount = (uint32_t)(sizeof(extensions) / sizeof(extensions[0]));
	info.ppEnabledExtensionNames = extensions;
	diag::mark("vulkan: creating the instance");
	const VkResult result = vkCreateInstance(&info, nullptr, &instance);
	if (result != VK_SUCCESS)
		return fail("vkCreateInstance", result);
	if (!Vulkan::LoadVulkanInstanceFunctions(instance))
	{
		diag::mark("vulkan: instance functions are missing");
		return false;
	}
	return true;
}

bool pickGpu()
{
	const Vulkan::Context::GPUList gpus = Vulkan::Context::EnumerateGPUs(instance);
	if (gpus.empty())
	{
		diag::mark("vulkan: no device");
		return false;
	}
	gpu = gpus[0];
#if !defined(SWANSTATION_PS5)
	// A PC may list several: SWANSTATION_GPU picks one by a part of its name
	// (the test runs name the software driver).
	if (const char *wanted = getenv("SWANSTATION_GPU"))
		for (VkPhysicalDevice candidate : gpus)
		{
			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(candidate, &properties);
			if (strstr(properties.deviceName, wanted) != nullptr)
				gpu = candidate;
		}
#endif
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(gpu, &properties);
	gpuName = properties.deviceName;
	diag::mark("vulkan: %s, Vulkan %u.%u.%u", properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
			VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
	return true;
}

#if defined(SWANSTATION_PS5)
}
// The graphics driver's own (PS5_Mesa, wsi_common_videoout.c): each picture is
// shown for rate + 1 refreshes.
extern "C" int wsi_videoout_set_flip_rate(int rate);
namespace
{
// The console's one display, as a plane surface: the mode with the most
// pixels, then the highest refresh rate (PSFlyCast's choice).
bool createSurface()
{
	uint32_t count = 0;
	vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &count, nullptr);
	if (count == 0)
	{
		diag::mark("vulkan: no display");
		return false;
	}
	std::vector<VkDisplayPropertiesKHR> displays(count);
	vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &count, displays.data());
	uint32_t modeCount = 0;
	vkGetDisplayModePropertiesKHR(gpu, displays[0].display, &modeCount, nullptr);
	if (modeCount == 0)
	{
		diag::mark("vulkan: the display has no mode");
		return false;
	}
	std::vector<VkDisplayModePropertiesKHR> modes(modeCount);
	vkGetDisplayModePropertiesKHR(gpu, displays[0].display, &modeCount, modes.data());
	// The largest size, and of its modes the fastest: when the driver offers
	// 119.88 Hz the output is running at it, whichever mode is taken.
	auto score = [](const VkDisplayModePropertiesKHR& mode) {
		const auto& p = mode.parameters;
		return (long long)p.visibleRegion.width * p.visibleRegion.height * 1000000 + (long long)p.refreshRate;
	};
	const VkDisplayModePropertiesKHR *best = &modes[0];
	for (const VkDisplayModePropertiesKHR& mode : modes)
	{
		diag::mark("display: mode %u x %u at %.2f Hz", mode.parameters.visibleRegion.width,
				mode.parameters.visibleRegion.height, mode.parameters.refreshRate / 1000.0);
		if (score(mode) > score(*best))
			best = &mode;
	}
	extent = best->parameters.visibleRegion;
	refresh = best->parameters.refreshRate / 1000.f;
	outputRate = refresh;
	if (refresh > 100.f && options::frontend().displayMode == 0)
	{
		// 119.88 Hz that was not asked for: each picture stays for two refreshes.
		const int result = wsi_videoout_set_flip_rate(1);
		diag::mark("display: the output is at %.2f Hz and 60 is set: two refreshes a picture (%d)", refresh, result);
		if (result >= 0)
			refresh *= 0.5f;
	}
	VkDisplaySurfaceCreateInfoKHR info{ VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR };
	info.displayMode = best->displayMode;
	info.planeIndex = 0;
	info.planeStackIndex = 0;
	info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	info.globalAlpha = 1.0f;
	info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	info.imageExtent = extent;
	const VkResult result = vkCreateDisplayPlaneSurfaceKHR(instance, &info, nullptr, &surface);
	if (result != VK_SUCCESS)
		return fail("vkCreateDisplayPlaneSurfaceKHR", result);
	diag::mark("display: %u x %u at %.2f Hz (%u mode%s offered)", extent.width, extent.height, refresh, modeCount,
			modeCount == 1 ? "" : "s");
	return true;
}
#else
// A PC: a surface with no window, at the size the test asks for.
struct HeadlessSurfaceCreateInfo
{
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
};
bool createSurface()
{
	using Create = VkResult (VKAPI_PTR *)(VkInstance, const HeadlessSurfaceCreateInfo *, const VkAllocationCallbacks *,
			VkSurfaceKHR *);
	const Create create = reinterpret_cast<Create>(vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
	if (create == nullptr)
	{
		diag::mark("vulkan: no headless surface");
		return false;
	}
	HeadlessSurfaceCreateInfo info{ (VkStructureType)1000256000, nullptr, 0 };
	const VkResult result = create(instance, &info, nullptr, &surface);
	if (result != VK_SUCCESS)
		return fail("vkCreateHeadlessSurfaceEXT", result);
	extent.width = 1920;
	extent.height = 1080;
	if (const char *size = getenv("SWANSTATION_SIZE"))
	{
		unsigned w = 0, h = 0;
		if (sscanf(size, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360)
		{
			extent.width = w;
			extent.height = h;
		}
	}
	refresh = 59.94f;
	// A test can stand in for a 120 Hz display.
	if (const char *rate = getenv("SWANSTATION_REFRESH"))
		refresh = std::clamp((float)atof(rate), 24.f, 240.f);
	outputRate = refresh;
	return true;
}
#endif

bool createSwapchain()
{
	VkDevice device = g_vulkan_context->GetDevice();
	VkSurfaceCapabilitiesKHR caps{};
	VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
	if (result != VK_SUCCESS)
		return fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
	if (caps.currentExtent.width != 0xffffffffu)
		extent = caps.currentExtent;
	extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
	extent.height = std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data());
	if (formats.empty())
	{
		diag::mark("vulkan: the surface has no format");
		return false;
	}
	VkSurfaceFormatKHR chosen = formats[0];
	for (const VkSurfaceFormatKHR& format : formats)
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM)
		{
			chosen = format;
			break;
		}
	for (const VkSurfaceFormatKHR& format : formats)
		if (chosen.format != VK_FORMAT_B8G8R8A8_UNORM && format.format == VK_FORMAT_R8G8B8A8_UNORM)
			chosen = format;
	swapFormat = chosen.format;

	uint32_t imageCount = std::max<uint32_t>(3, caps.minImageCount);
	if (caps.maxImageCount != 0)
		imageCount = std::min(imageCount, caps.maxImageCount);
	canReadBack = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

	VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = surface;
	info.minImageCount = imageCount;
	info.imageFormat = chosen.format;
	info.imageColorSpace = chosen.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (canReadBack ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
			? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
	info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
			? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.clipped = VK_TRUE;
	result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
	if (result != VK_SUCCESS)
		return fail("vkCreateSwapchainKHR", result);

	uint32_t count = 0;
	vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
	images.resize(count);
	vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

	VkAttachmentDescription attachment{};
	attachment.format = swapFormat;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	passInfo.attachmentCount = 1;
	passInfo.pAttachments = &attachment;
	passInfo.subpassCount = 1;
	passInfo.pSubpasses = &subpass;
	passInfo.dependencyCount = 1;
	passInfo.pDependencies = &dependency;
	result = vkCreateRenderPass(device, &passInfo, nullptr, &renderPass);
	if (result != VK_SUCCESS)
		return fail("vkCreateRenderPass", result);
	// The same pass for the title's own pictures (ImGui's pipeline fits any
	// pass that differs from the swapchain's in no more than this).
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	result = vkCreateRenderPass(device, &passInfo, nullptr, &targetClearPass);
	if (result != VK_SUCCESS)
		return fail("vkCreateRenderPass", result);
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	result = vkCreateRenderPass(device, &passInfo, nullptr, &targetBlendPass);
	if (result != VK_SUCCESS)
		return fail("vkCreateRenderPass", result);
	// And the passes of the pictures the title's own shaders draw (FSR, frame
	// generation): such a picture was read by a shader before it is drawn
	// into again, and is read by one straight after, which these say.
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkSubpassDependency around[2] = {};
	around[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	around[0].dstSubpass = 0;
	around[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	around[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	around[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	around[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	around[1].srcSubpass = 0;
	around[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	around[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	around[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	around[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	around[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	passInfo.dependencyCount = 2;
	passInfo.pDependencies = around;
	result = vkCreateRenderPass(device, &passInfo, nullptr, &shaderPass);
	if (result != VK_SUCCESS)
		return fail("vkCreateRenderPass", result);
	attachment.format = FloatFormat;
	result = vkCreateRenderPass(device, &passInfo, nullptr, &floatPass);
	if (result != VK_SUCCESS)
		return fail("vkCreateRenderPass", result);

	views.resize(count);
	framebuffers.resize(count);
	for (uint32_t i = 0; i < count; i++)
	{
		VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = images[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = swapFormat;
		viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		result = vkCreateImageView(device, &viewInfo, nullptr, &views[i]);
		if (result != VK_SUCCESS)
			return fail("vkCreateImageView", result);
		VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fbInfo.renderPass = renderPass;
		fbInfo.attachmentCount = 1;
		fbInfo.pAttachments = &views[i];
		fbInfo.width = extent.width;
		fbInfo.height = extent.height;
		fbInfo.layers = 1;
		result = vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]);
		if (result != VK_SUCCESS)
			return fail("vkCreateFramebuffer", result);
	}
	acquireSemaphores.resize(count + 1);
	renderSemaphores.resize(count + 1);
	for (uint32_t i = 0; i < count + 1; i++)
	{
		VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		vkCreateSemaphore(device, &semInfo, nullptr, &acquireSemaphores[i]);
		vkCreateSemaphore(device, &semInfo, nullptr, &renderSemaphores[i]);
	}
	diag::mark("vulkan: swapchain %u x %u, %u images, format %d", extent.width, extent.height, count, (int)swapFormat);
	return true;
}

void checkResult(VkResult result)
{
	if (result != VK_SUCCESS)
		diag::mark("imgui: Vulkan error %d", (int)result);
}

bool initImGui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	io.DisplaySize = ImVec2((float)extent.width, (float)extent.height);

	if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0,
			[](const char *name, void *user) { return vkGetInstanceProcAddr(static_cast<VkInstance>(user), name); },
			instance))
	{
		diag::mark("imgui: Vulkan functions are missing");
		return false;
	}
	ImGui_ImplVulkan_InitInfo info{};
	info.ApiVersion = VK_API_VERSION_1_0;
	info.Instance = instance;
	info.PhysicalDevice = gpu;
	info.Device = g_vulkan_context->GetDevice();
	info.QueueFamily = g_vulkan_context->GetGraphicsQueueFamilyIndex();
	info.Queue = g_vulkan_context->GetGraphicsQueue();
	info.PipelineInfoMain.RenderPass = renderPass;
	info.MinImageCount = (uint32_t)images.size();
	// How many sets of vertex buffers the backend goes round: a frame draws
	// up to three times (the screen, the ambient light, a capture), and a set
	// must not come round again while the graphics processor may still read it.
	info.ImageCount = (uint32_t)images.size() * 3 + 1;
	info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	// Covers are one descriptor set each.
	info.DescriptorPoolSize = 4096;
	info.CheckVkResultFn = checkResult;
	if (!ImGui_ImplVulkan_Init(&info))
	{
		diag::mark("imgui: the Vulkan backend did not start");
		return false;
	}
	return true;
}

} // namespace

bool init()
{
	if (!createInstance() || !pickGpu() || !createSurface())
		return false;
	diag::mark("vulkan: device");
	if (!Vulkan::Context::CreateFromExistingInstance(instance, gpu, surface, false, false, false))
	{
		diag::mark("vulkan: the device could not be made");
		return false;
	}
	if (!createSwapchain())
		return false;
	if (!initImGui())
		return false;
	diag::mark("vulkan: ready");
	return true;
}

void shutdown()
{
	if (!g_vulkan_context)
		return;
	VkDevice device = g_vulkan_context->GetDevice();
	g_vulkan_context->WaitForGPUIdle();
	releaseWrapped();
	lookShutdown();
	generationShutdown();
	fsrShutdown();
	for (Target *target : { &captureTarget, &ambientTarget })
	{
		retire(target->set);
		if (target->framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(device, target->framebuffer, nullptr);
		target->texture.Destroy(false);
		*target = Target();
	}
	freeRetired(true);
	ImGui_ImplVulkan_Shutdown();
	ImGui::DestroyContext();
	for (VkSemaphore semaphore : acquireSemaphores)
		vkDestroySemaphore(device, semaphore, nullptr);
	for (VkSemaphore semaphore : renderSemaphores)
		vkDestroySemaphore(device, semaphore, nullptr);
	for (VkFramebuffer framebuffer : framebuffers)
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	for (VkImageView view : views)
		vkDestroyImageView(device, view, nullptr);
	vkDestroyRenderPass(device, renderPass, nullptr);
	vkDestroyRenderPass(device, targetClearPass, nullptr);
	vkDestroyRenderPass(device, shaderPass, nullptr);
	vkDestroyRenderPass(device, floatPass, nullptr);
	vkDestroyRenderPass(device, targetBlendPass, nullptr);
	vkDestroySwapchainKHR(device, swapchain, nullptr);
	Vulkan::Context::Destroy();
	vkDestroyDevice(device, nullptr);
	vkDestroySurfaceKHR(instance, surface, nullptr);
	vkDestroyInstance(instance, nullptr);
	instance = VK_NULL_HANDLE;
}

int width()
{
	return (int)extent.width;
}

int height()
{
	return (int)extent.height;
}

float refreshRate()
{
	return refresh;
}

float outputRefreshRate()
{
	return outputRate;
}

float scale()
{
	return (float)extent.height / 1080.f;
}

std::string deviceName()
{
	return gpuName;
}

uint64_t frameCount()
{
	return frames;
}

void *vkInstance()
{
	return instance;
}

bool beginFrame()
{
	VkDevice device = g_vulkan_context->GetDevice();
	semaphoreIndex = (semaphoreIndex + 1) % acquireSemaphores.size();
	const VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquireSemaphores[semaphoreIndex],
			VK_NULL_HANDLE, &imageIndex);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		static int said;
		if (said++ < 5)
			diag::mark("vulkan: no swapchain image: %d", (int)result);
		return false;
	}
	frameOpen = true;
	freeRetired(false);
	ImGui_ImplVulkan_NewFrame();
	ImGui::GetIO().DisplaySize = ImVec2((float)extent.width, (float)extent.height);
	ImGui::NewFrame();
	return true;
}

void endFrame()
{
	if (!frameOpen)
		return;
	frameOpen = false;
	ImGui::Render();
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	VkClearValue clear{};
	clear.color = { { 0.f, 0.f, 0.f, 1.f } };
	VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	begin.renderPass = renderPass;
	begin.framebuffer = framebuffers[imageIndex];
	begin.renderArea = { { 0, 0 }, extent };
	begin.clearValueCount = 1;
	begin.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkCmdEndRenderPass(cmd);

	g_vulkan_context->SubmitCommandBuffer(acquireSemaphores[semaphoreIndex], renderSemaphores[semaphoreIndex]);
	VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &renderSemaphores[semaphoreIndex];
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain;
	present.pImageIndices = &imageIndex;
	const VkResult result = vkQueuePresentKHR(g_vulkan_context->GetPresentQueue(), &present);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		static int said;
		if (said++ < 5)
			diag::mark("vulkan: present failed: %d", (int)result);
	}
	g_vulkan_context->MoveToNextCommandBuffer();
	if (frames < 3)
		diag::mark("vulkan: present %d", (int)frames + 1);
	frames++;
}

// ------------------------------------------------------------------ textures

Texture *createTexture(int w, int h, const uint8_t *rgba)
{
	if (w <= 0 || h <= 0 || rgba == nullptr)
		return nullptr;
	Texture *texture = new Texture();
	texture->width = w;
	texture->height = h;
	if (!texture->texture.Create(w, h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
			VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			|| !texture->staging.Create(Vulkan::StagingBuffer::Type::Upload, VK_FORMAT_R8G8B8A8_UNORM, w, h))
	{
		delete texture;
		return nullptr;
	}
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	texture->texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	texture->staging.WriteTexels(0, 0, w, h, rgba, w * 4);
	texture->staging.CopyToTexture(cmd, 0, 0, texture->texture, 0, 0, 0, 0, w, h);
	texture->texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	texture->staging.Destroy(true);
	texture->set = ImGui_ImplVulkan_AddTexture(texture->texture.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
#if defined(SWANSTATION_HOST)
	// The frame generation test (ui.cpp) passes pictures from files as the game's.
	dynamicViews[(void *)texture->set] = texture->texture.GetView();
#endif
	return texture;
}

Texture *createDynamicTexture(int w, int h)
{
	if (w <= 0 || h <= 0)
		return nullptr;
	Texture *texture = new Texture();
	texture->width = w;
	texture->height = h;
	texture->dynamic = true;
	if (!texture->texture.Create(w, h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
			VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			|| !texture->staging.Create(Vulkan::StagingBuffer::Type::Upload, VK_FORMAT_R8G8B8A8_UNORM, w, h))
	{
		delete texture;
		return nullptr;
	}
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	texture->texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	static const VkClearColorValue black = { { 0.f, 0.f, 0.f, 1.f } };
	static const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(cmd, texture->texture.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	texture->texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	texture->set = ImGui_ImplVulkan_AddTexture(texture->texture.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	dynamicViews[(void *)texture->set] = texture->texture.GetView();
	return texture;
}

void updateTexture(Texture *texture, const void *pixels, int w, int h, size_t pitch, PixelFormat format)
{
	if (texture == nullptr || pixels == nullptr || w > texture->width || h > texture->height)
		return;
	uint8_t *to = reinterpret_cast<uint8_t *>(texture->staging.GetMappedPointer());
	const size_t stride = texture->staging.GetMappedStride();
	const uint8_t *from = static_cast<const uint8_t *>(pixels);
	for (int y = 0; y < h; y++)
	{
		uint8_t *out = to + (size_t)y * stride;
		const uint8_t *in = from + (size_t)y * pitch;
		switch (format)
		{
		case Xrgb8888:
			for (int x = 0; x < w; x++)
			{
				out[x * 4 + 0] = in[x * 4 + 2];
				out[x * 4 + 1] = in[x * 4 + 1];
				out[x * 4 + 2] = in[x * 4 + 0];
				out[x * 4 + 3] = 255;
			}
			break;
		case Rgb565:
			for (int x = 0; x < w; x++)
			{
				uint16_t p;
				memcpy(&p, in + x * 2, 2);
				const unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
				out[x * 4 + 0] = (uint8_t)((r << 3) | (r >> 2));
				out[x * 4 + 1] = (uint8_t)((g << 2) | (g >> 4));
				out[x * 4 + 2] = (uint8_t)((b << 3) | (b >> 2));
				out[x * 4 + 3] = 255;
			}
			break;
		case Xrgb1555:
			for (int x = 0; x < w; x++)
			{
				uint16_t p;
				memcpy(&p, in + x * 2, 2);
				const unsigned r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
				out[x * 4 + 0] = (uint8_t)((r << 3) | (r >> 2));
				out[x * 4 + 1] = (uint8_t)((g << 3) | (g >> 2));
				out[x * 4 + 2] = (uint8_t)((b << 3) | (b >> 2));
				out[x * 4 + 3] = 255;
			}
			break;
		}
	}
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	texture->texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	texture->staging.CopyToTexture(cmd, 0, 0, texture->texture, 0, 0, 0, 0, w, h);
	texture->texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void destroyTexture(Texture *texture)
{
	if (texture == nullptr)
		return;
	dynamicViews.erase((void *)texture->set);
	retire(texture->set);
	texture->staging.Destroy(true);
	texture->texture.Destroy(true);
	delete texture;
}

void *textureId(const Texture *texture)
{
	return texture != nullptr ? (void *)texture->set : nullptr;
}

int textureWidth(const Texture *texture)
{
	return texture != nullptr ? texture->width : 0;
}

int textureHeight(const Texture *texture)
{
	return texture != nullptr ? texture->height : 0;
}

void *wrapView(void *imageView, int layout)
{
	const VkImageView view = static_cast<VkImageView>(imageView);
	if (view == VK_NULL_HANDLE)
		return nullptr;
	if (view != wrappedView || wrappedLayout != layout)
	{
		retire(wrappedSet);
		wrappedSet = ImGui_ImplVulkan_AddTexture(view, (VkImageLayout)layout);
		wrappedView = view;
		wrappedLayout = layout;
	}
	return (void *)wrappedSet;
}

void releaseWrapped()
{
	retire(wrappedSet);
	wrappedSet = VK_NULL_HANDLE;
	wrappedView = VK_NULL_HANDLE;
}

void sampling(void *drawList, bool nearest)
{
	// The backend's two samplers, chosen by a command in the draw list.
	const ImGuiPlatformIO& io = ImGui::GetPlatformIO();
	const ImDrawCallback callback = nearest ? io.DrawCallback_SetSamplerNearest : io.DrawCallback_SetSamplerLinear;
	if (callback != nullptr)
		static_cast<ImDrawList *>(drawList)->AddCallback(callback, nullptr);
}

namespace
{
// `format` and `pass`: the swapchain's and the pass for the title's own
// pictures, or 16-bit floats and theirs. `forImGui`: ImGui is to draw it.
bool ensureTarget(Target& target, int width, int height, VkFormat format = VK_FORMAT_UNDEFINED,
		VkRenderPass pass = VK_NULL_HANDLE, bool forImGui = true)
{
	if (format == VK_FORMAT_UNDEFINED)
		format = swapFormat;
	if (pass == VK_NULL_HANDLE)
		pass = targetClearPass;
	if (target.framebuffer != VK_NULL_HANDLE && target.width == width && target.height == height && target.format == format)
		return true;
	VkDevice device = g_vulkan_context->GetDevice();
	if (target.framebuffer != VK_NULL_HANDLE)
	{
		// What may still be in a frame under way is let go of later.
		retire(target.set);
		g_vulkan_context->DeferFramebufferDestruction(target.framebuffer);
		target.texture.Destroy(true);
		target = Target();
	}
	if (!target.texture.Create((uint32_t)width, (uint32_t)height, 1, 1, format, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
		return false;
	const VkImageView view = target.texture.GetView();
	VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	info.renderPass = pass;
	info.attachmentCount = 1;
	info.pAttachments = &view;
	info.width = (uint32_t)width;
	info.height = (uint32_t)height;
	info.layers = 1;
	if (vkCreateFramebuffer(device, &info, nullptr, &target.framebuffer) != VK_SUCCESS)
	{
		target.texture.Destroy(false);
		target = Target();
		return false;
	}
	if (forImGui)
		target.set = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	target.width = width;
	target.height = height;
	target.format = format;
	target.drawn = false;
	return true;
}

void destroyTarget(Target& target)
{
	retire(target.set);
	if (target.framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(g_vulkan_context->GetDevice(), target.framebuffer, nullptr);
	target.texture.Destroy(false);
	target = Target();
}

// Draws `texture` (its part up to u, v) over the whole of a target, with
// ImGui's pipeline: `alpha` below 1 blends it over what the target holds.
// `shiftU`, `shiftV` move where it is sampled, in parts of the texture.
void drawInto(Target& target, void *texture, float u, float v, float alpha, float shiftU, float shiftV)
{
	const bool blend = alpha < 1.f && target.drawn;
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	VkClearValue clear{};
	clear.color = { { 0.f, 0.f, 0.f, 1.f } };
	VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	begin.renderPass = blend ? targetBlendPass : targetClearPass;
	begin.framebuffer = target.framebuffer;
	begin.renderArea = { { 0, 0 }, { (uint32_t)target.width, (uint32_t)target.height } };
	begin.clearValueCount = 1;
	begin.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	const ImVec2 size((float)target.width, (float)target.height);
	static ImDrawList list(ImGui::GetDrawListSharedData());
	list._ResetForNewFrame();
	list.PushClipRect(ImVec2(0, 0), size);
	list.PushTexture(ImTextureRef((ImTextureID)(size_t)texture));
	list.AddImage(ImTextureRef((ImTextureID)(size_t)texture), ImVec2(0, 0), size, ImVec2(shiftU, shiftV), ImVec2(u + shiftU, v + shiftV),
			IM_COL32(255, 255, 255, blend ? (int)(alpha * 255.f) : 255));
	ImDrawData data;
	data.Clear();
	data.Valid = true;
	data.DisplayPos = ImVec2(0, 0);
	data.DisplaySize = size;
	data.FramebufferScale = ImVec2(1, 1);
	data.AddDrawList(&list);
	ImGui_ImplVulkan_RenderDrawData(&data, cmd);
	vkCmdEndRenderPass(cmd);
	target.texture.OverrideImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	target.drawn = true;
}
}

// ---------------------------------------------------------------------- FSR

namespace
{
#include "fsr_spirv.inc"

struct Fsr
{
	bool tried = false, ready = false;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipelineLayout easuLayout = VK_NULL_HANDLE, rcasLayout = VK_NULL_HANDLE;
	VkPipeline easu = VK_NULL_HANDLE, rcas = VK_NULL_HANDLE;
	// A set is written each time it is used, and must not be written again
	// while a frame under way reads it: they go round.
	static constexpr unsigned Sets = 64;
	VkDescriptorSet sets[Sets] = {};
	unsigned next = 0;
	Target easuTarget, rcasTarget;
	int saidW = 0, saidH = 0, saidOutW = 0, saidOutH = 0;
} fsr;

// What FsrEasuCon() of ffx_fsr1.h computes: floats, passed as their bits.
struct EasuConstants
{
	uint32_t con0[4], con1[4], con2[4], con3[4];
	float limit[4];
};

uint32_t floatBits(float v)
{
	uint32_t u;
	memcpy(&u, &v, 4);
	return u;
}

// viewW x viewH: the picture; texW x texH: the texture it is a part of.
EasuConstants easuConstants(float viewW, float viewH, float texW, float texH, float outW, float outH)
{
	EasuConstants c{};
	c.con0[0] = floatBits(viewW / outW);
	c.con0[1] = floatBits(viewH / outH);
	c.con0[2] = floatBits(0.5f * viewW / outW - 0.5f);
	c.con0[3] = floatBits(0.5f * viewH / outH - 0.5f);
	c.con1[0] = floatBits(1.f / texW);
	c.con1[1] = floatBits(1.f / texH);
	c.con1[2] = floatBits(1.f / texW);
	c.con1[3] = floatBits(-1.f / texH);
	c.con2[0] = floatBits(-1.f / texW);
	c.con2[1] = floatBits(2.f / texH);
	c.con2[2] = floatBits(1.f / texW);
	c.con2[3] = floatBits(2.f / texH);
	c.con3[0] = floatBits(0.f);
	c.con3[1] = floatBits(4.f / texH);
	// A gather centred here reads the picture's last texels and none beyond.
	c.limit[0] = (viewW - 1.f) / texW;
	c.limit[1] = (viewH - 1.f) / texH;
	return c;
}

VkShaderModule shaderModule(const uint32_t *words, size_t bytes)
{
	VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = bytes;
	info.pCode = words;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(g_vulkan_context->GetDevice(), &info, nullptr, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}

VkPipeline fsrPipeline(VkShaderModule vertex, VkShaderModule fragment, VkPipelineLayout layout,
		VkRenderPass pass = VK_NULL_HANDLE)
{
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
	info.layout = layout;
	// The pass such pictures are drawn in: one colour attachment, left
	// readable by a shader.
	info.renderPass = pass != VK_NULL_HANDLE ? pass : shaderPass;
	VkPipeline pipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return pipeline;
}

// What does not depend on the picture's size, made when FSR is first asked for.
bool fsrInit()
{
	if (fsr.tried)
		return fsr.ready;
	fsr.tried = true;
	VkDevice device = g_vulkan_context->GetDevice();
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &binding;
	VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Fsr::Sets };
	VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.maxSets = Fsr::Sets;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxAnisotropy = 1.f;
	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fsr.setLayout) != VK_SUCCESS
			|| vkCreateDescriptorPool(device, &poolInfo, nullptr, &fsr.pool) != VK_SUCCESS
			|| vkCreateSampler(device, &samplerInfo, nullptr, &fsr.sampler) != VK_SUCCESS)
	{
		diag::mark("fsr: the driver refused a layout, a pool or a sampler: the picture is stretched the usual way");
		return false;
	}
	VkDescriptorSetLayout layouts[Fsr::Sets];
	for (VkDescriptorSetLayout& layout : layouts)
		layout = fsr.setLayout;
	VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate.descriptorPool = fsr.pool;
	allocate.descriptorSetCount = Fsr::Sets;
	allocate.pSetLayouts = layouts;
	VkPushConstantRange range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(EasuConstants) };
	VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayout.setLayoutCount = 1;
	pipelineLayout.pSetLayouts = &fsr.setLayout;
	pipelineLayout.pushConstantRangeCount = 1;
	pipelineLayout.pPushConstantRanges = &range;
	if (vkAllocateDescriptorSets(device, &allocate, fsr.sets) != VK_SUCCESS
			|| vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &fsr.easuLayout) != VK_SUCCESS)
	{
		diag::mark("fsr: the driver refused the sets or a layout: the picture is stretched the usual way");
		return false;
	}
	range.size = 16;
	if (vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &fsr.rcasLayout) != VK_SUCCESS)
		return false;
	const VkShaderModule vertex = shaderModule(fsr_vertex_spirv, sizeof(fsr_vertex_spirv));
	const VkShaderModule easu = shaderModule(fsr_easu_spirv, sizeof(fsr_easu_spirv));
	const VkShaderModule rcas = shaderModule(fsr_rcas_spirv, sizeof(fsr_rcas_spirv));
	if (vertex != VK_NULL_HANDLE && easu != VK_NULL_HANDLE && rcas != VK_NULL_HANDLE)
	{
		fsr.easu = fsrPipeline(vertex, easu, fsr.easuLayout);
		fsr.rcas = fsrPipeline(vertex, rcas, fsr.rcasLayout);
	}
	for (VkShaderModule module : { vertex, easu, rcas })
		if (module != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, module, nullptr);
	fsr.ready = fsr.easu != VK_NULL_HANDLE && fsr.rcas != VK_NULL_HANDLE;
	diag::mark(fsr.ready ? "fsr: ready" : "fsr: the driver refused a shader or a pipeline: the picture is stretched the usual way");
	return fsr.ready;
}

// One of the two passes: `view` through a pipeline, over the whole of a target.
void fsrPass(Target& target, VkPipeline pipeline, VkPipelineLayout layout, VkImageView view, VkImageLayout viewLayout,
		const void *constants, uint32_t constantsSize)
{
	VkDevice device = g_vulkan_context->GetDevice();
	const VkDescriptorSet set = fsr.sets[fsr.next++ % Fsr::Sets];
	VkDescriptorImageInfo image{ fsr.sampler, view, viewLayout };
	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	VkClearValue clear{};
	clear.color = { { 0.f, 0.f, 0.f, 1.f } };
	const VkExtent2D size{ (uint32_t)target.width, (uint32_t)target.height };
	VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	begin.renderPass = shaderPass;
	begin.framebuffer = target.framebuffer;
	begin.renderArea = { { 0, 0 }, size };
	begin.clearValueCount = 1;
	begin.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	const VkViewport viewport{ 0.f, 0.f, (float)size.width, (float)size.height, 0.f, 1.f };
	const VkRect2D scissor{ { 0, 0 }, size };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, constantsSize, constants);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	target.texture.OverrideImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	target.drawn = true;
}

void fsrShutdown()
{
	VkDevice device = g_vulkan_context->GetDevice();
	for (Target *target : { &fsr.easuTarget, &fsr.rcasTarget })
	{
		retire(target->set);
		if (target->framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(device, target->framebuffer, nullptr);
		target->texture.Destroy(false);
	}
	if (fsr.easu != VK_NULL_HANDLE)
		vkDestroyPipeline(device, fsr.easu, nullptr);
	if (fsr.rcas != VK_NULL_HANDLE)
		vkDestroyPipeline(device, fsr.rcas, nullptr);
	if (fsr.easuLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device, fsr.easuLayout, nullptr);
	if (fsr.rcasLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device, fsr.rcasLayout, nullptr);
	if (fsr.sampler != VK_NULL_HANDLE)
		vkDestroySampler(device, fsr.sampler, nullptr);
	if (fsr.pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(device, fsr.pool, nullptr);
	if (fsr.setLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(device, fsr.setLayout, nullptr);
	fsr = Fsr();
}
}

namespace
{
// FSR from an image: `view` (in `layout`), whose part up to u, v is the
// picture of `width` x `height`.
void *upscaleView(VkImageView view, VkImageLayout layout, int width, int height, float u, float v, int outWidth, int outHeight,
		int sharpness)
{
	if (width < 16 || height < 16 || u <= 0 || v <= 0 || outWidth > 8192 || outHeight > 8192 || width > outWidth
			|| height > outHeight || (width == outWidth && height == outHeight))
		return nullptr;
	if (view == VK_NULL_HANDLE || !fsrInit() || !ensureTarget(fsr.easuTarget, outWidth, outHeight, swapFormat, shaderPass)
			|| !ensureTarget(fsr.rcasTarget, outWidth, outHeight, swapFormat, shaderPass))
		return nullptr;
	const EasuConstants easu = easuConstants((float)width, (float)height, (float)width / u, (float)height / v,
			(float)outWidth, (float)outHeight);
	fsrPass(fsr.easuTarget, fsr.easu, fsr.easuLayout, view, layout, &easu, sizeof(easu));
	// The sharpening, in stops less than the most: 0 is the sharpest.
	static const float stops[3] = { 1.f, 0.25f, 0.f };
	const uint32_t rcas[4] = { floatBits(std::exp2(-stops[std::clamp(sharpness, 0, 2)])), 0, 0, 0 };
	fsrPass(fsr.rcasTarget, fsr.rcas, fsr.rcasLayout, fsr.easuTarget.texture.GetView(),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, rcas, sizeof(rcas));
	if (fsr.saidW != width || fsr.saidH != height || fsr.saidOutW != outWidth || fsr.saidOutH != outHeight)
	{
		diag::mark("fsr: %d x %d to %d x %d", width, height, outWidth, outHeight);
		fsr.saidW = width;
		fsr.saidH = height;
		fsr.saidOutW = outWidth;
		fsr.saidOutH = outHeight;
	}
	return (void *)fsr.rcasTarget.set;
}
}

void *upscale(void *texture, int width, int height, float u, float v, int outWidth, int outHeight, int sharpness)
{
	if (texture == nullptr || !frameOpen)
		return nullptr;
	// The image behind what ImGui draws.
	VkImageView view = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (texture == (void *)wrappedSet && wrappedView != VK_NULL_HANDLE)
	{
		view = wrappedView;
		layout = (VkImageLayout)wrappedLayout;
	}
	else
	{
		const auto it = dynamicViews.find(texture);
		if (it != dynamicViews.end())
			view = it->second;
	}
	return view != VK_NULL_HANDLE ? upscaleView(view, layout, width, height, u, v, outWidth, outHeight, sharpness) : nullptr;
}

// --------------------------------------------------------- frame generation

namespace
{
#include "fg_spirv.inc"

struct Generation
{
	bool tried = false, ready = false;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline copy = VK_NULL_HANDLE, luma = VK_NULL_HANDLE, search = VK_NULL_HANDLE, tidy = VK_NULL_HANDLE,
			choose = VK_NULL_HANDLE, blend = VK_NULL_HANDLE, check = VK_NULL_HANDLE;
	VkPipeline copyFloat = VK_NULL_HANDLE;		// the copy into a picture of floats
	// A set is written each time it is used: they go round, and none comes
	// round again while a frame under way reads it.
	static constexpr unsigned Sets = 64;
	VkDescriptorSet sets[Sets] = {};
	unsigned next = 0;
	// The picture before and this one, as they are drawn; of each, how bright
	// its parts are, at four sizes (the largest first).
	static constexpr int Levels = 4;
	Target kept[2], bright[2][Levels];
	// How the picture moved, at each of the four sizes; at the largest, put in
	// order (fg_tidy.frag); decided for each part of the picture itself
	// (fg_choose.frag); and the picture between.
	Target movement[Levels], tidied, chosen, between;
	// The quality kind's two-way check: the movement found from each frame's
	// side alone, and the tidied movement with what they disagree on marked.
	Target fromBefore, fromNow, checked;
	int now = 0;				// which of the two is this frame's
	bool haveNow = false, haveBefore = false, haveMovement = false, useChecked = false;
	uint64_t keptAt = 0;		// the display frame this frame's was kept in
	int saidW = 0, saidH = 0;
} generation;

struct GenerationConstants
{
	float size[4];
	float more[4];
};

bool generationInit()
{
	Generation& g = generation;
	if (g.tried)
		return g.ready;
	g.tried = true;
	VkDevice device = g_vulkan_context->GetDevice();
	VkDescriptorSetLayoutBinding bindings[3] = {};
	for (uint32_t i = 0; i < 3; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 3;
	layoutInfo.pBindings = bindings;
	VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Generation::Sets * 3 };
	VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.maxSets = Generation::Sets;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxAnisotropy = 1.f;
	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &g.setLayout) != VK_SUCCESS
			|| vkCreateDescriptorPool(device, &poolInfo, nullptr, &g.pool) != VK_SUCCESS
			|| vkCreateSampler(device, &samplerInfo, nullptr, &g.sampler) != VK_SUCCESS)
	{
		diag::mark("frame generation: the driver refused a layout, a pool or a sampler: it stays off");
		return false;
	}
	VkDescriptorSetLayout layouts[Generation::Sets];
	for (VkDescriptorSetLayout& layout : layouts)
		layout = g.setLayout;
	VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate.descriptorPool = g.pool;
	allocate.descriptorSetCount = Generation::Sets;
	allocate.pSetLayouts = layouts;
	VkPushConstantRange range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GenerationConstants) };
	VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayout.setLayoutCount = 1;
	pipelineLayout.pSetLayouts = &g.setLayout;
	pipelineLayout.pushConstantRangeCount = 1;
	pipelineLayout.pPushConstantRanges = &range;
	if (vkAllocateDescriptorSets(device, &allocate, g.sets) != VK_SUCCESS
			|| vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &g.layout) != VK_SUCCESS)
	{
		diag::mark("frame generation: the driver refused the sets or a layout: it stays off");
		return false;
	}
	const VkShaderModule vertex = shaderModule(fsr_vertex_spirv, sizeof(fsr_vertex_spirv));
	const VkShaderModule copy = shaderModule(fg_copy_spirv, sizeof(fg_copy_spirv));
	const VkShaderModule luma = shaderModule(fg_luma_spirv, sizeof(fg_luma_spirv));
	const VkShaderModule search = shaderModule(fg_search_spirv, sizeof(fg_search_spirv));
	const VkShaderModule tidy = shaderModule(fg_tidy_spirv, sizeof(fg_tidy_spirv));
	const VkShaderModule choose = shaderModule(fg_choose_spirv, sizeof(fg_choose_spirv));
	const VkShaderModule blend = shaderModule(fg_blend_spirv, sizeof(fg_blend_spirv));
	const VkShaderModule check = shaderModule(fg_check_spirv, sizeof(fg_check_spirv));
	if (check != VK_NULL_HANDLE)
	{
		g.check = fsrPipeline(vertex, check, g.layout, floatPass);
		vkDestroyShaderModule(device, check, nullptr);
	}
	if (vertex != VK_NULL_HANDLE && copy != VK_NULL_HANDLE && luma != VK_NULL_HANDLE && search != VK_NULL_HANDLE
			&& tidy != VK_NULL_HANDLE && choose != VK_NULL_HANDLE && blend != VK_NULL_HANDLE)
	{
		g.choose = fsrPipeline(vertex, choose, g.layout, floatPass);
		g.copy = fsrPipeline(vertex, copy, g.layout);
		g.copyFloat = fsrPipeline(vertex, copy, g.layout, floatPass);
		g.luma = fsrPipeline(vertex, luma, g.layout, floatPass);
		g.search = fsrPipeline(vertex, search, g.layout, floatPass);
		g.tidy = fsrPipeline(vertex, tidy, g.layout, floatPass);
		g.blend = fsrPipeline(vertex, blend, g.layout);
	}
	for (VkShaderModule module : { vertex, copy, luma, search, tidy, choose, blend })
		if (module != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, module, nullptr);
	g.ready = g.copy != VK_NULL_HANDLE && g.copyFloat != VK_NULL_HANDLE && g.luma != VK_NULL_HANDLE && g.search != VK_NULL_HANDLE && g.tidy != VK_NULL_HANDLE
			&& g.choose != VK_NULL_HANDLE && g.blend != VK_NULL_HANDLE;
	diag::mark(g.ready ? "frame generation: ready" : "frame generation: the driver refused a shader or a pipeline: it stays off");
	return g.ready;
}

// One pass: up to three pictures through a pipeline, over the whole of a target.
void generationPass(Target& target, VkPipeline pipeline, VkImageView first, VkImageLayout firstLayout,
		VkImageView second, VkImageView third, const GenerationConstants& constants)
{
	Generation& g = generation;
	VkDevice device = g_vulkan_context->GetDevice();
	const VkDescriptorSet set = g.sets[g.next++ % Generation::Sets];
	// A pass that reads fewer than three is given the first again.
	VkDescriptorImageInfo images[3] = {
		{ g.sampler, first, firstLayout },
		{ g.sampler, second != VK_NULL_HANDLE ? second : first,
				second != VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : firstLayout },
		{ g.sampler, third != VK_NULL_HANDLE ? third : first,
				third != VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : firstLayout },
	};
	VkWriteDescriptorSet writes[3] = {};
	for (uint32_t i = 0; i < 3; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	VkClearValue clear{};
	clear.color = { { 0.f, 0.f, 0.f, 1.f } };
	const VkExtent2D size{ (uint32_t)target.width, (uint32_t)target.height };
	VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	begin.renderPass = target.format == FloatFormat ? floatPass : shaderPass;
	begin.framebuffer = target.framebuffer;
	begin.renderArea = { { 0, 0 }, size };
	begin.clearValueCount = 1;
	begin.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	const VkViewport viewport{ 0.f, 0.f, (float)size.width, (float)size.height, 0.f, 1.f };
	const VkRect2D scissor{ { 0, 0 }, size };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	target.texture.OverrideImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	target.drawn = true;
}

void generationShutdown()
{
	Generation& g = generation;
	VkDevice device = g_vulkan_context->GetDevice();
	for (Target& target : g.kept)
		destroyTarget(target);
	for (auto& levels : g.bright)
		for (Target& target : levels)
			destroyTarget(target);
	for (Target& target : g.movement)
		destroyTarget(target);
	destroyTarget(g.tidied);
	destroyTarget(g.chosen);
	destroyTarget(g.between);
	destroyTarget(g.fromBefore);
	destroyTarget(g.fromNow);
	destroyTarget(g.checked);
	for (VkPipeline pipeline : { g.copy, g.copyFloat, g.luma, g.search, g.tidy, g.choose, g.blend, g.check })
		if (pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(device, pipeline, nullptr);
	if (g.layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device, g.layout, nullptr);
	if (g.sampler != VK_NULL_HANDLE)
		vkDestroySampler(device, g.sampler, nullptr);
	if (g.pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(device, g.pool, nullptr);
	if (g.setLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(device, g.setLayout, nullptr);
	g = Generation();
}

// The image behind a picture ImGui draws: the emulator's, the software
// renderer's, or FSR's result.
bool viewBehind(void *texture, VkImageView& view, VkImageLayout& layout)
{
	layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (texture == nullptr)
		return false;
	if (texture == (void *)wrappedSet && wrappedView != VK_NULL_HANDLE)
	{
		view = wrappedView;
		layout = (VkImageLayout)wrappedLayout;
		return true;
	}
	if (texture == (void *)fsr.rcasTarget.set && fsr.rcasTarget.framebuffer != VK_NULL_HANDLE)
	{
		view = fsr.rcasTarget.texture.GetView();
		return true;
	}
	for (const Target *target : { &generation.kept[0], &generation.kept[1], &generation.between })
		if (texture == (void *)target->set && target->framebuffer != VK_NULL_HANDLE)
		{
			view = target->texture.GetView();
			return true;
		}
	const auto it = dynamicViews.find(texture);
	if (it == dynamicViews.end())
		return false;
	view = it->second;
	return true;
}
}

void forgetGenerated()
{
	generation.haveNow = generation.haveBefore = generation.haveMovement = false;
}

void *generated(void *texture, int width, int height, float u, float v, bool fresh, float phase, int quality, bool debug)
{
	const bool lighter = quality <= 0;
	const bool careful = quality >= 2 && generation.check != VK_NULL_HANDLE;
	Generation& g = generation;
	if (texture == nullptr || !frameOpen || width < 64 || height < 64 || width > 8192 || height > 8192 || u <= 0 || v <= 0)
		return nullptr;
	if (!generationInit())
		return nullptr;
	// Another size is another picture: what was kept is not its past.
	if (g.kept[g.now].width != width || g.kept[g.now].height != height)
		forgetGenerated();
	// The sizes movement is looked for at: the largest about 360 lines, each
	// of the others half the one before.
	int levelW[Generation::Levels], levelH[Generation::Levels];
	levelH[0] = std::clamp(height, 64, careful ? 480 : 360) / 8 * 8;
	levelW[0] = std::clamp((int)std::lround((double)levelH[0] * width / height / 8.0) * 8, 64, 1024);
	for (int i = 1; i < Generation::Levels; i++)
	{
		levelW[i] = levelW[i - 1] / 2;
		levelH[i] = levelH[i - 1] / 2;
	}
	// And the size the movement is decided at: half the picture's, but no
	// less than a PlayStation's own lines when the picture has them, and (the
	// lighter way) no more than 540.
	// (The quality kind: the picture's own size, up to 1080.)
	const int chosenH = careful ? std::min(height, 1080) : std::clamp(height / 2, std::min(height, 480), lighter ? 540 : 1080);
	const int chosenW = std::max((int)std::lround((double)chosenH * width / height), 64);
	if (fresh)
	{
		VkImageView view = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (!viewBehind(texture, view, layout))
			return nullptr;
		// A picture kept long ago is not the one before this (the game was
		// paused, or a menu was over it). A game that draws ten pictures a
		// second has twelve refreshes between two of them at 120 Hz.
		const bool follows = g.haveNow && frames - g.keptAt <= 16;
		const int now = g.now ^ 1;
		if (!ensureTarget(g.kept[now], width, height, swapFormat, shaderPass)
				|| !ensureTarget(g.between, width, height, swapFormat, shaderPass))
			return nullptr;
		for (int i = 0; i < Generation::Levels; i++)
			if (!ensureTarget(g.bright[now][i], levelW[i], levelH[i], FloatFormat, floatPass, false)
					|| !ensureTarget(g.movement[i], levelW[i], levelH[i], FloatFormat, floatPass, false))
				return nullptr;
		if (!ensureTarget(g.tidied, levelW[0], levelH[0], FloatFormat, floatPass, false)
				|| !ensureTarget(g.chosen, chosenW, chosenH, FloatFormat, floatPass, false))
			return nullptr;
		g.now = now;
		g.haveBefore = follows;
		g.haveNow = true;
		g.haveMovement = false;
		g.keptAt = frames;
		// The picture, kept.
		GenerationConstants c{};
		c.size[0] = 1.f / (float)width;
		c.size[1] = 1.f / (float)height;
		c.more[0] = u;
		c.more[1] = v;
		generationPass(g.kept[now], g.copy, view, layout, VK_NULL_HANDLE, VK_NULL_HANDLE, c);
		// How bright its parts are, smaller and smaller.
		for (int i = 0; i < Generation::Levels; i++)
		{
			c = GenerationConstants();
			c.size[0] = 1.f / (float)levelW[i];
			c.size[1] = 1.f / (float)levelH[i];
			c.more[0] = c.size[0] / 3.f;
			c.more[1] = c.size[1] / 3.f;
			c.more[2] = i == 0 ? 1.f : 0.f;
			generationPass(g.bright[now][i], g.luma, i == 0 ? g.kept[now].texture.GetView() : g.bright[now][i - 1].texture.GetView(),
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE, VK_NULL_HANDLE, c);
		}
		if (g.haveBefore && g.bright[now ^ 1][0].drawn && g.bright[now ^ 1][0].width == levelW[0]
				&& g.bright[now ^ 1][0].height == levelH[0])
		{
			// How it moved since the picture before: from the smallest size,
			// where a step is a long way, to the largest, in half steps.
			for (int i = Generation::Levels - 1; i >= 0; i--)
			{
				const bool smallest = i == Generation::Levels - 1;
				c = GenerationConstants();
				c.size[0] = 1.f / (float)levelW[i];
				c.size[1] = 1.f / (float)levelH[i];
				c.size[2] = 0.5f;
				c.more[0] = smallest ? 4.f : 2.f;
				c.more[1] = i == 0 ? 0.5f : 1.f;
				c.more[2] = smallest ? 0.f : 1.f;
				c.more[3] = 0.004f;
				generationPass(g.movement[i], g.search, g.bright[now ^ 1][i].texture.GetView(),
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, g.bright[now][i].texture.GetView(),
						smallest ? VK_NULL_HANDLE : g.movement[i + 1].texture.GetView(), c);
			}
			// And put in order: a patch alone in its movement gives way.
			c = GenerationConstants();
			c.size[0] = 1.f / (float)levelW[0];
			c.size[1] = 1.f / (float)levelH[0];
			c.more[0] = 0.02f;
			generationPass(g.tidied, g.tidy, g.bright[now ^ 1][0].texture.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					g.bright[now][0].texture.GetView(), g.movement[0].texture.GetView(), c);
			g.useChecked = false;
			if (careful && ensureTarget(g.fromBefore, levelW[0], levelH[0], FloatFormat, floatPass, false)
					&& ensureTarget(g.fromNow, levelW[0], levelH[0], FloatFormat, floatPass, false)
					&& ensureTarget(g.checked, levelW[0], levelH[0], FloatFormat, floatPass, false))
			{
				// The two-way check: the movement found again from each frame's
				// side (seeded by the next size's), and the tidied movement
				// marked where both say otherwise.
				for (int side = 0; side < 2; side++)
				{
					c = GenerationConstants();
					c.size[0] = 1.f / (float)levelW[0];
					c.size[1] = 1.f / (float)levelH[0];
					c.size[2] = side == 0 ? 0.f : 1.f;
					c.more[0] = 2.f;
					c.more[1] = 0.5f;
					c.more[2] = 1.f;
					c.more[3] = 0.004f;
					generationPass(side == 0 ? g.fromBefore : g.fromNow, g.search, g.bright[now ^ 1][0].texture.GetView(),
							VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, g.bright[now][0].texture.GetView(),
							g.movement[1].texture.GetView(), c);
				}
				c = GenerationConstants();
				c.size[0] = 1.f / (float)levelW[0];
				c.size[1] = 1.f / (float)levelH[0];
				generationPass(g.checked, g.check, g.tidied.texture.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						g.fromBefore.texture.GetView(), g.fromNow.texture.GetView(), c);
				g.useChecked = true;
			}
			g.haveMovement = true;
		}
		else
			g.haveBefore = false;
		if (g.saidW != width || g.saidH != height)
		{
			diag::mark("frame generation: pictures of %d x %d, movement looked for at %d x %d", width, height, levelW[0], levelH[0]);
			g.saidW = width;
			g.saidH = height;
		}
	}
	if (!g.haveNow || g.kept[g.now].width != width || g.kept[g.now].height != height)
		return nullptr;
	// This frame itself; or, with the one before to go by, the picture a part
	// of the way from that one to this.
	// (Past 1, up to 2: a picture ahead of this one, along the same movement.)
	phase = std::clamp(phase, 0.f, 2.f);
	if (std::fabs(phase - 1.f) < 0.001f || !g.haveBefore || !g.haveMovement)
		return (void *)g.kept[g.now].set;
	const Target& movement = g.useChecked ? g.checked : g.tidied;
	// The movement, decided for each part of the picture on the picture
	// itself, and for this moment between the two: where things meet, which
	// of them is in front depends on how far each has come.
	GenerationConstants c{};
	c.size[0] = 1.f / (float)g.chosen.width;
	c.size[1] = 1.f / (float)g.chosen.height;
	c.size[2] = 1.f / (float)(g.useChecked ? g.checked : g.tidied).width;
	c.size[3] = 1.f / (float)(g.useChecked ? g.checked : g.tidied).height;
	c.more[0] = 1.f / (float)width;
	c.more[1] = 1.f / (float)height;
	c.more[2] = lighter ? 0.f : 1.f;
	c.more[3] = phase;
	generationPass(g.chosen, g.choose, g.kept[g.now ^ 1].texture.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			g.kept[g.now].texture.GetView(), movement.texture.GetView(), c);
	c = GenerationConstants();
	c.size[0] = 1.f / (float)width;
	c.size[1] = 1.f / (float)height;
	c.more[0] = phase;
	c.more[1] = debug ? 1.f : 0.f;
	generationPass(g.between, g.blend, g.kept[g.now ^ 1].texture.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			g.kept[g.now].texture.GetView(), g.chosen.texture.GetView(), c);
	return (void *)g.between.set;
}

// ------------------------------------------------------------ the picture's look

namespace
{
#include "look_spirv.inc"
}
}

#include "NIS_Config.h"

namespace fe::display
{
namespace
{

struct LookConstants
{
	float a[4], b[4], c[4], d[4];
};

// What crt-guest-advanced's presets change from its own values.
struct CrtPreset
{
	const char *name;
	std::vector<std::pair<const char *, float>> values;
};
const std::vector<CrtPreset>& crtPresets()
{
	static const std::vector<CrtPreset> presets = {
		{ "Guest's own", {} },
		{ "Home television", { { "warpX", 0.03f }, { "warpY", 0.04f }, { "csize", 0.02f }, { "shadowMask", 1.f },
				{ "maskstr", 0.35f }, { "glow", 0.10f }, { "bloom", 0.15f }, { "halation", 0.08f }, { "h_sharp", 3.0f },
				{ "s_sharp", 0.30f }, { "vigstr", 0.20f } } },
		{ "Studio monitor", { { "shadowMask", 6.f }, { "maskstr", 0.35f }, { "h_sharp", 7.0f }, { "s_sharp", 1.0f },
				{ "beam_min", 1.60f }, { "beam_max", 1.05f }, { "scanline1", 10.f }, { "glow", 0.04f },
				{ "brightboost", 1.70f }, { "brightboost1", 1.25f } } },
		{ "Arcade monitor", { { "warpX", 0.02f }, { "warpY", 0.03f }, { "slotmask", 0.50f }, { "slotmask1", 0.60f },
				{ "maskstr", 0.40f }, { "glow", 0.12f }, { "bloom", 0.20f }, { "brightboost", 1.60f },
				{ "beam_min", 1.20f } } },
		{ "Soft", { { "h_sharp", 2.0f }, { "s_sharp", 0.20f }, { "glow", 0.15f }, { "maskstr", 0.15f },
				{ "beam_min", 1.0f } } },
	};
	return presets;
}

struct LookState
{
	bool tried = false, ready = false;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline signal = VK_NULL_HANDLE, scale = VK_NULL_HANDLE, cas = VK_NULL_HANDLE;
	// The picture at its own size, looked after; stretched to the screen's
	// size; and that sharpened (CAS); the picture NIS starts from.
	Target staged, stretched, sharpened, before;
	unsigned frame = 0;
	// NVIDIA Image Scaling.
	bool nisTried = false, nisReady = false;
	VkDescriptorSetLayout nisSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool nisPool = VK_NULL_HANDLE;
	static constexpr unsigned NisSets = 8;
	VkDescriptorSet nisSets[NisSets] = {};
	unsigned nisNext = 0;
	VkPipelineLayout nisLayout = VK_NULL_HANDLE;
	VkPipeline nis = VK_NULL_HANDLE;
	Vulkan::Texture coefScaler, coefUsm, nisOut;
	Vulkan::StagingBuffer coefStaging, nisUniforms;
	bool coefUploaded = false;
	VkDescriptorSet nisOutSet = VK_NULL_HANDLE;
	// crt-guest-advanced.
	chain::Chain *crt = nullptr;
	bool crtTried = false;
	// A preset brought in from a USB drive (slangimport.cpp), by its name.
	chain::Chain *imported = nullptr;
	std::string importedName;
	bool importedTried = false;
	int crtPreset = -1, crtMaskSize = 0;
	VkImageView crtView = VK_NULL_HANDLE;
	VkDescriptorSet crtSet = VK_NULL_HANDLE;
	std::string said;
} look;

bool lookInit()
{
	LookState& l = look;
	if (l.tried)
		return l.ready;
	l.tried = true;
	if (!fsrInit())
		return false;
	VkDevice device = g_vulkan_context->GetDevice();
	VkPushConstantRange range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LookConstants) };
	VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayout.setLayoutCount = 1;
	pipelineLayout.pSetLayouts = &fsr.setLayout;
	pipelineLayout.pushConstantRangeCount = 1;
	pipelineLayout.pPushConstantRanges = &range;
	if (vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &l.layout) != VK_SUCCESS)
		return false;
	const VkShaderModule vertex = shaderModule(fsr_vertex_spirv, sizeof(fsr_vertex_spirv));
	const VkShaderModule signal = shaderModule(look_signal_spirv, sizeof(look_signal_spirv));
	const VkShaderModule scale = shaderModule(look_scale_spirv, sizeof(look_scale_spirv));
	const VkShaderModule cas = shaderModule(look_cas_spirv, sizeof(look_cas_spirv));
	if (vertex != VK_NULL_HANDLE && signal != VK_NULL_HANDLE && scale != VK_NULL_HANDLE && cas != VK_NULL_HANDLE)
	{
		l.signal = fsrPipeline(vertex, signal, l.layout);
		l.scale = fsrPipeline(vertex, scale, l.layout);
		l.cas = fsrPipeline(vertex, cas, l.layout);
	}
	for (VkShaderModule module : { vertex, signal, scale, cas })
		if (module != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, module, nullptr);
	l.ready = l.signal != VK_NULL_HANDLE && l.scale != VK_NULL_HANDLE && l.cas != VK_NULL_HANDLE;
	diag::mark(l.ready ? "look: ready" : "look: the driver refused a shader or a pipeline: the picture is drawn the usual way");
	return l.ready;
}

bool nisInit()
{
	LookState& l = look;
	if (l.nisTried)
		return l.nisReady;
	l.nisTried = true;
	VkDevice device = g_vulkan_context->GetDevice();
	const VkDescriptorType types[6] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER,
		VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
	VkDescriptorSetLayoutBinding bindings[6] = {};
	for (uint32_t i = 0; i < 6; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = types[i];
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 6;
	layoutInfo.pBindings = bindings;
	const VkDescriptorPoolSize sizes[4] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, LookState::NisSets }, { VK_DESCRIPTOR_TYPE_SAMPLER, LookState::NisSets },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, LookState::NisSets * 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, LookState::NisSets },
	};
	VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.maxSets = LookState::NisSets;
	poolInfo.poolSizeCount = 4;
	poolInfo.pPoolSizes = sizes;
	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &l.nisSetLayout) != VK_SUCCESS
			|| vkCreateDescriptorPool(device, &poolInfo, nullptr, &l.nisPool) != VK_SUCCESS)
		return false;
	VkDescriptorSetLayout layouts[LookState::NisSets];
	for (VkDescriptorSetLayout& layout : layouts)
		layout = l.nisSetLayout;
	VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate.descriptorPool = l.nisPool;
	allocate.descriptorSetCount = LookState::NisSets;
	allocate.pSetLayouts = layouts;
	VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayout.setLayoutCount = 1;
	pipelineLayout.pSetLayouts = &l.nisSetLayout;
	if (vkAllocateDescriptorSets(device, &allocate, l.nisSets) != VK_SUCCESS
			|| vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &l.nisLayout) != VK_SUCCESS)
		return false;
	const VkShaderModule module = shaderModule(look_nis_spirv, sizeof(look_nis_spirv));
	if (module != VK_NULL_HANDLE)
	{
		VkComputePipelineCreateInfo info{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		info.stage.module = module;
		info.stage.pName = "main";
		info.layout = l.nisLayout;
		vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &l.nis);
		vkDestroyShaderModule(device, module, nullptr);
	}
	// The filters' tables, 64 phases of eight numbers, as two RGBA32F texels a row.
	const uint32_t align = std::max<uint32_t>((uint32_t)g_vulkan_context->GetDeviceLimits().minUniformBufferOffsetAlignment, 256);
	if (l.nis == VK_NULL_HANDLE
			|| !l.coefScaler.Create(2, (uint32_t)kPhaseCount, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			|| !l.coefUsm.Create(2, (uint32_t)kPhaseCount, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			|| !l.coefStaging.Create(Vulkan::StagingBuffer::Type::Upload, sizeof(coef_scale) + sizeof(coef_usm),
					VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !l.coefStaging.Map()
			|| !l.nisUniforms.Create(Vulkan::StagingBuffer::Type::Upload, (VkDeviceSize)align * LookState::NisSets,
					VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !l.nisUniforms.Map())
	{
		diag::mark("look: NIS could not be made: FSR's stretch is used in its place");
		return false;
	}
	memcpy(l.coefStaging.GetMapPointer(), coef_scale, sizeof(coef_scale));
	memcpy(l.coefStaging.GetMapPointer() + sizeof(coef_scale), coef_usm, sizeof(coef_usm));
	l.coefStaging.FlushCPUCache();
	l.nisReady = true;
	diag::mark("look: NIS ready");
	return true;
}

// NVIDIA Image Scaling from `view` (a `width` x `height` picture, all of the
// image) to `outWidth` x `outHeight`, at most twice as large each way.
void *nisScale(VkImageView view, int width, int height, int outWidth, int outHeight, int sharpness)
{
	LookState& l = look;
	if (!nisInit())
		return nullptr;
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	if (!l.coefUploaded)
	{
		for (Vulkan::Texture *texture : { &l.coefScaler, &l.coefUsm })
		{
			texture->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			texture->UpdateFromBuffer(cmd, 0, 0, 0, 0, 2, (uint32_t)kPhaseCount, l.coefStaging.GetBuffer(),
					texture == &l.coefScaler ? 0 : (uint32_t)sizeof(coef_scale), 2);
			texture->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		l.coefUploaded = true;
	}
	NISConfig config{};
	static const float strength[3] = { 0.15f, 0.5f, 0.85f };
	if (!NVScalerUpdateConfig(config, strength[std::clamp(sharpness, 0, 2)], 0, 0, (uint32_t)width, (uint32_t)height,
				(uint32_t)width, (uint32_t)height, 0, 0, (uint32_t)outWidth, (uint32_t)outHeight, (uint32_t)outWidth,
				(uint32_t)outHeight))
		return nullptr;
	if (!l.nisOut.IsValid() || (int)l.nisOut.GetWidth() != outWidth || (int)l.nisOut.GetHeight() != outHeight)
	{
		retire(l.nisOutSet);
		l.nisOutSet = VK_NULL_HANDLE;
		l.nisOut.Destroy(true);
		if (!l.nisOut.Create((uint32_t)outWidth, (uint32_t)outHeight, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
			return nullptr;
		l.nisOutSet = ImGui_ImplVulkan_AddTexture(l.nisOut.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	const uint32_t align = std::max<uint32_t>((uint32_t)g_vulkan_context->GetDeviceLimits().minUniformBufferOffsetAlignment, 256);
	const unsigned slot = l.nisNext++ % LookState::NisSets;
	memcpy(l.nisUniforms.GetMapPointer() + (size_t)slot * align, &config, sizeof(config));
	l.nisUniforms.FlushCPUCache((VkDeviceSize)slot * align, sizeof(config));
	const VkDescriptorSet set = l.nisSets[slot];
	VkDescriptorBufferInfo buffer{ l.nisUniforms.GetBuffer(), (VkDeviceSize)slot * align, sizeof(config) };
	VkDescriptorImageInfo images[5] = {
		{ fsr.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED },
		{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ VK_NULL_HANDLE, l.nisOut.GetView(), VK_IMAGE_LAYOUT_GENERAL },
		{ VK_NULL_HANDLE, l.coefScaler.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ VK_NULL_HANDLE, l.coefUsm.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	const VkDescriptorType types[5] = { VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
	VkWriteDescriptorSet writes[6] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &buffer;
	for (uint32_t i = 0; i < 5; i++)
	{
		writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i + 1].dstSet = set;
		writes[i + 1].dstBinding = i + 1;
		writes[i + 1].descriptorCount = 1;
		writes[i + 1].descriptorType = types[i];
		writes[i + 1].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets(g_vulkan_context->GetDevice(), 6, writes, 0, nullptr);
	// The picture it reads was drawn by a render pass; the one it writes was
	// read by ImGui a frame ago.
	VkMemoryBarrier drawn{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	drawn.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	drawn.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &drawn,
			0, nullptr, 0, nullptr);
	VkImageMemoryBarrier toWrite{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	toWrite.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	toWrite.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toWrite.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	toWrite.srcQueueFamilyIndex = toWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toWrite.image = l.nisOut.GetImage();
	toWrite.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
			nullptr, 1, &toWrite);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l.nis);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l.nisLayout, 0, 1, &set, 0, nullptr);
	// NIS_Scaler.h's blocks: 32 x 24 pixels each.
	vkCmdDispatch(cmd, (uint32_t)(outWidth + 31) / 32, (uint32_t)(outHeight + 23) / 24, 1);
	VkImageMemoryBarrier toRead = toWrite;
	toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
			nullptr, 1, &toRead);
	l.nisOut.OverrideImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return (void *)l.nisOutSet;
}

uint16_t halfBits(float value)
{
	uint32_t f = floatBits(value);
	const uint32_t sign = (f >> 16) & 0x8000u;
	const int exponent = (int)((f >> 23) & 0xff) - 127 + 15;
	const uint32_t mantissa = (f >> 13) & 0x3ffu;
	if (exponent <= 0)
		return (uint16_t)sign;
	if (exponent >= 31)
		return (uint16_t)(sign | 0x7c00u);
	return (uint16_t)(sign | ((uint32_t)exponent << 10) | mantissa);
}

void *crtRun(VkImageView view, int width, int height, int outWidth, int outHeight, int preset)
{
	LookState& l = look;
	// Past the title's own: one brought in from a USB drive.
	const int own = (int)crtPresets().size();
	if (preset >= own)
	{
		const std::vector<std::string> names = slangimport::imported();
		const int index = preset - own;
		if (index >= (int)names.size())
			return nullptr;
		if (names[(size_t)index] != l.importedName)
		{
			chain::destroy(l.imported);
			l.imported = nullptr;
			l.importedName = names[(size_t)index];
			l.importedTried = false;
		}
		if (!l.importedTried)
		{
			l.importedTried = true;
			l.imported = chain::load(slangimport::folder(l.importedName));
		}
		if (l.imported == nullptr)
			return nullptr;
		chain::setContentRate((float)host::coreFps());
		const VkImageView out = chain::run(l.imported, g_vulkan_context->GetCurrentCommandBuffer(), view,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, width, height, outWidth, outHeight);
		if (out == VK_NULL_HANDLE)
			return nullptr;
		if (out != l.crtView)
		{
			retire(l.crtSet);
			l.crtSet = ImGui_ImplVulkan_AddTexture(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			l.crtView = out;
		}
		return (void *)l.crtSet;
	}
	if (!l.crtTried)
	{
		l.crtTried = true;
		l.crt = chain::load(appDir + "assets/shaders/crt-guest-advanced");
	}
	if (l.crt == nullptr)
		return nullptr;
	// The mask is drawn in the screen's own pixels: on a 4K screen, twice as wide.
	const int maskSize = outHeight >= 1600 ? 2 : 1;
	if (preset != l.crtPreset || maskSize != l.crtMaskSize)
	{
		chain::resetParameters(l.crt);
		chain::setParameter(l.crt, "masksize", (float)maskSize);
		const std::vector<CrtPreset>& presets = crtPresets();
		for (const auto& [name, value] : presets[std::clamp(preset, 0, (int)presets.size() - 1)].values)
			chain::setParameter(l.crt, name, value);
		l.crtPreset = preset;
		l.crtMaskSize = maskSize;
	}
	const VkImageView out = chain::run(l.crt, g_vulkan_context->GetCurrentCommandBuffer(), view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, width, height, outWidth, outHeight);
	if (out == VK_NULL_HANDLE)
		return nullptr;
	if (out != l.crtView)
	{
		retire(l.crtSet);
		l.crtSet = ImGui_ImplVulkan_AddTexture(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		l.crtView = out;
	}
	return (void *)l.crtSet;
}

void lookShutdown()
{
	LookState& l = look;
	VkDevice device = g_vulkan_context->GetDevice();
	chain::destroy(l.crt);
	chain::destroy(l.imported);
	retire(l.crtSet);
	retire(l.nisOutSet);
	for (Target *target : { &l.staged, &l.stretched, &l.sharpened, &l.before })
		destroyTarget(*target);
	for (VkPipeline pipeline : { l.signal, l.scale, l.cas, l.nis })
		if (pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(device, pipeline, nullptr);
	for (VkPipelineLayout layout : { l.layout, l.nisLayout })
		if (layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(device, layout, nullptr);
	if (l.nisPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(device, l.nisPool, nullptr);
	if (l.nisSetLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(device, l.nisSetLayout, nullptr);
	l.coefScaler.Destroy(false);
	l.coefUsm.Destroy(false);
	l.nisOut.Destroy(false);
	l.coefStaging.Destroy(false);
	l.nisUniforms.Destroy(false);
	l = LookState();
}
}

std::vector<std::string> crtPresetNames()
{
	std::vector<std::string> names;
	for (const CrtPreset& preset : crtPresets())
		names.push_back(preset.name);
	// Then the presets brought in from a USB drive.
	for (const std::string& name : slangimport::imported())
		names.push_back(name);
	return names;
}

bool Look::plain() const
{
	return scaler == 0 && signal == 0 && crt == 0 && brightness == 1.f && contrast == 1.f && saturation == 1.f && gamma == 1.f;
}

void forgetPicture()
{
	if (look.crt != nullptr)
		chain::forget(look.crt);
	if (look.imported != nullptr)
		chain::forget(look.imported);
}

void *picture(void *texture, int width, int height, float u, float v, int outWidth, int outHeight, const Look& want, bool& full)
{
	full = false;
	if (texture == nullptr || !frameOpen || want.plain() || width < 8 || height < 8 || width > 8192 || height > 8192 || u <= 0
			|| v <= 0 || outWidth < 8 || outHeight < 8 || outWidth > 8192 || outHeight > 8192 || !lookInit())
		return nullptr;
	LookState& l = look;
	VkImageView view = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (!viewBehind(texture, view, layout))
		return nullptr;
	// The picture at its own size: its part of the texture, the signal, the
	// colours.
	if (!ensureTarget(l.staged, width, height, swapFormat, shaderPass))
		return nullptr;
	LookConstants c{};
	c.a[0] = (float)width;
	c.a[1] = (float)height;
	c.a[2] = u;
	c.a[3] = v;
	c.b[0] = (float)std::clamp(want.signal, 0, 3);
	c.b[1] = (float)std::max(want.cell, 1);
	c.b[2] = (float)(l.frame++ & 1);
	// The colour carrier's cycles a pixel: 3.58 MHz over the dot clock, which
	// is about 0.021 MHz for each pixel of a line's width.
	const float lineWidth = (float)width / (float)std::max(want.cell, 1);
	c.b[3] = 3.579545f / std::max(lineWidth * 0.020975f, 1.f);
	c.c[0] = want.brightness;
	c.c[1] = want.contrast;
	c.c[2] = want.saturation;
	c.c[3] = std::max(want.gamma, 0.1f);
	c.d[0] = 0.05f;
	fsrPass(l.staged, l.signal, l.layout, view, layout, &c, sizeof(c));
	const VkImageView staged = l.staged.texture.GetView();
	const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	std::string said;
	void *result = nullptr;
	if (want.crt > 0)
	{
		result = crtRun(staged, width, height, outWidth, outHeight, want.crt - 1);
		said = "crt-guest-advanced";
	}
	else if (want.scaler != 0 && (width < outWidth || height < outHeight))
	{
		auto stretch = [&](Target& target, int w, int h, bool sharp) {
			if (!ensureTarget(target, w, h, swapFormat, shaderPass))
				return false;
			LookConstants s{};
			s.a[0] = (float)width;
			s.a[1] = (float)height;
			s.a[2] = s.a[3] = 1.f;
			s.b[0] = (float)w;
			s.b[1] = (float)h;
			s.b[2] = sharp ? 1.f : 0.f;
			fsrPass(target, l.scale, l.layout, staged, readable, &s, sizeof(s));
			return true;
		};
		switch (want.scaler)
		{
		case 1:
			if (stretch(l.stretched, outWidth, outHeight, true))
				result = (void *)l.stretched.set;
			said = "sharp bilinear";
			break;
		case 2:
			result = upscaleView(staged, readable, width, height, 1.f, 1.f, outWidth, outHeight, want.sharpness);
			said = "FSR 1";
			break;
		case 3:
		{
			// NIS grows a picture at most twice each way: a larger step starts
			// from the picture made half the screen's size, sharp bilinear.
			VkImageView from = staged;
			int fromW = width, fromH = height;
			if (outWidth > width * 2 || outHeight > height * 2)
			{
				fromW = (outWidth + 1) / 2;
				fromH = (outHeight + 1) / 2;
				if (!stretch(l.before, fromW, fromH, true))
					break;
				from = l.before.texture.GetView();
			}
			result = nisScale(from, fromW, fromH, outWidth, outHeight, want.sharpness);
			said = "NIS";
			break;
		}
		case 4:
			if (stretch(l.stretched, outWidth, outHeight, false) && ensureTarget(l.sharpened, outWidth, outHeight, swapFormat, shaderPass))
			{
				static const float strength[3] = { 0.2f, 0.6f, 1.f };
				const float sharp = -1.f / (8.f + (5.f - 8.f) * strength[std::clamp(want.sharpness, 0, 2)]);
				uint32_t k[8] = { floatBits(1.f), floatBits(1.f), floatBits(0.f), floatBits(0.f), floatBits(sharp),
					(uint32_t)halfBits(sharp), floatBits(8.f), 0 };
				fsrPass(l.sharpened, l.cas, l.layout, l.stretched.texture.GetView(), readable, k, sizeof(k));
				result = (void *)l.sharpened.set;
			}
			said = "CAS";
			break;
		}
	}
	if (result != nullptr)
		full = true;
	else
	{
		// Only the signal and the colours: ImGui stretches the picture.
		result = (void *)l.staged.set;
		said = said.empty() ? "colours and signal" : said + " refused: colours and signal";
	}
	const std::string line = format("look: %s, %d x %d to %d x %d", said.c_str(), width, height, outWidth, outHeight);
	if (line != l.said)
	{
		diag::mark("%s", line.c_str());
		l.said = line;
	}
	return result;
}

bool capture(void *texture, float u, float v, int width, int height, std::vector<uint8_t>& rgba)
{
	rgba.clear();
	if (texture == nullptr || !frameOpen || width < 8 || height < 8 || width > 8192 || height > 8192
			|| !ensureTarget(captureTarget, width, height))
		return false;
	drawInto(captureTarget, texture, u, v, 1.f, 0, 0);
	VkDevice device = g_vulkan_context->GetDevice();
	const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	bool coherent = false;
	if (!Vulkan::StagingBuffer::AllocateBuffer(Vulkan::StagingBuffer::Type::Readback, bytes,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffer, &memory, &coherent))
		return false;
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	captureTarget.texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
	vkCmdCopyImageToBuffer(cmd, captureTarget.texture.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	captureTarget.texture.TransitionToLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	// Everything so far is done, and waited for: the copy is in the buffer.
	g_vulkan_context->ExecuteCommandBuffer(true);
	bool ok = false;
	void *mapped = nullptr;
	if (vkMapMemory(device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS)
	{
		rgba.resize((size_t)bytes);
		const uint8_t *in = static_cast<const uint8_t *>(mapped);
		const bool bgr = swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
		for (size_t i = 0; i < (size_t)width * height; i++)
		{
			rgba[i * 4 + 0] = in[i * 4 + (bgr ? 2 : 0)];
			rgba[i * 4 + 1] = in[i * 4 + 1];
			rgba[i * 4 + 2] = in[i * 4 + (bgr ? 0 : 2)];
			rgba[i * 4 + 3] = 255;
		}
		vkUnmapMemory(device, memory);
		ok = true;
	}
	vkDestroyBuffer(device, buffer, nullptr);
	vkFreeMemory(device, memory, nullptr);
	return ok;
}

void *ambient(void *texture, float u, float v)
{
	// A few pixels: stretched over the screen they are only colours.
	constexpr int Width = 32, Height = 24;
	if (texture == nullptr || !frameOpen || !ensureTarget(ambientTarget, Width, Height))
		return nullptr;
	// Each frame a little of the picture is blended in, sampled a little
	// elsewhere each time: over half a second that is its average, which a
	// single sample of a large picture is not.
	static unsigned turn;
	turn++;
	const float shiftU = (((turn * 7u) % 16u) / 16.f - 0.5f) * u / Width;
	const float shiftV = (((turn * 11u) % 16u) / 16.f - 0.5f) * v / Height;
	drawInto(ambientTarget, texture, u, v, 0.10f, shiftU, shiftV);
	return (void *)ambientTarget.set;
}

void forgetAmbient()
{
	ambientTarget.drawn = false;
}

bool writePng(const std::string& path, const uint8_t *rgba, int width, int height)
{
	if (rgba == nullptr || width <= 0 || height <= 0)
		return false;
	std::vector<uint8_t> rgb((size_t)width * height * 3);
	for (size_t i = 0; i < (size_t)width * height; i++)
	{
		rgb[i * 3 + 0] = rgba[i * 4 + 0];
		rgb[i * 3 + 1] = rgba[i * 4 + 1];
		rgb[i * 3 + 2] = rgba[i * 4 + 2];
	}
	size_t pngBytes = 0;
	void *png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), width, height, 3, &pngBytes, 3, MZ_FALSE);
	if (png == nullptr)
		return false;
	const bool ok = writeFile(path, png, pngBytes);
	mz_free(png);
	return ok;
}

// The image presented last, read back and written as a PNG.
bool saveScreenshot(const std::string& path)
{
	if (!canReadBack || images.empty() || frames == 0)
		return false;
	VkDevice device = g_vulkan_context->GetDevice();
	g_vulkan_context->ExecuteCommandBuffer(true);
	const VkDeviceSize bytes = (VkDeviceSize)extent.width * extent.height * 4;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	bool coherent = false;
	if (!Vulkan::StagingBuffer::AllocateBuffer(Vulkan::StagingBuffer::Type::Readback, bytes,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffer, &memory, &coherent))
		return false;
	VkCommandBuffer cmd = g_vulkan_context->GetCurrentCommandBuffer();
	const VkImage image = images[imageIndex];
	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			nullptr, 1, &barrier);
	VkBufferImageCopy region{};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { extent.width, extent.height, 1 };
	vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = 0;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
			nullptr, 1, &barrier);
	g_vulkan_context->ExecuteCommandBuffer(true);

	bool ok = false;
	void *mapped = nullptr;
	if (vkMapMemory(device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS)
	{
		std::vector<uint8_t> rgb((size_t)extent.width * extent.height * 3);
		const uint8_t *in = static_cast<const uint8_t *>(mapped);
		const bool bgr = swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
		for (size_t i = 0; i < (size_t)extent.width * extent.height; i++)
		{
			rgb[i * 3 + 0] = in[i * 4 + (bgr ? 2 : 0)];
			rgb[i * 3 + 1] = in[i * 4 + 1];
			rgb[i * 3 + 2] = in[i * 4 + (bgr ? 0 : 2)];
		}
		vkUnmapMemory(device, memory);
		size_t pngBytes = 0;
		void *png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), (int)extent.width, (int)extent.height, 3,
				&pngBytes, 6, MZ_FALSE);
		if (png != nullptr)
		{
			ok = writeFile(path, png, pngBytes);
			mz_free(png);
		}
	}
	vkDestroyBuffer(device, buffer, nullptr);
	vkFreeMemory(device, memory, nullptr);
	return ok;
}

}
