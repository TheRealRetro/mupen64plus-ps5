// Mupen64Plus PS5: DualSense / DualShock input through libScePad.
//
// sceUserServiceInitialize + scePadInit once, then scePadOpen(user, STANDARD, 0) for every logged-in user
// (the ps5-payload-dev SDL2 port's joystick driver and PS5SX2 do the same). The console delivers the pads to
// the title in front only: 1.0-1.5, payloads next to the system UI, never read a button (the 1.5 console log:
// "scePadOpen 809b0081, scePadGetHandle 0, neither reads"). 1.6 is the dashboard title itself.
//
// SPDX-License-Identifier: MIT

#include "ProsperoInput.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <cstring>
#include <ctime>

namespace ps5input
{
namespace
{
struct Slot
{
	int32_t user = -1;
	int handle = -1;
	bool valid = false; // handle answers scePadReadState (PS5 handles can have the top bit set: never test < 0)
	bool owned = false; // opened by us (not borrowed from the system UI with scePadGetHandle)
	int last_log = 0; // the last open result logged, so a retry every 2 s doesn't flood boot.log
	int read_errors = 0;
	bool first_input = false;
	uint16_t rumble = 0; // last motors sent (large << 8 | small)
	PadState state;
};

Slot s_slots[kMaxPads];
PadState s_empty;
uint32_t s_prev_p1 = 0;
uint32_t s_pressed_p1 = 0;
double s_next_detect = 0;
bool s_inited = false;

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// A return value of the pad library is an error when it is in libScePad's error range (0x8092xxxx).
// Anything else is a handle -- the first console log (1.2) had the system's handle 0x809b0081, which is
// negative as an int, and 1.2 wrongly took it for an error.
bool IsPadError(int r)
{
	return (uint32_t(r) & 0xffff0000u) == 0x80920000u;
}

int ReadCode(int handle)
{
	ScePadData data;
	memset(&data, 0, sizeof(data));
	return scePadReadState(handle, &data);
}

// Opens the user's pad. 1.6 runs as the title in front (eboot.bin), where scePadOpen gives a plain handle,
// and takes it the way PS5SX2 does (main-boot.cpp, orbis_pad_thread): scePadOpen, scePadGetHandle when that
// fails, and any handle >= 0 is used. The 1.3-1.5 payload's rule stays behind it: a value outside the pad
// library's error range that answers scePadReadState (the 1.2 console log's 0x809b0081).
bool OpenPad(Slot& s, int32_t user)
{
	const int opened = scePadOpen(user, SCE_PAD_PORT_TYPE_STANDARD, 0, nullptr);
	int got = -1;
	if (opened < 0)
		got = scePadGetHandle(user, SCE_PAD_PORT_TYPE_STANDARD, 0);
	const int plain = opened >= 0 ? opened : got;
	if (plain >= 0)
	{
		s.handle = plain;
		s.valid = true;
		s.owned = opened >= 0;
		s.read_errors = 0;
		OrbisLog("[pad] user %x: scePadOpen %x, scePadGetHandle %x -> using %x (%s), first read %x", unsigned(user),
			unsigned(opened), unsigned(got), unsigned(plain), s.owned ? "opened" : "existing handle",
			unsigned(ReadCode(plain)));
		return true;
	}
	const int candidates[2] = {opened, got};
	int codes[2] = {0, 0};
	for (int i = 0; i < 2; i++)
	{
		const int h = candidates[i];
		if (IsPadError(h) || (i == 1 && h == opened))
			continue;
		codes[i] = ReadCode(h);
		if (codes[i] == 0)
		{
			s.handle = h;
			s.valid = true;
			s.owned = i == 0;
			s.read_errors = 0;
			OrbisLog("[pad] user %x: scePadOpen %x, scePadGetHandle %x -> using %x (%s)", unsigned(user), unsigned(opened),
				unsigned(got), unsigned(h), s.owned ? "ours" : "shared with the system");
			return true;
		}
	}
	if (s.last_log != (opened ^ got))
	{
		s.last_log = opened ^ got;
		OrbisLog("[pad] user %x: scePadOpen %x (read %x), scePadGetHandle %x (read %x): no usable handle, will try again",
			unsigned(user), unsigned(opened), unsigned(codes[0]), unsigned(got), unsigned(codes[1]));
	}
	if (!IsPadError(opened))
		scePadClose(opened);
	return false;
}

void Detect()
{
	int32_t list[4] = {-1, -1, -1, -1};
	if (sceUserServiceGetLoginUserIdList(list) != 0)
		return;
	int32_t fg = -1;
	sceUserServiceGetForegroundUser(&fg);

	// Wanted order: the foreground user first, then the others as the system lists them.
	int32_t wanted[kMaxPads] = {-1, -1, -1, -1};
	int n = 0;
	if (fg != -1)
		wanted[n++] = fg;
	for (int i = 0; i < 4 && n < kMaxPads; i++)
		if (list[i] != -1 && list[i] != fg)
			wanted[n++] = list[i];

	for (int i = 0; i < kMaxPads; i++)
	{
		if (s_slots[i].user == wanted[i] && (wanted[i] == -1 || s_slots[i].valid))
			continue;
		if (s_slots[i].valid && s_slots[i].owned)
			scePadClose(s_slots[i].handle); // handles the system UI owns stay open
		if (s_slots[i].user != wanted[i])
			s_slots[i].last_log = 0;
		s_slots[i].handle = -1;
		s_slots[i].valid = false;
		s_slots[i].user = wanted[i];
		s_slots[i].state = PadState();
		if (wanted[i] != -1)
		{
			if (OpenPad(s_slots[i], wanted[i]))
			{
				// Player colours on the light bar: blue, red, green, pink.
				static const ScePadColor colors[kMaxPads] = {
					{0, 0, 255, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {255, 0, 255, 255}};
				scePadSetLightBar(s_slots[i].handle, &colors[i]);
				// rumble through the classic two-motor interface (the N64 Rumble Pak)
				const int vm = scePadSetVibrationMode(s_slots[i].handle, SCE_PAD_VIBRATION_MODE_COMPATIBLE);
				OrbisLog("[pad] player %d: scePadSetVibrationMode(compatible) -> %x", i + 1, unsigned(vm));
			}
		}
	}
}

uint32_t StickToDpad(uint8_t x, uint8_t y)
{
	uint32_t b = 0;
	if (x < 64)
		b |= SCE_PAD_BUTTON_LEFT;
	else if (x > 192)
		b |= SCE_PAD_BUTTON_RIGHT;
	if (y < 64)
		b |= SCE_PAD_BUTTON_UP;
	else if (y > 192)
		b |= SCE_PAD_BUTTON_DOWN;
	return b;
}
} // namespace

bool Init()
{
	if (s_inited)
		return true;
	int r = sceUserServiceInitialize(nullptr);
	OrbisLog("[pad] sceUserServiceInitialize -> %x", r);
	if (r != 0 && r != SCE_USER_SERVICE_ERROR_ALREADY_INITIALIZED)
		return false;
	r = scePadInit();
	OrbisLog("[pad] scePadInit -> %x", r);
	if (r != 0)
		return false;
	s_inited = true;
	Detect();
	s_next_detect = Now() + 2.0;
	return true;
}

void Shutdown()
{
	for (Slot& s : s_slots)
	{
		if (s.valid && s.owned)
			scePadClose(s.handle);
		s = Slot();
	}
	s_inited = false;
}

void Poll()
{
	if (!s_inited)
		return;
	const double now = Now();
	if (now >= s_next_detect)
	{
		Detect();
		s_next_detect = now + 2.0;
	}
	for (Slot& s : s_slots)
	{
		if (!s.valid)
		{
			s.state = PadState();
			continue;
		}
		ScePadData data;
		memset(&data, 0, sizeof(data));
		// Like the SDK's SDL2 port, trust a successful read and don't look at data.connected (its offset in
		// this hand-written struct is the least certain field).
		const int r = scePadReadState(s.handle, &data);
		if (r != 0)
		{
			s.state.connected = false;
			s.state.buttons = s.state.raw_buttons = 0;
			if (++s.read_errors == 1)
				OrbisLog("[pad] scePadReadState(%x) -> %x", unsigned(s.handle), unsigned(r));
			if (s.read_errors >= 120) // two seconds of errors: open it again
			{
				s.valid = false;
				s.user = -2; // force Detect to redo this slot
			}
			continue;
		}
		s.read_errors = 0;
		if (!s.first_input && data.buttons != 0)
		{
			s.first_input = true;
			OrbisLog("[pad] first buttons from handle %x: %x", unsigned(s.handle), data.buttons);
		}
		PadState& p = s.state;
		p.connected = true;
		p.raw_buttons = data.buttons;
		p.lx = data.leftStick.x;
		p.ly = data.leftStick.y;
		p.rx = data.rightStick.x;
		p.ry = data.rightStick.y;
		p.l2 = data.analogButtons.l2;
		p.r2 = data.analogButtons.r2;
		p.buttons = data.buttons | StickToDpad(p.lx, p.ly);
	}
	const uint32_t cur = s_slots[0].state.buttons;
	s_pressed_p1 = cur & ~s_prev_p1;
	s_prev_p1 = cur;
}

const PadState& Pad(int player)
{
	if (player < 0 || player >= kMaxPads)
		return s_empty;
	return s_slots[player].state;
}

int ConnectedCount()
{
	int n = 0;
	for (const Slot& s : s_slots)
		n += s.state.connected;
	return n;
}

uint32_t Pressed()
{
	return s_pressed_p1;
}

void SetRumble(int player, uint8_t large, uint8_t small)
{
	if (player < 0 || player >= kMaxPads)
		return;
	Slot& s = s_slots[player];
	const uint16_t want = uint16_t((large << 8) | small);
	if (!s.valid || s.rumble == want)
		return;
	s.rumble = want;
	ScePadVibrationParam p = {large, small};
	const int r = scePadSetVibration(s.handle, &p);
	static int logged = 0;
	if (r != 0 || logged < 20) // the first few, then failures only
	{
		logged++;
		OrbisLog("[pad] player %d: scePadSetVibration(%u, %u) -> %x", player + 1, large, small, unsigned(r));
	}
}
} // namespace ps5input
