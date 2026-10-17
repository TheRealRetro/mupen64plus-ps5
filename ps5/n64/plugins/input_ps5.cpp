// Mupen64Plus PS5: the input plugin, DualSense / DualShock through libScePad (ProsperoInput.h).
//
// The frontend polls the pads once per video interrupt (fe_emu.cpp); GetKeys turns that state into the
// N64 controller's buttons and stick. The left stick is the analog stick; every other control presses the
// N64 button the controller layout gives it (n64ps5_input_options.layout, per game). The default layout, by
// position as on the N64 pad:
//
//   left stick  -> analog stick         D-pad      -> D-pad
//   Cross       -> A                    Square     -> B
//   L2 or R2    -> Z                    L1 / R1    -> L / R
//   right stick -> C buttons            Triangle   -> C-up, Circle -> C-down
//   OPTIONS     -> Start                L3, R3     -> nothing
//
// Rumble: the core sends Rumble Pak writes through ControllerCommand; they drive the controller's motors.
//
// SPDX-License-Identifier: MIT

#include "api/m64p_plugin.h"
#include "api/m64p_types.h"

#include "OrbisPaths.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "n64_bridge.h"
#include "static_dynlib.h"

#include <atomic>
#include <cmath>

namespace
{
constexpr int kVersion = 0x010000;
constexpr int kApiVersion = 0x020100;
constexpr int kNumControllers = 4;

bool g_init = false;
CONTROL* g_controls = nullptr;
n64ps5_input_options g_opt = {12, 80, 1, 0, 0, {}};
bool g_opt_set = false;
std::atomic<bool> g_block{false};

// One stick axis (0..255, centre 128) -> -range..range, with a dead zone and a linear ramp after it.
int Axis(uint8_t v, int deadzone_pct, int range)
{
	const double x = (double(v) - 128.0) / 127.0; // -1..1
	const double dz = deadzone_pct / 100.0;
	const double mag = std::fabs(x);
	if (mag <= dz)
		return 0;
	double s = (mag - dz) / (1.0 - dz);
	if (s > 1.0)
		s = 1.0;
	const int out = int(std::lround(s * range));
	return x < 0 ? -out : out;
}

// ---- the plugin API -------------------------------------------------------------------------------------
m64p_error PluginStartup(m64p_dynlib_handle, void*, void (*)(void*, int, const char*))
{
	if (g_init)
		return M64ERR_ALREADY_INIT;
	g_init = true;
	return M64ERR_SUCCESS;
}

m64p_error PluginShutdown()
{
	if (!g_init)
		return M64ERR_NOT_INIT;
	g_init = false;
	return M64ERR_SUCCESS;
}

m64p_error PluginGetVersion(m64p_plugin_type* type, int* version, int* api, const char** name, int* caps)
{
	if (type)
		*type = M64PLUGIN_INPUT;
	if (version)
		*version = kVersion;
	if (api)
		*api = kApiVersion;
	if (name)
		*name = "PS5 DualSense";
	if (caps)
		*caps = 0;
	return M64ERR_SUCCESS;
}

int PakPlugin()
{
	switch (g_opt.pak)
	{
		case 0: return PLUGIN_NONE;
		case 2: return PLUGIN_RUMBLE_PAK;
		default: return PLUGIN_MEMPAK;
	}
}

void InitiateControllers(CONTROL_INFO info)
{
	if (!g_opt_set) // before the frontend's first options: the default layout
		n64ps5_input_default_layout(g_opt.layout);
	g_controls = info.Controls;
	if (!g_controls)
		return;
	ps5input::Poll();
	for (int i = 0; i < kNumControllers; i++)
	{
		// Games look for controllers when they boot: player 1 is always plugged in, the others when a pad
		// is connected at that moment.
		g_controls[i].Present = (i == 0 || ps5input::Pad(i).connected) ? 1 : 0;
		g_controls[i].RawData = 0;
		g_controls[i].Plugin = PakPlugin();
		g_controls[i].Type = CONT_TYPE_STANDARD;
	}
}

void GetKeys(int control, BUTTONS* keys)
{
	if (!keys)
		return;
	keys->Value = 0;
	if (control < 0 || control >= kNumControllers || g_block.load(std::memory_order_relaxed))
		return;
	const ps5input::PadState& p = ps5input::Pad(control);
	if (!p.connected)
		return;
	const uint32_t b = p.raw_buttons; // the D-pad only (the stick is the analog stick here)

	// what each DualSense control is doing; the right stick counts at half tilt. Settings "Invert left stick"
	// and "Invert right stick" (GitHub issue #3) turn the sticks' axes round.
	bool on[N64PS5_PAD_COUNT] = {};
	on[N64PS5_PAD_CROSS] = (b & SCE_PAD_BUTTON_CROSS) != 0;
	on[N64PS5_PAD_CIRCLE] = (b & SCE_PAD_BUTTON_CIRCLE) != 0;
	on[N64PS5_PAD_SQUARE] = (b & SCE_PAD_BUTTON_SQUARE) != 0;
	on[N64PS5_PAD_TRIANGLE] = (b & SCE_PAD_BUTTON_TRIANGLE) != 0;
	on[N64PS5_PAD_L1] = (b & SCE_PAD_BUTTON_L1) != 0;
	on[N64PS5_PAD_R1] = (b & SCE_PAD_BUTTON_R1) != 0;
	on[N64PS5_PAD_L2] = (b & SCE_PAD_BUTTON_L2) != 0;
	on[N64PS5_PAD_R2] = (b & SCE_PAD_BUTTON_R2) != 0;
	on[N64PS5_PAD_L3] = (b & SCE_PAD_BUTTON_L3) != 0;
	on[N64PS5_PAD_R3] = (b & SCE_PAD_BUTTON_R3) != 0;
	on[N64PS5_PAD_OPTIONS] = (b & SCE_PAD_BUTTON_OPTIONS) != 0;
	on[N64PS5_PAD_UP] = (b & SCE_PAD_BUTTON_UP) != 0;
	on[N64PS5_PAD_DOWN] = (b & SCE_PAD_BUTTON_DOWN) != 0;
	on[N64PS5_PAD_LEFT] = (b & SCE_PAD_BUTTON_LEFT) != 0;
	on[N64PS5_PAD_RIGHT] = (b & SCE_PAD_BUTTON_RIGHT) != 0;
	const bool rs_right = p.rx > 192, rs_left = p.rx < 64, rs_down = p.ry > 192, rs_up = p.ry < 64;
	const bool rs_flip_x = (g_opt.invert_c & 1) != 0, rs_flip_y = (g_opt.invert_c & 2) != 0;
	on[N64PS5_PAD_RS_RIGHT] = rs_flip_x ? rs_left : rs_right;
	on[N64PS5_PAD_RS_LEFT] = rs_flip_x ? rs_right : rs_left;
	on[N64PS5_PAD_RS_DOWN] = rs_flip_y ? rs_up : rs_down;
	on[N64PS5_PAD_RS_UP] = rs_flip_y ? rs_down : rs_up;

	// and what they press on the N64 pad
	bool n64[N64PS5_N64_COUNT] = {};
	for (int i = 0; i < N64PS5_PAD_COUNT; i++)
		if (on[i] && g_opt.layout[i] < N64PS5_N64_COUNT)
			n64[g_opt.layout[i]] = true;
	keys->A_BUTTON = n64[N64PS5_N64_A];
	keys->B_BUTTON = n64[N64PS5_N64_B];
	keys->Z_TRIG = n64[N64PS5_N64_Z];
	keys->L_TRIG = n64[N64PS5_N64_L];
	keys->R_TRIG = n64[N64PS5_N64_R];
	keys->START_BUTTON = n64[N64PS5_N64_START];
	keys->U_CBUTTON = n64[N64PS5_N64_C_UP];
	keys->D_CBUTTON = n64[N64PS5_N64_C_DOWN];
	keys->L_CBUTTON = n64[N64PS5_N64_C_LEFT];
	keys->R_CBUTTON = n64[N64PS5_N64_C_RIGHT];
	keys->U_DPAD = n64[N64PS5_N64_D_UP];
	keys->D_DPAD = n64[N64PS5_N64_D_DOWN];
	keys->L_DPAD = n64[N64PS5_N64_D_LEFT];
	keys->R_DPAD = n64[N64PS5_N64_D_RIGHT];

	const int x = Axis(p.lx, g_opt.deadzone, g_opt.range);
	const int y = -Axis(p.ly, g_opt.deadzone, g_opt.range); // the N64's Y grows upwards
	keys->X_AXIS = (g_opt.invert & 1) ? -x : x;
	keys->Y_AXIS = (g_opt.invert & 2) ? -y : y;
}

// Raw joybus commands for this controller. Only the Rumble Pak's motor write is looked at:
// T=0x23 R=0x01, command 0x03 (pak write), address 0xC000 (+CRC), 32 data bytes (non-zero = motor on).
void ControllerCommand(int control, unsigned char* command)
{
	if (!command || control < 0 || control >= kNumControllers)
		return;
	if (command[2] != 0x03)
		return;
	const unsigned addr = ((unsigned(command[3]) << 8) | command[4]) & 0xffe0u;
	if (addr != 0xc000u)
		return;
	const bool on = command[5] != 0;
	static int logged = 0;
	if (logged < 10)
	{
		logged++;
		OrbisLog("[input] controller %d: Rumble Pak motor %s", control + 1, on ? "on" : "off");
	}
	ps5input::SetRumble(control, on ? 200 : 0, on ? 160 : 0);
}

void ReadController(int, unsigned char*)
{
}

int RomOpen()
{
	return 1;
}

void RomClosed()
{
	for (int i = 0; i < kNumControllers; i++)
		ps5input::SetRumble(i, 0, 0);
}

void SDL_KeyDown(int, int)
{
}

void SDL_KeyUp(int, int)
{
}

#define SYM(name) {#name, reinterpret_cast<m64p_function>(&name)}
const m64ps5_symbol kSymbols[] = {
	SYM(PluginStartup),
	SYM(PluginShutdown),
	SYM(PluginGetVersion),
	SYM(InitiateControllers),
	SYM(GetKeys),
	SYM(ControllerCommand),
	SYM(ReadController),
	SYM(RomOpen),
	SYM(RomClosed),
	SYM(SDL_KeyDown),
	SYM(SDL_KeyUp),
	{nullptr, nullptr},
};
} // namespace

