// Mupen64Plus PS5: the crash printer and stage markers (ProsperoCrash.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoCrash.h"

#include "OrbisPaths.h"

#if defined(__linux__)
#define _GNU_SOURCE 1
#include <sys/ucontext.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

// An anchor the handler reports addresses against: rip - &n64ps5_crash_anchor + (its vaddr in mupen64plus-pie.elf,
// from `nm`) is the ELF address llvm-addr2line wants. extern "C" so nm finds it by a plain name.
extern "C" __attribute__((noinline, used)) void n64ps5_crash_anchor()
{
	__asm__ volatile("");
}

namespace crashlog
{
namespace
{
std::atomic<const char*> g_stage[int(Area::Count)] = {};
std::atomic<uintptr_t> g_anchor{0};
char g_sigstack[128 * 1024];

// Signal-safe-ish line out: to the open boot.log fd and to stdout (the ELF loader's console). snprintf is
// not strictly async-signal-safe, but it touches no locks here and PS5SX2 prints from its handler the same
// way; getting the line out matters more than purity.
void Emit(const char* s)
{
	const size_t n = strlen(s);
	const int fd = OrbisLogFd();
	if (fd >= 0)
	{
		ssize_t r = write(fd, s, n);
		(void)r;
	}
	ssize_t r2 = write(1, s, n);
	(void)r2;
}

void EmitLine(const char* s)
{
	Emit(s);
	Emit("\n");
}

const char* SignalName(int sig)
{
	switch (sig)
	{
		case SIGSEGV: return "SIGSEGV";
		case SIGBUS: return "SIGBUS";
		case SIGILL: return "SIGILL";
		case SIGFPE: return "SIGFPE";
		case SIGABRT: return "SIGABRT";
		case SIGTRAP: return "SIGTRAP";
		default: return "signal";
	}
}

bool PlausibleText(uintptr_t v)
{
	// our eboot's text and the system modules, so the list is return addresses, not data
	return (v >= 0x10000 && v < 0x100000000ULL) || (v >= 0x200000000ULL && v < 0x900000000ULL);
}

void Handler(int sig, siginfo_t* info, void* ctx)
{
	static std::atomic<int> entered{0};
	if (entered.fetch_add(1) != 0)
		_exit(1); // a fault inside the handler: don't loop

	const uintptr_t anchor = g_anchor.load();
	uintptr_t rip = 0, rsp = 0, rbp = 0;
	if (ctx)
	{
		ucontext_t* uc = static_cast<ucontext_t*>(ctx);
#if defined(__FreeBSD__) || defined(__PROSPERO__)
		rip = uintptr_t(uc->uc_mcontext.mc_rip);
		rsp = uintptr_t(uc->uc_mcontext.mc_rsp);
		rbp = uintptr_t(uc->uc_mcontext.mc_rbp);
#elif defined(__linux__)
		rip = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
		rsp = uintptr_t(uc->uc_mcontext.gregs[REG_RSP]);
		rbp = uintptr_t(uc->uc_mcontext.gregs[REG_RBP]);
#endif
	}
	const void* fault = info ? info->si_addr : nullptr;

	char buf[512];
	EmitLine("");
	EmitLine("== CRASH =======================================================");
	snprintf(buf, sizeof(buf), "signal %d (%s), fault address %p, tid %llu", sig, SignalName(sig), fault,
		(unsigned long long)pthread_self());
	EmitLine(buf);
	snprintf(buf, sizeof(buf), "rip=%#lx rsp=%#lx rbp=%#lx", (unsigned long)rip, (unsigned long)rsp,
		(unsigned long)rbp);
	EmitLine(buf);
	if (anchor && rip)
	{
		snprintf(buf, sizeof(buf), "rip is anchor%+ld  (llvm-addr2line -e mupen64plus-pie.elf -f  <anchor-vaddr %+ld>)",
			long(rip - anchor), long(rip - anchor));
		EmitLine(buf);
	}
	static const char* const kNames[] = {"boot", "shelf", "cover", "pool", "emu"};
	for (int i = 0; i < int(Area::Count); i++)
	{
		const char* s = g_stage[i].load();
		snprintf(buf, sizeof(buf), "stage[%s] = %s", kNames[i], s ? s : "(none)");
		EmitLine(buf);
	}

	// Frame-pointer walk, then a raw scan, each address as anchor+off so it maps into the pie ELF.
	EmitLine("backtrace (ret = anchor + off):");
	uintptr_t fp = rbp;
	for (int i = 0; i < 24 && fp >= 0x10000 && (fp & 7) == 0; i++)
	{
		const uintptr_t* f = reinterpret_cast<const uintptr_t*>(fp);
		const uintptr_t ret = f[1];
		if (PlausibleText(ret) && anchor)
		{
			snprintf(buf, sizeof(buf), "  bt%-2d ret=%#lx  off=%+ld", i, (unsigned long)ret, long(ret - anchor));
			EmitLine(buf);
		}
		const uintptr_t next = f[0];
		if (next <= fp)
			break;
		fp = next;
	}
	if (rsp >= 0x10000 && (rsp & 7) == 0)
	{
		const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(rsp);
		for (int i = 0, shown = 0; i < 256 && shown < 24; i++)
			if (PlausibleText(sp[i]) && anchor)
			{
				snprintf(buf, sizeof(buf), "  st[%-3d] =%#lx  off=%+ld", i, (unsigned long)sp[i], long(sp[i] - anchor));
				EmitLine(buf);
				shown++;
			}
	}
	EmitLine("== END CRASH ===================================================");
	EmitLine("");
	_exit(1);
}
} // namespace

void Install()
{
	g_anchor.store(reinterpret_cast<uintptr_t>(&n64ps5_crash_anchor));
	OrbisLog("[crash] handler armed; anchor n64ps5_crash_anchor = %#lx (map an off= with: llvm-addr2line-18 -e "
			 "build/app/mupen64plus-pie.elf -f  $((<anchor vaddr from nm> + off)))",
		(unsigned long)reinterpret_cast<uintptr_t>(&n64ps5_crash_anchor));

	signal(SIGPIPE, SIG_IGN);

	// On the host the sanitizer owns the signal handlers and its own alternate stacks; leave them be (the
	// stage markers above still work, and ASan reports faults). Arm our handler only on the console.
#if !defined(__linux__)
	stack_t ss = {};
	ss.ss_sp = g_sigstack;
	ss.ss_size = sizeof(g_sigstack);
	ss.ss_flags = 0;
	sigaltstack(&ss, nullptr);

	struct sigaction sa = {};
	sa.sa_sigaction = Handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP})
		sigaction(sig, &sa, nullptr);
#else
	(void)g_sigstack;
	(void)&Handler;
#endif
}

void Stage(Area area, const char* where)
{
	g_stage[int(area)].store(where, std::memory_order_relaxed);
}
} // namespace crashlog
