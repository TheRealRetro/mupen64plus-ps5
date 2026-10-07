// Mupen64Plus PS5: the /data/mupen64plus folder layout and the boot log (orbis-shims/orbis_paths.cpp).
//
//   /data/mupen64plus/roms         ROMs (.z64 .n64 .v64 .rom .zip); sub-folders are browsable
//   /data/mupen64plus/saves        in-game saves (EEPROM, SRAM, FlashRAM, Controller Pak), written by the core
//   /data/mupen64plus/states       save states (<rom>.st0 .. .st9)
//   /data/mupen64plus/config       mupen64plus.cfg, the core's own configuration
//   /data/mupen64plus/data         mupen64plus.ini, the core's ROM catalog (written by the app)
//   /data/mupen64plus/covers       downloaded covers and your own
//   /data/mupen64plus/logs         boot.log (this run) and boot.prev.log (the run before); installer.log
//   /data/mupen64plus/mupen64plus-ps5.ini  the frontend's settings
//
// USB drives are searched too: /mnt/usbN/mupen64plus/roms (N = 0..7) and /mnt/extN/mupen64plus/roms.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#ifndef ORBIS_ROOT_DEFAULT
#define ORBIS_ROOT_DEFAULT "/data/mupen64plus"
#endif

// Root folder (ORBIS_ROOT_DEFAULT; the host tests point it elsewhere with N64PS5_PS5_ROOT).
const std::string& OrbisRoot();
// <root>/<sub>, created at boot by OrbisPathsInit.
std::string OrbisDir(const char* sub);
// Creates the folder tree. Returns false when the root itself can't be created (no /data access).
bool OrbisPathsInit();
// Folders that hold ROMs and exist right now (internal first, then USB drives).
std::vector<std::string> OrbisRomRoots();

bool OrbisIsDir(const std::string& path);
bool OrbisIsFile(const std::string& path);
bool OrbisMkdirs(const std::string& path);

// <root>/logs/<name>.log (the run before kept as <name>.prev.log): boot.log for the emulator, installer.log
// for the installer/helper payload. Every line also goes to stdout. Lines logged before the file is open
// (before the jailbreak shows /data to the app) are kept and written first.
void OrbisLogOpen(const char* name = "boot");
void OrbisLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void OrbisLogClose();
// The open log's file descriptor, for the crash handler's signal-safe write(); -1 before it is open.
int OrbisLogFd();
