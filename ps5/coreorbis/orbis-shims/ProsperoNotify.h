// Mupen64Plus PS5: PS5 toast notifications (the kernel toast, as in PS5SX2's ProsperoNotify).
// SPDX-License-Identifier: MIT
#pragma once

// Queues a toast; a worker thread sends it, so the emulation never waits on the system UI.
void ProsperoNotify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Sends whatever is queued and stops the worker (called before the payload exits).
void ProsperoNotifyFlush();
