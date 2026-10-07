// Mupen64Plus PS5: what the logs need to say where something failed (orbis-shims/orbis_debug.cpp). 1.6.1.
//
//   - every log line also goes to the kernel log (sceKernelDebugOutText: klogsrv shows it) and as a UDP
//     packet to the local network (broadcast to port 9999; tools/logrecv.py on a PC prints and keeps them),
//     so a run that dies before /data can be written still leaves its lines somewhere;
//   - a crash handler logs the signal, the address in the eboot (eboot+0x...: the build's symbol map,
//     symbols-<version>.txt, names the function), the registers, a frame-pointer backtrace and the stage;
//   - stages: each step of the start is named, timed and written to logs/last-stage.txt; a watchdog says
//     when nothing has moved for 5 s and in which stage;
//   - the environment: firmware, pid, uid, memory, which folders the process sees, before and after the
//     jailbreak; and the app checks its own eboot.bin and libc.prx against the installer's manifest.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Who is logging ("app", "installer", "helper"): the prefix of the UDP and kernel log lines.
void OrbisLogIdentity(const char* who);
// Called by OrbisLog for each finished line (kernel log + UDP). Not for direct use.
void OrbisDebugSink(const char* line, size_t len);
// After /data is visible: /data/mupen64plus/log-udp.txt may hold "off" or "IP[:port]" (default: broadcast :9999).
void OrbisDebugConfigure();

// Crash handler for this process; OrbisCrashThreadInit gives the calling thread an alternate signal stack.
void OrbisCrashHandlerInstall();
void OrbisCrashThreadInit();

// A named step: logged with the time since the previous step and since the start, kept for the crash
// handler and the watchdog, written to logs/last-stage.txt.
void OrbisStage(const char* name);
const char* OrbisCurrentStage();
// Progress for the watchdog (every frame of every loop: ps5input::Poll calls it).
void OrbisHeartbeat();
void OrbisWatchdogStart(double stall_seconds = 5.0);

// "[env] ..." lines: firmware, pid, uids, memory, folders the process sees. `when` labels the dump.
void OrbisLogEnvironment(const char* when);

// The app's folder: /app0 on the console (N64PS5_APP0 on the host).
std::string OrbisAppRoot();
// SHA-256 of a buffer / of a file (hex, "" when the file can't be read).
std::string OrbisSha256Hex(const void* data, size_t size);
std::string OrbisSha256File(const std::string& path, size_t* size = nullptr);
// <root>/install-manifest.txt, written by the installer: "version X" then "<sha256> <size> <path>" lines.
std::string OrbisManifestPath();
// The app compares its own eboot.bin and sce_module/libc.prx with the manifest and logs the result.
void OrbisVerifyAppFiles();
