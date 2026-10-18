// Mupen64Plus PS5: the system's on-screen keyboard (ProsperoKeyboard.h).
//
// libSceImeDialog's functions are imported like the other system calls, but its module is one the app loads
// itself, with sceSysmoduleLoadModule (ps5-generals' SDL port does the same for the keyboard and mouse
// modules). That has to happen while the app is still in its sandbox: once it left it, a module asked for is
// looked for at the sandbox's path and not found (80020063 on the console, by path or by sysmodule; ps5-native's
// docs/CURL.md, "Elevated apps"), so main-boot.cpp calls Preload() before the jailbreak. Until the module loaded
// its functions aren't called, so where it doesn't load the app still runs, only without typing. The dialog is the
// system's: it draws itself over the app's picture and takes the controller while it is up. sceImeDialogInit
// opens it, sceImeDialogGetStatus says when the player closed it, sceImeDialogGetResult how (Done or canceled),
// and sceImeDialogTerm ends it.
//
// SPDX-License-Identifier: MIT

#include "ProsperoKeyboard.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <cstdio>
#include <cstring>
#include <vector>

static_assert(sizeof(SceImeDialogParam) == 0x60, "SceImeDialogParam is 0x60 bytes");

namespace ps5keyboard
{
namespace
{
bool s_tried = false;
int s_load = -1; // sceSysmoduleLoadModule's answer: 0 (or more) loaded

bool Load()
{
	if (!s_tried)
	{
		s_tried = true;
		s_load = sceSysmoduleLoadModule(SCE_SYSMODULE_IME_DIALOG);
		OrbisLog("[keyboard] sceSysmoduleLoadModule(IME dialog %#x) -> %x", SCE_SYSMODULE_IME_DIALOG, unsigned(s_load));
	}
	return s_load >= 0;
}
} // namespace

void Preload()
{
	Load();
}

namespace
{
// UTF-8 -> UTF-16 (at most max units), and back
std::vector<uint16_t> ToUtf16(const std::string& s, size_t max)
{
	std::vector<uint16_t> out;
	for (size_t i = 0; i < s.size();)
	{
		const unsigned char c = (unsigned char)s[i];
		uint32_t cp = c;
		int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra)
			cp = c & (0x3F >> extra);
		i++;
		for (; extra > 0 && i < s.size(); extra--, i++)
			cp = cp << 6 | ((unsigned char)s[i] & 0x3F);
		const size_t need = cp >= 0x10000 ? 2 : 1;
		if (out.size() + need > max)
			break;
		if (cp >= 0x10000)
		{
			cp -= 0x10000;
			out.push_back(uint16_t(0xD800 | (cp >> 10)));
			out.push_back(uint16_t(0xDC00 | (cp & 0x3FF)));
		}
		else
			out.push_back(uint16_t(cp));
	}
	return out;
}

std::string ToUtf8(const uint16_t* s)
{
	std::string out;
	for (size_t i = 0; s[i]; i++)
	{
		uint32_t cp = s[i];
		if (cp >= 0xD800 && cp < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
		{
			cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
			i++;
		}
		if (cp < 0x80)
			out += char(cp);
		else if (cp < 0x800)
		{
			out += char(0xC0 | (cp >> 6));
			out += char(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			out += char(0xE0 | (cp >> 12));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		}
		else
		{
			out += char(0xF0 | (cp >> 18));
			out += char(0x80 | ((cp >> 12) & 0x3F));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		}
	}
	return out;
}

bool Fail(std::string* error, const char* what, int code)
{
	char text[96];
	snprintf(text, sizeof(text), "The keyboard did not open (%s %08X)", what, unsigned(code));
	OrbisLog("[keyboard] %s", text);
	if (error)
		*error = text;
	return false;
}
} // namespace

bool Edit(const std::string& title, std::string& text, int max_chars, Type type, const std::function<void()>& frame,
	std::string* error)
{
	if (!Load())
		return Fail(error, "module", s_load);
	const size_t max = size_t(max_chars < 1 ? 1 : max_chars > SCE_IME_DIALOG_MAX_TEXT_LENGTH ? SCE_IME_DIALOG_MAX_TEXT_LENGTH : max_chars);
	std::vector<uint16_t> buffer = ToUtf16(text, max);
	buffer.resize(max + 1, 0);
	std::vector<uint16_t> title16 = ToUtf16(title, 128);
	title16.push_back(0);

	SceImeDialogParam param;
	memset(&param, 0, sizeof(param));
	int32_t user = -1;
	sceUserServiceGetForegroundUser(&user);
	param.userId = user;
	param.type = type == Type::Number ? SCE_IME_TYPE_NUMBER : type == Type::Latin ? SCE_IME_TYPE_BASIC_LATIN : SCE_IME_TYPE_DEFAULT;
	param.maxTextLength = uint32_t(max);
	param.inputTextBuffer = buffer.data();
	param.posx = 960.0f;
	param.posy = 540.0f;
	param.horizontalAlignment = SCE_IME_HALIGN_CENTER;
	param.verticalAlignment = SCE_IME_VALIGN_CENTER;
	param.title = title16.data();

	int r = sceImeDialogInit(&param, nullptr);
	if (r != 0)
	{
		// one left open (a crash of the menu around it): end that one and try again
		sceImeDialogTerm();
		r = sceImeDialogInit(&param, nullptr);
	}
	OrbisLog("[keyboard] sceImeDialogInit(user %x, type %d, max %zu) -> %x", unsigned(user), param.type, max, unsigned(r));
	if (r != 0)
		return Fail(error, "init", r);

	// running until the player closes it (the first frames may still say "none" while it comes up)
	int status = SCE_IME_DIALOG_STATUS_RUNNING;
	for (int n = 0;; n++)
	{
		status = sceImeDialogGetStatus();
		if (status != SCE_IME_DIALOG_STATUS_RUNNING && !(status == SCE_IME_DIALOG_STATUS_NONE && n < 30))
			break;
		frame();
	}
	SceImeDialogResult result;
	memset(&result, 0, sizeof(result));
	result.endstatus = SCE_IME_DIALOG_END_STATUS_ABORTED;
	const int rr = status == SCE_IME_DIALOG_STATUS_FINISHED ? sceImeDialogGetResult(&result) : status;
	sceImeDialogTerm();
	OrbisLog("[keyboard] closed: status %x, result %x, end %d", unsigned(status), unsigned(rr), int(result.endstatus));
	if (status != SCE_IME_DIALOG_STATUS_FINISHED || rr != 0)
		return Fail(error, "status", status != SCE_IME_DIALOG_STATUS_FINISHED ? status : rr);
	if (result.endstatus != SCE_IME_DIALOG_END_STATUS_OK)
		return false;
	buffer[max] = 0;
	text = ToUtf8(buffer.data());
	return true;
}
} // namespace ps5keyboard
