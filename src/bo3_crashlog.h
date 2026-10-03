// Debug aid (weapon_tech_crashlog=1 in the cfg, read before install, or WT_CRASHLOG defined): a vectored exception
// handler that writes access violations / illegal instructions to weapon_tech.log synchronously (the async writer may
// not get to run), with the faulting RIP as exe+RVA / weapon_tech+RVA, and exe / DLL return addresses found on the
// stack. First-chance only, so it logs and passes on; it never handles anything. At most 8 reports per process.
#pragma once

namespace
{
	bool g_wtCrashLog;
	HMODULE g_wtSelf;

	void CrashLogAddr(char *out, size_t n, uint64_t a)
	{
		const uint64_t exe = reinterpret_cast<uint64_t>(g_base);
		MODULEINFO mi{};
		if (g_base && a >= exe && a < exe + 0x20000000)
		{
			snprintf(out, n, "exe+%llX", a - exe);
			return;
		}
		if (g_wtSelf && K32GetModuleInformation(GetCurrentProcess(), g_wtSelf, &mi, sizeof(mi)) &&
		    a >= reinterpret_cast<uint64_t>(mi.lpBaseOfDll) && a < reinterpret_cast<uint64_t>(mi.lpBaseOfDll) + mi.SizeOfImage)
		{
			snprintf(out, n, "weapon_tech+%llX", a - reinterpret_cast<uint64_t>(mi.lpBaseOfDll));
			return;
		}
		snprintf(out, n, "%llX", a);
	}

	LONG CALLBACK WtCrashVeh(EXCEPTION_POINTERS *e)
	{
		static volatile LONG s_count;
		const DWORD code = e->ExceptionRecord->ExceptionCode;
		if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
		    code != EXCEPTION_STACK_OVERFLOW)
			return EXCEPTION_CONTINUE_SEARCH;
		if (InterlockedIncrement(&s_count) > 8 || !g_log)
			return EXCEPTION_CONTINUE_SEARCH;
		const CONTEXT *c = e->ContextRecord;
		char rip[64], line[1024];
		CrashLogAddr(rip, sizeof(rip), c->Rip);
		int n = snprintf(line, sizeof(line), "CRASHLOG: exception %08lX at %s (thread %lu) info %llX %llX | rax %llX rbx %llX rcx %llX rdx %llX "
		                 "rsi %llX rdi %llX r8 %llX r9 %llX r14 %llX r15 %llX rsp %llX | stack:",
		                 code, rip, GetCurrentThreadId(), e->ExceptionRecord->NumberParameters > 0 ? e->ExceptionRecord->ExceptionInformation[0] : 0,
		                 e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0, c->Rax, c->Rbx, c->Rcx,
		                 c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9, c->R14, c->R15, c->Rsp);
		const uint64_t *sp = reinterpret_cast<const uint64_t *>(c->Rsp);
		int found = 0;
		for (int i = 0; i < 256 && found < 12 && n < static_cast<int>(sizeof(line)) - 40; i++)
		{
			if (!Readable(sp + i, 8))
				break;
			char a[64];
			CrashLogAddr(a, sizeof(a), sp[i]);
			if (strncmp(a, "exe+", 4) == 0 || strncmp(a, "weapon_tech+", 12) == 0)
			{
				n += snprintf(line + n, sizeof(line) - n, " [%d]%s", i, a);
				found++;
			}
		}
		// Synchronous: the process may be about to die.
		fprintf(g_log, "%s\n", line);
		fflush(g_log);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	void InstallCrashLog(HMODULE self)
	{
		static bool s_done;
		if (s_done)
			return;
		s_done = true;
		g_wtSelf = self;
		AddVectoredExceptionHandler(1, WtCrashVeh);
		Log("weapon_tech: crash log on (weapon_tech_crashlog=1): access violations are written to this log as they happen");
	}
}
