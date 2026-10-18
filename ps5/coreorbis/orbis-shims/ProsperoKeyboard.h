// Mupen64Plus PS5: the system's on-screen keyboard (libSceImeDialog), for naming cheats and typing codes.
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>

namespace ps5keyboard
{
// Loads the keyboard's system module. Before the app leaves the sandbox (main-boot.cpp): afterwards the system
// looks for modules at a path inside the sandbox and doesn't find them (80020063; ps5-native's docs/CURL.md,
// "Elevated apps"). The module stays loaded.
void Preload();

enum class Type
{
	Text, // any text: a name
	Number, // digits
	Latin, // letters and digits: GameShark codes
};

// Opens the keyboard over the picture with `text` in its field, titled `title`, and calls `frame` (show one
// frame) until the player closes it. True with the new text (UTF-8) when the player pressed Done; false when they
// canceled it, or when the keyboard could not open -- then `error` says why ("the keyboard did not open (0x...)").
bool Edit(const std::string& title, std::string& text, int max_chars, Type type, const std::function<void()>& frame,
	std::string* error = nullptr);
} // namespace ps5keyboard
