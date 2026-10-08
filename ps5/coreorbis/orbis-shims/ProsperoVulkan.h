// Mupen64Plus PS5: the Vulkan self-test (GitHub issue #1, internal resolution: the first step towards a GPU
// renderer, ParaLLEl-RDP).
//
// Built only with `make VULKAN=1`: the app then links RADV, Mesa's Vulkan driver for AMD GPUs, as
// mihawk-99/PS5_Vulkan builds it for the console (tools/build-radv.sh release), and runs Probe() once the
// video is up. The probe asks for what ParaLLEl-RDP needs and logs each answer ("[vulkan] ..." in boot.log):
//   - an instance, the GPU and a device with a compute queue;
//   - a compute shader run over a buffer and read back on the CPU;
//   - the GPU writing memory the app allocated itself (VK_EXT_external_memory_host): ParaLLEl-RDP reads and
//     writes the N64's RDRAM in place this way;
//   - the features its shaders use (8- and 16-bit storage, subgroup operations, timeline semaphores).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace ps5vulkan
{
struct ProbeResult
{
	bool device = false;    // instance and device created
	bool compute = false;   // the compute shader's output read back right
	bool host_memory = false; // the GPU wrote memory the app allocated (RDRAM in place)
	bool features = false;  // every feature ParaLLEl-RDP needs is there
	std::string gpu;        // "AMD ... (RADV ...), Vulkan 1.4.x"
	std::string summary;    // a few lines for the screen
};

ProbeResult Probe();
} // namespace ps5vulkan
