// Mupen64Plus PS5: the Vulkan self-test (ProsperoVulkan.h).
//
// No Vulkan loader: every command comes from the RADV archive's vk_icdGetInstanceProcAddr, as in
// PS5_Vulkan's radv/radv_smoke.c, which this follows. A GPU fault ends the process, so each step is logged
// before it runs and the host-memory check, the one that can fault, comes last.
//
// SPDX-License-Identifier: MIT

#include "ProsperoVulkan.h"

#include "OrbisPaths.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "probe_comp.h" // the compute shader (shaders/probe.comp) as SPIR-V, made by glslangValidator

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

namespace ps5vulkan
{
namespace
{
#define INSTANCE_COMMANDS(X)                                                                                         \
	X(DestroyInstance)                                                                                               \
	X(EnumeratePhysicalDevices)                                                                                      \
	X(GetPhysicalDeviceProperties2)                                                                                  \
	X(GetPhysicalDeviceFeatures2)                                                                                    \
	X(GetPhysicalDeviceQueueFamilyProperties)                                                                        \
	X(GetPhysicalDeviceMemoryProperties)                                                                             \
	X(EnumerateDeviceExtensionProperties)                                                                            \
	X(CreateDevice)                                                                                                  \
	X(GetDeviceProcAddr)

#define DEVICE_COMMANDS(X)                                                                                           \
	X(DestroyDevice)                                                                                                 \
	X(GetDeviceQueue)                                                                                                \
	X(DeviceWaitIdle)                                                                                                \
	X(CreateBuffer)                                                                                                  \
	X(DestroyBuffer)                                                                                                 \
	X(GetBufferMemoryRequirements)                                                                                   \
	X(AllocateMemory)                                                                                                \
	X(FreeMemory)                                                                                                    \
	X(BindBufferMemory)                                                                                              \
	X(MapMemory)                                                                                                     \
	X(UnmapMemory)                                                                                                   \
	X(CreateShaderModule)                                                                                            \
	X(DestroyShaderModule)                                                                                           \
	X(CreateDescriptorSetLayout)                                                                                     \
	X(DestroyDescriptorSetLayout)                                                                                    \
	X(CreatePipelineLayout)                                                                                          \
	X(DestroyPipelineLayout)                                                                                         \
	X(CreateComputePipelines)                                                                                        \
	X(DestroyPipeline)                                                                                               \
	X(CreateDescriptorPool)                                                                                          \
	X(DestroyDescriptorPool)                                                                                         \
	X(AllocateDescriptorSets)                                                                                        \
	X(UpdateDescriptorSets)                                                                                          \
	X(CreateCommandPool)                                                                                             \
	X(DestroyCommandPool)                                                                                            \
	X(AllocateCommandBuffers)                                                                                        \
	X(BeginCommandBuffer)                                                                                            \
	X(EndCommandBuffer)                                                                                              \
	X(ResetCommandBuffer)                                                                                            \
	X(CmdBindPipeline)                                                                                               \
	X(CmdBindDescriptorSets)                                                                                         \
	X(CmdPushConstants)                                                                                              \
	X(CmdDispatch)                                                                                                   \
	X(CmdPipelineBarrier)                                                                                            \
	X(CreateFence)                                                                                                   \
	X(DestroyFence)                                                                                                  \
	X(ResetFences)                                                                                                   \
	X(WaitForFences)                                                                                                 \
	X(QueueSubmit)

#define DECLARE(name) PFN_vk##name vk##name;
INSTANCE_COMMANDS(DECLARE)
DEVICE_COMMANDS(DECLARE)
#undef DECLARE
PFN_vkCreateInstance vkCreateInstance;
PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT;

double Ms(const timespec& from)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - from.tv_sec) * 1e3 + (now.tv_nsec - from.tv_nsec) / 1e6;
}

timespec Now()
{
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
{
	for (const VkExtensionProperties& e : list)
		if (strcmp(e.extensionName, name) == 0)
			return true;
	return false;
}

const char* Yes(bool b)
{
	return b ? "yes" : "NO";
}

struct Context
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = UINT32_MAX;
	VkPhysicalDeviceMemoryProperties memory = {};
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkDescriptorPool descriptors = VK_NULL_HANDLE;
	bool host_memory_ext = false;
	VkDeviceSize host_alignment = 0;
};

