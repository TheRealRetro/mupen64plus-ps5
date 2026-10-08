// Mupen64Plus PS5: the GPU video plugin, paraLLEl-RDP (third_party/parallel-rdp, Themaister's standalone
// tree, MIT) on Vulkan, for a higher internal resolution (GitHub issue #1). Built only with `make VULKAN=1`,
// which links RADV (coreorbis/orbis-shims/ProsperoVulkan.h).
//
// paraLLEl-RDP runs the RDP's command lists as Vulkan compute shaders on the GPU, working on RDRAM in place
// (VK_EXT_external_memory_host: the core's RDRAM is imported into Vulkan), optionally upscaled 2x/4x/8x. Its
// VI turns the frame buffer into a picture on the GPU too. Here the picture is copied back to the CPU and
// handed to the frontend (n64ps5_on_vi) like angrylion's, so the frontend's scaling, menus and on-screen
// text stay as they are. The command list handling follows mupen64plus-libretro-nx's
// mupen64plus-video-paraLLEl/rdp.cpp.
//
// The Vulkan device is made once (n64ps5_gpu_available) and kept; the command processor lives from RomOpen
// to RomClosed.
//
// SPDX-License-Identifier: MIT

#include "rdp_device.hpp"
#include "context.hpp"
#include "device.hpp"
#include "logging.hpp"

extern "C" {
#include "api/m64p_plugin.h"
#include "api/m64p_types.h"
}

#include "OrbisPaths.h"
#include "n64_bridge.h"
#include "perf.h"
#include "static_dynlib.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

