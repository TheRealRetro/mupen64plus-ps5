// Mupen64Plus PS5 frontend: the game list and the menus, drawn on the 1920x1080 surface.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace fe
{
// The ROM browser. Returns the chosen file, or "" when the user picks "quit" (OPTIONS).
std::string RomBrowser();

enum class PauseAction
{
	Resume,
	BackToList,
	Quit,
};
// The in-game menu, over the paused picture.
PauseAction PauseMenu();

// The settings screen (Triangle on the shelf).
void SettingsMenu();

// Shows a message box until Cross / Circle is pressed (errors such as a ROM that won't load).
void MessageBox(const std::string& title, const std::string& text);

// A question with longer text (wrapped, '\n' for new lines, Markdown bullets shown as bullets): true for Cross.
bool Confirm(const std::string& title, const std::string& text, const char* yes, const char* no);

// A full-screen "working..." line, left up while something slow runs.
void ShowBusy(const std::string& text);
} // namespace fe