uint32_t MemoryType(const Context& c, uint32_t bits, VkMemoryPropertyFlags wanted)
{
	for (uint32_t i = 0; i < c.memory.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (c.memory.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	return UINT32_MAX;
}

bool LoadInstance(VkInstance instance)
{
	bool ok = true;
#define LOAD(name)                                                                                                   \
	vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name);                                        \
	if (!vk##name)                                                                                                   \
	{                                                                                                                \
		OrbisLog("[vulkan] missing vk" #name);                                                                       \
		ok = false;                                                                                                  \
	}
	INSTANCE_COMMANDS(LOAD)
#undef LOAD
	return ok;
}

bool LoadDevice(VkDevice device)
{
	bool ok = true;
#define LOAD(name)                                                                                                   \
	vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);                                                \
	if (!vk##name)                                                                                                   \
	{                                                                                                                \
		OrbisLog("[vulkan] missing vk" #name);                                                                       \
		ok = false;                                                                                                  \
	}
	DEVICE_COMMANDS(LOAD)
#undef LOAD
	return ok;
}

// The instance, the GPU, a device with one graphics+compute queue; logs what ParaLLEl-RDP looks for.
bool CreateDevice(Context& c, ProbeResult& r)
{
	vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	if (!vkCreateInstance)
	{
		OrbisLog("[vulkan] the driver has no vkCreateInstance");
		return false;
	}
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "Mupen64Plus PS5";
	app.apiVersion = VK_API_VERSION_1_1; // what ParaLLEl-RDP asks for
	VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance_info.pApplicationInfo = &app;
	VkResult res = vkCreateInstance(&instance_info, nullptr, &c.instance);
	OrbisLog("[vulkan] vkCreateInstance -> %d", int(res));
	if (res != VK_SUCCESS || !LoadInstance(c.instance))
		return false;

	uint32_t count = 1;
	res = vkEnumeratePhysicalDevices(c.instance, &count, &c.physical);
	if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || count == 0)
	{
		OrbisLog("[vulkan] no GPU (vkEnumeratePhysicalDevices -> %d, %u)", int(res), count);
		return false;
	}

	VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
	VkPhysicalDeviceSubgroupProperties subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
	subgroup.pNext = &host_props;
	VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	driver.pNext = &subgroup;
	VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	props.pNext = &driver;
	vkGetPhysicalDeviceProperties2(c.physical, &props);
	const uint32_t api = props.properties.apiVersion;
	char gpu[256];
	snprintf(gpu, sizeof(gpu), "%s (%s %s), Vulkan %u.%u.%u", props.properties.deviceName, driver.driverName,
		driver.driverInfo, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
	r.gpu = gpu;
	OrbisLog("[vulkan] GPU: %s", gpu);
	OrbisLog("[vulkan] subgroups: size %u, stages 0x%x, operations 0x%x (ballot %s, arithmetic %s, shuffle %s)",
		subgroup.subgroupSize, unsigned(subgroup.supportedStages), unsigned(subgroup.supportedOperations),
		Yes(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT),
		Yes(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT),
		Yes(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT));
	OrbisLog("[vulkan] limits: max compute workgroup %u invocations, storage buffer range %u MiB, "
		"max image 2D %u", props.properties.limits.maxComputeWorkGroupInvocations,
		props.properties.limits.maxStorageBufferRange >> 20, props.properties.limits.maxImageDimension2D);

	vkGetPhysicalDeviceMemoryProperties(c.physical, &c.memory);
	for (uint32_t i = 0; i < c.memory.memoryHeapCount; i++)
		OrbisLog("[vulkan] memory heap %u: %llu MiB, flags 0x%x", i,
			(unsigned long long)(c.memory.memoryHeaps[i].size >> 20), unsigned(c.memory.memoryHeaps[i].flags));
	for (uint32_t i = 0; i < c.memory.memoryTypeCount; i++)
		OrbisLog("[vulkan] memory type %u: heap %u, flags 0x%x", i, c.memory.memoryTypes[i].heapIndex,
			unsigned(c.memory.memoryTypes[i].propertyFlags));

	count = 0;
	vkEnumerateDeviceExtensionProperties(c.physical, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	vkEnumerateDeviceExtensionProperties(c.physical, nullptr, &count, extensions.data());
	extensions.resize(count);
	c.host_memory_ext = HasExtension(extensions, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
	c.host_alignment = host_props.minImportedHostPointerAlignment;
	OrbisLog("[vulkan] %u device extensions; VK_EXT_external_memory_host %s (alignment %llu), "
		"VK_KHR_8bit_storage %s, VK_KHR_16bit_storage %s, VK_KHR_timeline_semaphore %s, VK_KHR_swapchain %s",
		count, Yes(c.host_memory_ext), (unsigned long long)c.host_alignment,
		Yes(HasExtension(extensions, VK_KHR_8BIT_STORAGE_EXTENSION_NAME)),
		Yes(HasExtension(extensions, VK_KHR_16BIT_STORAGE_EXTENSION_NAME)),
		Yes(HasExtension(extensions, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)),
		Yes(HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)));

	// The features ParaLLEl-RDP's shaders use (8/16-bit storage and arithmetic, timeline semaphores for its
	// frame pacing, storage images written without a format).
	VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	f11.pNext = &f12;
	VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	features.pNext = &f11;
	vkGetPhysicalDeviceFeatures2(c.physical, &features);
	OrbisLog("[vulkan] features: storageBuffer8BitAccess %s, storageBuffer16BitAccess %s, shaderInt8 %s, "
		"shaderInt16 %s, shaderFloat16 %s, timelineSemaphore %s, shaderStorageImageWriteWithoutFormat %s",
		Yes(f12.storageBuffer8BitAccess), Yes(f11.storageBuffer16BitAccess), Yes(f12.shaderInt8),
		Yes(features.features.shaderInt16), Yes(f12.shaderFloat16), Yes(f12.timelineSemaphore),
		Yes(features.features.shaderStorageImageWriteWithoutFormat));
	r.features = f12.storageBuffer8BitAccess && f11.storageBuffer16BitAccess && f12.shaderInt8 &&
		features.features.shaderInt16 && f12.timelineSemaphore &&
		(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) && c.host_memory_ext;

	VkQueueFamilyProperties families[8];
	count = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &count, families);
	for (uint32_t i = 0; i < count; i++)
	{
		OrbisLog("[vulkan] queue family %u: %u queues, flags 0x%x", i, families[i].queueCount,
			unsigned(families[i].queueFlags));
		const VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
		if (c.family == UINT32_MAX && (families[i].queueFlags & want) == want)
			c.family = i;
	}
	if (c.family == UINT32_MAX)
	{
		OrbisLog("[vulkan] no graphics+compute queue");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue_info.queueFamilyIndex = c.family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	const char* enable[2];
	uint32_t enabled = 0;
	if (c.host_memory_ext)
	{
		enable[enabled++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
		enable[enabled++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
	}
	VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = enabled;
	device_info.ppEnabledExtensionNames = enable;
	res = vkCreateDevice(c.physical, &device_info, nullptr, &c.device);
	OrbisLog("[vulkan] vkCreateDevice -> %d", int(res));
	if (res != VK_SUCCESS || !LoadDevice(c.device))
		return false;
	vkGetMemoryHostPointerPropertiesEXT =
		(PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(c.device, "vkGetMemoryHostPointerPropertiesEXT");
	vkGetDeviceQueue(c.device, c.family, 0, &c.queue);

	VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = c.family;
	VkCommandBufferAllocateInfo cmd_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cmd_info.commandBufferCount = 1;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vkCreateCommandPool(c.device, &pool_info, nullptr, &c.pool) != VK_SUCCESS)
		return false;
	cmd_info.commandPool = c.pool;
	return vkAllocateCommandBuffers(c.device, &cmd_info, &c.cmd) == VK_SUCCESS &&
		vkCreateFence(c.device, &fence_info, nullptr, &c.fence) == VK_SUCCESS;
}

// The compute pipeline: one storage buffer, v[i] = i * 2654435761 + seed (shaders/probe.comp).
bool CreatePipeline(Context& c)
{
	VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module_info.codeSize = sizeof(kProbeComp);
	module_info.pCode = kProbeComp;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(c.device, &module_info, nullptr, &module) != VK_SUCCESS)
		return false;

	VkDescriptorSetLayoutBinding binding = {};
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	set_info.bindingCount = 1;
	set_info.pBindings = &binding;
	VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
	VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &c.set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName = "main";
	VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
	VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pool_info.maxSets = 4;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;

	const timespec start = Now();
	bool ok = vkCreateDescriptorSetLayout(c.device, &set_info, nullptr, &c.set_layout) == VK_SUCCESS &&
		vkCreatePipelineLayout(c.device, &layout_info, nullptr, &c.layout) == VK_SUCCESS;
	pipeline_info.layout = c.layout;
	ok = ok && vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &c.pipeline) == VK_SUCCESS;
	OrbisLog("[vulkan] compute pipeline: %s, compiled in %.1f ms", ok ? "created" : "FAILED", Ms(start));
	vkDestroyShaderModule(c.device, module, nullptr);
	return ok && vkCreateDescriptorPool(c.device, &pool_info, nullptr, &c.descriptors) == VK_SUCCESS;
}

// Runs the shader over `words` words of `buffer`, waits for it, and returns how long the GPU part took.
bool Dispatch(Context& c, VkBuffer buffer, uint32_t words, uint32_t seed, double* ms)
{
	VkDescriptorSetAllocateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	set_info.descriptorPool = c.descriptors;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts = &c.set_layout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(c.device, &set_info, &set) != VK_SUCCESS)
		return false;
	VkDescriptorBufferInfo buffer_info = {buffer, 0, VkDeviceSize(words) * 4};
	VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = set;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &buffer_info;
	vkUpdateDescriptorSets(c.device, 1, &write, 0, nullptr);

	VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(c.cmd, 0);
	vkBeginCommandBuffer(c.cmd, &begin);
	vkCmdBindPipeline(c.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
	vkCmdBindDescriptorSets(c.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(c.cmd, c.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &seed);
	vkCmdDispatch(c.cmd, words / 64, 1, 1);
	VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
		nullptr, 0, nullptr);
	vkEndCommandBuffer(c.cmd);

	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &c.cmd;
	vkResetFences(c.device, 1, &c.fence);
	const timespec start = Now();
	VkResult res = vkQueueSubmit(c.queue, 1, &submit, c.fence);
	if (res == VK_SUCCESS)
		res = vkWaitForFences(c.device, 1, &c.fence, VK_TRUE, 2000000000ull);
	*ms = Ms(start);
	if (res != VK_SUCCESS)
		OrbisLog("[vulkan] submit/wait -> %d after %.1f ms", int(res), *ms);
	return res == VK_SUCCESS;
}

uint32_t Wrong(const volatile uint32_t* v, uint32_t words, uint32_t seed)
{
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < words; i++)
		wrong += v[i] != i * 2654435761u + seed;
	return wrong;
}

// A host-visible buffer the driver allocates, written by the shader, read back by the CPU.
bool TestCompute(Context& c)
{
	const uint32_t words = 1u << 20; // 4 MiB, the size of the N64's RDRAM
	VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.size = VkDeviceSize(words) * 4;
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(c.device, &info, nullptr, &buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(c.device, buffer, &req);
	VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = MemoryType(c, req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* map = nullptr;
	bool ok = alloc.memoryTypeIndex != UINT32_MAX &&
		vkAllocateMemory(c.device, &alloc, nullptr, &memory) == VK_SUCCESS &&
		vkBindBufferMemory(c.device, buffer, memory, 0) == VK_SUCCESS &&
		vkMapMemory(c.device, memory, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS;
	double ms = 0;
	uint32_t wrong = words;
	if (ok)
	{
		memset(map, 0, info.size);
		OrbisLog("[vulkan] compute: running the shader over %u words", words);
		ok = Dispatch(c, buffer, words, 0x1234u, &ms);
		if (ok)
			wrong = Wrong(static_cast<const uint32_t*>(map), words, 0x1234u);
	}
	OrbisLog("[vulkan] compute: %s (%u of %u words wrong, %.2f ms)", ok && wrong == 0 ? "passed" : "FAILED", wrong,
		words, ms);
	if (map)
		vkUnmapMemory(c.device, memory);
	vkDestroyBuffer(c.device, buffer, nullptr);
	if (memory)
		vkFreeMemory(c.device, memory, nullptr);
	return ok && wrong == 0;
}

// Memory the app allocated the way mupen64plus-core allocates RDRAM (posix_memalign), imported into Vulkan
// (VK_EXT_external_memory_host) and written by the shader: ParaLLEl-RDP works on RDRAM in place this way.
bool TestHostMemory(Context& c)
{
	if (!c.host_memory_ext || !vkGetMemoryHostPointerPropertiesEXT)
	{
		OrbisLog("[vulkan] host memory: VK_EXT_external_memory_host missing, skipped");
		return false;
	}
	const uint32_t words = 1u << 20;
	const size_t bytes = size_t(words) * 4;
	const size_t align = c.host_alignment > 4096 ? size_t(c.host_alignment) : 4096;
	void* host = nullptr;
	if (posix_memalign(&host, align, bytes) != 0)
	{
		OrbisLog("[vulkan] host memory: posix_memalign(%zu, %zu) failed", align, bytes);
		return false;
	}
	memset(host, 0, bytes);
	VkMemoryHostPointerPropertiesEXT host_props = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
	VkResult res = vkGetMemoryHostPointerPropertiesEXT(c.device,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &host_props);
	OrbisLog("[vulkan] host memory: %p (%zu bytes), vkGetMemoryHostPointerPropertiesEXT -> %d, types 0x%x", host,
		bytes, int(res), unsigned(host_props.memoryTypeBits));

	VkExternalMemoryBufferCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.pNext = &external;
	info.size = bytes;
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	bool ok = res == VK_SUCCESS && host_props.memoryTypeBits != 0 &&
		vkCreateBuffer(c.device, &info, nullptr, &buffer) == VK_SUCCESS;
	if (ok)
	{
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(c.device, buffer, &req);
		VkImportMemoryHostPointerInfoEXT import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
		import.pHostPointer = host;
		VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.pNext = &import;
		alloc.allocationSize = bytes;
		alloc.memoryTypeIndex = MemoryType(c, req.memoryTypeBits & host_props.memoryTypeBits, 0);
		res = alloc.memoryTypeIndex == UINT32_MAX ? VK_ERROR_UNKNOWN :
			vkAllocateMemory(c.device, &alloc, nullptr, &memory);
		OrbisLog("[vulkan] host memory: import (type %u) -> %d", alloc.memoryTypeIndex, int(res));
		ok = res == VK_SUCCESS && vkBindBufferMemory(c.device, buffer, memory, 0) == VK_SUCCESS;
	}
	double ms = 0;
	uint32_t wrong = words;
	if (ok)
	{
		OrbisLog("[vulkan] host memory: the shader writes it (a GPU fault would end the app here)");
		ok = Dispatch(c, buffer, words, 0x5a5au, &ms);
		if (ok)
			wrong = Wrong(static_cast<const uint32_t*>(host), words, 0x5a5au);
	}
	OrbisLog("[vulkan] host memory: %s (%u of %u words wrong, %.2f ms)", ok && wrong == 0 ? "passed" : "FAILED",
		wrong, words, ms);
	if (buffer)
		vkDestroyBuffer(c.device, buffer, nullptr);
	if (memory)
		vkFreeMemory(c.device, memory, nullptr);
	free(host);
	return ok && wrong == 0;
}

void Destroy(Context& c)
{
	if (c.device)
	{
		vkDeviceWaitIdle(c.device);
		if (c.descriptors)
			vkDestroyDescriptorPool(c.device, c.descriptors, nullptr);
		if (c.pipeline)
			vkDestroyPipeline(c.device, c.pipeline, nullptr);
		if (c.layout)
			vkDestroyPipelineLayout(c.device, c.layout, nullptr);
		if (c.set_layout)
			vkDestroyDescriptorSetLayout(c.device, c.set_layout, nullptr);
		if (c.fence)
			vkDestroyFence(c.device, c.fence, nullptr);
		if (c.pool)
			vkDestroyCommandPool(c.device, c.pool, nullptr);
		vkDestroyDevice(c.device, nullptr);
	}
	if (c.instance)
		vkDestroyInstance(c.instance, nullptr);
}
} // namespace

ProbeResult Probe()
{
	ProbeResult r;
	// RADV keeps compiled shaders on disk; not for a test.
	setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
	const timespec start = Now();
	OrbisLog("[vulkan] self-test starts");
	Context c;
	r.device = CreateDevice(c, r);
	OrbisLog("[vulkan] device %s in %.1f ms", r.device ? "ready" : "FAILED", Ms(start));
	if (r.device && CreatePipeline(c))
	{
		r.compute = TestCompute(c);
		if (r.compute)
			r.host_memory = TestHostMemory(c);
	}
	Destroy(c);
	OrbisLog("[vulkan] self-test ends in %.1f ms: device %s, compute %s, RDRAM sharing %s, features %s",
		Ms(start), Yes(r.device), Yes(r.compute), Yes(r.host_memory), Yes(r.features));

	auto line = [](const char* what, bool ok) { return std::string(what) + (ok ? ": passed\n" : ": FAILED\n"); };
	r.summary = (r.gpu.empty() ? std::string("GPU: not found\n") : "GPU: " + r.gpu + "\n") +
		line("Vulkan device", r.device) + line("Compute shader", r.compute) +
		line("GPU access to N64 memory", r.host_memory) + line("Features ParaLLEl-RDP needs", r.features);
	return r;
}
} // namespace ps5vulkan