namespace
{
constexpr int kPluginVersion = 0x010000;
constexpr int kApiVersion = 0x020200;
constexpr uint32_t kDpStatusXbusDma = 0x1;
constexpr uint32_t kMiIntrDp = 0x20;

// paraLLEl-RDP's and Granite's messages go to boot.log ("[gpu] ...") on the threads that set this.
class Logger : public Util::LoggingInterface
{
public:
	bool log(const char* tag, const char* fmt, va_list va) override
	{
		char line[512];
		vsnprintf(line, sizeof(line), fmt, va);
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		OrbisLog("[gpu] %s%s", tag, line);
		return true;
	}
};
Logger g_logger;

struct Gpu
{
	bool tried = false;
	std::unique_ptr<Vulkan::Context> context;
	std::unique_ptr<Vulkan::Device> device;
};
Gpu g_gpu;

GFX_INFO l_Gfx;
bool l_Initialized;
bool l_RomOpen;
void (*l_RenderCallback)(int);
n64ps5_gfx_options l_Options = {0, false, 6, 1, 4, true};
std::unique_ptr<RDP::CommandProcessor> l_Rdp;
RDP::VIScanoutBuffer l_Scanout;

// What the GPU costs, logged every 600 VIs (Report): the waits for it (rdtsc ticks) and the GPU's own time,
// from paraLLEl-RDP's timestamps (PARALLEL_RDP_BENCH=2).
struct Stats
{
	uint64_t syncs = 0, sync_ticks = 0, scanout_ticks = 0;
	unsigned vis = 0;
	uint64_t tsc0 = 0;
	timespec t0 = {};
};
Stats l_Stats;

// the RDP command list as the RSP left it (rdp.cpp's process_commands)
int l_CmdCur, l_CmdPtr;
uint32_t l_CmdData[0x00040000 >> 2];
const unsigned kCmdLength[64] = {
	1, 1, 1, 1, 1, 1, 1, 1, 4, 6, 12, 14, 12, 14, 20, 22,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

RDP::CommandProcessorFlags Flags(int upscale)
{
	switch (upscale)
	{
	case 2: return RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT | RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
	case 4: return RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT | RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
	case 8: return RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT | RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
	default: return 0;
	}
}

// ---- the plugin API -------------------------------------------------------------------------------------
m64p_error PluginStartup(m64p_dynlib_handle, void*, void (*)(void*, int, const char*))
{
	if (l_Initialized)
		return M64ERR_ALREADY_INIT;
	l_Initialized = true;
	return M64ERR_SUCCESS;
}

m64p_error PluginShutdown()
{
	if (!l_Initialized)
		return M64ERR_NOT_INIT;
	l_Initialized = false;
	return M64ERR_SUCCESS;
}

m64p_error PluginGetVersion(m64p_plugin_type* type, int* version, int* api, const char** name, int* caps)
{
	if (type)
		*type = M64PLUGIN_GFX;
	if (version)
		*version = kPluginVersion;
	if (api)
		*api = kApiVersion;
	if (name)
		*name = "paraLLEl-RDP (PS5)";
	if (caps)
		*caps = 0;
	return M64ERR_SUCCESS;
}

int InitiateGFX(GFX_INFO info)
{
	l_Gfx = info;
	return 1;
}

void MoveScreen(int, int)
{
}

void ProcessDList()
{
}

void ProcessRDPList()
{
	if (!l_RomOpen)
		return;
	const uint64_t t0 = n64ps5_perf_now();
	const uint32_t current = *l_Gfx.DPC_CURRENT_REG & 0x00FFFFF8;
	const uint32_t end = *l_Gfx.DPC_END_REG & 0x00FFFFF8;
	int length = int(end) - int(current);
	if (length <= 0)
		return;
	length = unsigned(length) >> 3;
	if ((l_CmdPtr + length) & ~(0x0003FFFF >> 3))
		return;

	uint32_t offset = current;
	if (*l_Gfx.DPC_STATUS_REG & kDpStatusXbusDma)
	{
		do
		{
			offset &= 0xFF8;
			memcpy(&l_CmdData[2 * l_CmdPtr], l_Gfx.DMEM + offset, 8);
			offset += 8;
			l_CmdPtr++;
		} while (--length > 0);
	}
	else
	{
		if (end > 0x7ffffff || current > 0x7ffffff)
			return;
		do
		{
			offset &= 0xFFFFF8;
			memcpy(&l_CmdData[2 * l_CmdPtr], l_Gfx.RDRAM + offset, 8);
			offset += 8;
			l_CmdPtr++;
		} while (--length > 0);
	}

	while (l_CmdCur - l_CmdPtr < 0)
	{
		const uint32_t command = (l_CmdData[2 * l_CmdCur] >> 24) & 63;
		const int cmd_length = int(kCmdLength[command]);
		if (l_CmdPtr - l_CmdCur - cmd_length < 0)
		{
			*l_Gfx.DPC_START_REG = *l_Gfx.DPC_CURRENT_REG = *l_Gfx.DPC_END_REG;
			n64ps5_perf_add(N64PS5_PERF_RDP, n64ps5_perf_now() - t0);
			return;
		}
		if (command >= 8 && l_Rdp)
			l_Rdp->enqueue_command(unsigned(cmd_length) * 2, &l_CmdData[2 * l_CmdCur]);
		if (RDP::Op(command) == RDP::Op::SyncFull)
		{
			// the game waits for the RDP here: let the GPU finish what came before (GPU sync "Accurate");
			// "Fast" lets the game go on while the GPU works, which most games don't notice
			if (l_Rdp && l_Options.gpu_sync)
			{
				const uint64_t w0 = n64ps5_perf_now();
				l_Rdp->wait_for_timeline(l_Rdp->signal_timeline());
				l_Stats.sync_ticks += n64ps5_perf_now() - w0;
			}
			l_Stats.syncs++;
			*l_Gfx.MI_INTR_REG |= kMiIntrDp;
			l_Gfx.CheckInterrupts();
		}
		l_CmdCur += cmd_length;
	}
	l_CmdPtr = 0;
	l_CmdCur = 0;
	*l_Gfx.DPC_START_REG = *l_Gfx.DPC_CURRENT_REG = *l_Gfx.DPC_END_REG;
	n64ps5_perf_add(N64PS5_PERF_RDP, n64ps5_perf_now() - t0);
}

int RomOpen()
{
	if (!n64ps5_gpu_available())
		return 0;
	Util::set_thread_logging_interface(&g_logger);
	const size_t rdram_size = (l_Gfx.version >= 2 && l_Gfx.RDRAM_SIZE) ? *l_Gfx.RDRAM_SIZE : 8 * 1024 * 1024;
	const size_t align =
		g_gpu.device->get_device_features().host_memory_properties.minImportedHostPointerAlignment;
	if (align && (reinterpret_cast<uintptr_t>(l_Gfx.RDRAM) & (align - 1)))
	{
		OrbisLog("[gpu] RDRAM at %p is not aligned to %zu bytes", static_cast<void*>(l_Gfx.RDRAM), align);
		return 0;
	}
	const int upscale = l_Options.upscale == 2 || l_Options.upscale == 4 || l_Options.upscale == 8 ? l_Options.upscale : 1;
	l_Rdp.reset(new RDP::CommandProcessor(*g_gpu.device, l_Gfx.RDRAM, 0, rdram_size, rdram_size / 2, Flags(upscale)));
	if (!l_Rdp->device_is_supported())
	{
		OrbisLog("[gpu] paraLLEl-RDP: the device is not supported");
		l_Rdp.reset();
		return 0;
	}
	RDP::Quirks quirks;
	quirks.set_native_texture_lod(false);
	quirks.set_native_resolution_tex_rect(true);
	l_Rdp->set_quirks(quirks);
	l_CmdCur = l_CmdPtr = 0;
	l_Stats = {};
	l_RomOpen = true;
	OrbisLog("[gpu] paraLLEl-RDP ready: %ux internal resolution, GPU sync %s, RDRAM %zu KiB at %p",
		unsigned(upscale), l_Options.gpu_sync ? "accurate" : "fast", rdram_size >> 10,
		static_cast<void*>(l_Gfx.RDRAM));
	return 1;
}

void RomClosed()
{
	if (!l_RomOpen)
		return;
	l_RomOpen = false;
	if (l_Rdp)
		l_Rdp->idle();
	l_Scanout = {};
	l_Rdp.reset();
	if (g_gpu.device)
		g_gpu.device->wait_idle();
}

void ShowCFB()
{
}

void Report()
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	const uint64_t tsc = n64ps5_perf_now();
	if (l_Stats.tsc0 && l_Stats.vis)
	{
		const double ms = (now.tv_sec - l_Stats.t0.tv_sec) * 1e3 + (now.tv_nsec - l_Stats.t0.tv_nsec) / 1e6;
		const double tick_ms = ms / double(tsc - l_Stats.tsc0);
		const double vis = l_Stats.vis;
		OrbisLog("[gpu] per VI: %.2f syncs, %.2f ms waiting for them, %.2f ms waiting for the picture",
			l_Stats.syncs / vis, l_Stats.sync_ticks * tick_ms / vis, l_Stats.scanout_ticks * tick_ms / vis);
		g_gpu.device->timestamp_log([](const std::string& tag, const Vulkan::TimestampIntervalReport& r) {
			if (r.time_per_frame_context > 0.0001)
				OrbisLog("[gpu]   GPU %s: %.3f ms per frame (%.1f times)", tag.c_str(), r.time_per_frame_context * 1e3,
					r.accumulations_per_frame_context);
		});
		g_gpu.device->timestamp_log_reset();
	}
	l_Stats.syncs = l_Stats.sync_ticks = l_Stats.scanout_ticks = 0;
	l_Stats.vis = 0;
	l_Stats.tsc0 = tsc;
	l_Stats.t0 = now;
}

// One frame: the VI registers to paraLLEl-RDP, its scanout copied back, the picture to the frontend.
void UpdateScreen()
{
	if (!l_RomOpen || !l_Rdp)
		return;
	const uint64_t t0 = n64ps5_perf_now();
	unsigned int** vi = &l_Gfx.VI_STATUS_REG; // the 14 VI registers, in paraLLEl-RDP's VIRegister order
	for (int i = 0; i < int(RDP::VIRegister::Count); i++)
		l_Rdp->set_vi_register(RDP::VIRegister(i), *vi[i]);

	RDP::ScanoutOptions opts;
	opts.persist_frame_on_invalid_input = true;
	const bool filtered = l_Options.vi_mode == 0;
	opts.vi.aa = filtered;
	opts.vi.divot_filter = filtered;
	opts.vi.dither_filter = filtered;
	opts.vi.gamma_dither = filtered;
	opts.vi.scale = true;
	opts.blend_previous_frame = true;
	opts.upscale_deinterlacing = false;
	opts.crop_overscan_pixels = l_Options.hide_overscan ? 8 : 0;
	l_Rdp->scanout_async_buffer(l_Scanout, opts);

	if (l_Scanout.width && l_Scanout.height && l_Scanout.fence)
	{
		const uint64_t w0 = n64ps5_perf_now();
		l_Scanout.fence->wait();
		l_Stats.scanout_ticks += n64ps5_perf_now() - w0;
		const auto* px = static_cast<const uint32_t*>(
			g_gpu.device->map_host_buffer(*l_Scanout.buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
		n64ps5_perf_add(N64PS5_PERF_VI, n64ps5_perf_now() - t0);
		const n64ps5_frame frame = {px, int(l_Scanout.width), int(l_Scanout.height), int(l_Scanout.width)};
		n64ps5_on_vi(&frame);
		g_gpu.device->unmap_host_buffer(*l_Scanout.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
	}
	else
	{
		n64ps5_perf_add(N64PS5_PERF_VI, n64ps5_perf_now() - t0);
		n64ps5_on_vi(nullptr);
	}
	l_Rdp->begin_frame_context();
	if (++l_Stats.vis >= 600 || !l_Stats.tsc0)
		Report();
	if (l_RenderCallback)
		l_RenderCallback(1);
}

void ViStatusChanged()
{
}

void ViWidthChanged()
{
}

void ChangeWindow()
{
}

void ReadScreen2(void*, int* width, int* height, int)
{
	if (width)
		*width = 0;
	if (height)
		*height = 0;
}

void SetRenderingCallback(void (*callback)(int))
{
	l_RenderCallback = callback;
}

void ResizeVideoOutput(int, int)
{
}

void FBRead(unsigned int)
{
}

void FBWrite(unsigned int, unsigned int)
{
}

void FBGetFrameBufferInfo(void*)
{
}

#define SYM(name) {#name, (m64p_function)name}
const m64ps5_symbol l_Symbols[] = {
	SYM(PluginStartup),
	SYM(PluginShutdown),
	SYM(PluginGetVersion),
	SYM(ChangeWindow),
	SYM(InitiateGFX),
	SYM(MoveScreen),
	SYM(ProcessDList),
	SYM(ProcessRDPList),
	SYM(RomClosed),
	SYM(RomOpen),
	SYM(ShowCFB),
	SYM(UpdateScreen),
	SYM(ViStatusChanged),
	SYM(ViWidthChanged),
	SYM(ReadScreen2),
	SYM(SetRenderingCallback),
	SYM(ResizeVideoOutput),
	SYM(FBRead),
	SYM(FBWrite),
	SYM(FBGetFrameBufferInfo),
	{nullptr, nullptr},
};
#undef SYM
} // namespace

extern "C" const m64ps5_library m64ps5_gfx_parallel_lib = {"parallel-rdp", l_Symbols};

extern "C" void n64ps5_gpu_set_options(const n64ps5_gfx_options* opt)
{
	if (opt)
		l_Options = *opt;
}

// The Vulkan instance and device paraLLEl-RDP runs on, made the first time a game asks for the GPU renderer
// and kept for the app's life. False when the GPU can't be used (the frontend then uses angrylion).
extern "C" bool n64ps5_gpu_available(void)
{
	if (g_gpu.tried)
		return g_gpu.device != nullptr;
	g_gpu.tried = true;
	Util::set_thread_logging_interface(&g_logger);
	// RADV keeps the shaders it compiled (paraLLEl-RDP's) here, so later starts skip compiling them
	const std::string cache = OrbisDir("cache") + "/radv";
	OrbisMkdirs(cache);
	setenv("MESA_SHADER_CACHE_DIR", cache.c_str(), 1);
	// GPU timestamps around paraLLEl-RDP's work, for Report (test builds)
	setenv("PARALLEL_RDP_BENCH", "2", 1);

	if (!Vulkan::Context::init_loader(vk_icdGetInstanceProcAddr))
	{
		OrbisLog("[gpu] Vulkan loader init failed");
		return false;
	}
	std::unique_ptr<Vulkan::Context> context(new Vulkan::Context);
	if (!context->init_instance_and_device(nullptr, 0, nullptr, 0, 0))
	{
		OrbisLog("[gpu] Vulkan instance/device creation failed");
		return false;
	}
	std::unique_ptr<Vulkan::Device> device(new Vulkan::Device);
	device->set_context(*context);
	if (!device->get_device_features().supports_external_memory_host)
	{
		OrbisLog("[gpu] no VK_EXT_external_memory_host: paraLLEl-RDP can't share RDRAM with the GPU");
		return false;
	}
	g_gpu.context = std::move(context);
	g_gpu.device = std::move(device);
	OrbisLog("[gpu] Vulkan device ready for paraLLEl-RDP (shader cache %s)", cache.c_str());
	return true;
}
