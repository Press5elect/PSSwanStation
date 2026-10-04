/*
	SwanStation for PS5 - the display: Vulkan on RADV, the swapchain, Dear ImGui.

	SPDX-License-Identifier: GPL-3.0-or-later

	The console has no Vulkan loader and no window system. RADV (PS5_Vulkan /
	PS5_Mesa) is linked into the title; its ICD entry point resolves every
	command (dep/vulkan-loader), and the picture goes to a VK_KHR_display plane
	surface on the console's one display, as PS5_Vulkan's own titles and
	PSFlyCast drive it: the display's 4K mode with the highest refresh rate the
	driver offers (59.94 Hz here: param.json does not ask for 119.88), a FIFO
	swapchain, B8G8R8A8.

	The device is the emulator's own Vulkan context (src/common/vulkan/context),
	made here once and kept for the whole run: the interface draws with it, and
	the emulator's hardware renderer finds it there when a game starts
	(host.cpp answers the core's libretro Vulkan interface from it). A frame is
	the emulator's commands, submitted by the emulator, then one render pass of
	ImGui into the swapchain image: the game's picture is an ImGui image under
	the menus.

	On a PC (the host build) the surface is VK_EXT_headless_surface and frames
	can be saved as PNGs: that is how the interface is checked without a console.
*/
#include "display.h"
#include "fe.h"

#include "common/vulkan/context.h"
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
VkSampler linearSampler = VK_NULL_HANDLE, nearestSampler = VK_NULL_HANDLE;
bool gameLinear = true;
std::string gpuName;

// The emulator's picture, wrapped for ImGui.
VkImageView wrappedView = VK_NULL_HANDLE;
VkDescriptorSet wrappedSet = VK_NULL_HANDLE;
bool wrappedLinear = true;
int wrappedLayout = 0;
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
	app.apiVersion = VK_API_VERSION_1_0;
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

VkSampler makeSampler(VkFilter filter)
{
	VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	info.magFilter = filter;
	info.minFilter = filter;
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.maxLod = 1.0f;
	info.maxAnisotropy = 1.0f;
	VkSampler sampler = VK_NULL_HANDLE;
	vkCreateSampler(g_vulkan_context->GetDevice(), &info, nullptr, &sampler);
	return sampler;
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
	info.RenderPass = renderPass;
	info.MinImageCount = (uint32_t)images.size();
	info.ImageCount = (uint32_t)images.size();
	info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
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
	linearSampler = makeSampler(VK_FILTER_LINEAR);
	nearestSampler = makeSampler(VK_FILTER_NEAREST);
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
	vkDestroySampler(device, linearSampler, nullptr);
	vkDestroySampler(device, nearestSampler, nullptr);
	vkDestroyRenderPass(device, renderPass, nullptr);
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
	texture->set = ImGui_ImplVulkan_AddTexture(linearSampler, texture->texture.GetView(),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
	texture->set = ImGui_ImplVulkan_AddTexture(gameLinear ? linearSampler : nearestSampler, texture->texture.GetView(),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
	if (view != wrappedView || wrappedLinear != gameLinear || wrappedLayout != layout)
	{
		retire(wrappedSet);
		wrappedSet = ImGui_ImplVulkan_AddTexture(gameLinear ? linearSampler : nearestSampler, view,
				(VkImageLayout)layout);
		wrappedView = view;
		wrappedLinear = gameLinear;
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

void setLinear(bool linear)
{
	gameLinear = linear;
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