extern "C" const m64ps5_library m64ps5_input_lib = {"ps5-dualsense", kSymbols};

extern "C" void n64ps5_input_set_options(const n64ps5_input_options* opt)
{
	if (opt)
	{
		g_opt = *opt;
		g_opt_set = true;
	}
}

extern "C" void n64ps5_input_default_layout(unsigned char layout[N64PS5_PAD_COUNT])
{
	static const unsigned char kDefault[N64PS5_PAD_COUNT] = {
		N64PS5_N64_A, // Cross
		N64PS5_N64_C_DOWN, // Circle
		N64PS5_N64_B, // Square
		N64PS5_N64_C_UP, // Triangle
		N64PS5_N64_L, // L1
		N64PS5_N64_R, // R1
		N64PS5_N64_Z, // L2
		N64PS5_N64_Z, // R2
		N64PS5_N64_NONE, // L3
		N64PS5_N64_NONE, // R3
		N64PS5_N64_START, // OPTIONS
		N64PS5_N64_D_UP,
		N64PS5_N64_D_DOWN,
		N64PS5_N64_D_LEFT,
		N64PS5_N64_D_RIGHT,
		N64PS5_N64_C_UP, // right stick
		N64PS5_N64_C_DOWN,
		N64PS5_N64_C_LEFT,
		N64PS5_N64_C_RIGHT,
	};
	for (int i = 0; i < N64PS5_PAD_COUNT; i++)
		layout[i] = kDefault[i];
}

extern "C" void n64ps5_input_block(bool block)
{
	g_block.store(block);
}
