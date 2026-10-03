// Shared BO3 zone/PMem helpers for prt_extend (ReShade add-on) and stub_boot (map-loaded DLL).
// Everything lives in an anonymous namespace: each DLL gets its own copy.
//
// Two exe builds are known, and they're different compiles with different addresses:
//  - "enhanced": the Steam BlackOps3.exe that BO3 Enhanced ships (no Arxan).
//  - "retail":   the stock Steam BlackOps3.exe (Arxan-protected; BlackOps3b.exe on this machine).
// Addresses are found from the shape of the code (Resolve()), so other builds work too while
// that code is recognisable; nothing that needs an address is touched otherwise.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <initializer_list>
#include <share.h>
#include <new>
#include <psapi.h>

namespace
{
	FILE *g_log;
	uint8_t *g_base;  // the exe's load address

	// Log(): a timestamped line to g_log, written and flushed at once. With the async writer on (StartLogWriter(), which
	// weapon_tech.dll uses) the line is only copied into a memory buffer under a short lock, and a writer thread does
	// the file I/O every 100 ms, so the game thread never waits on the disk (a flush per line is a WriteFile per line,
	// ~30 us and at times over a millisecond). Lines that don't fit the buffer are dropped and counted.
	struct LogBuffers
	{
		SRWLOCK lock = SRWLOCK_INIT;
		char buf[2][128 * 1024];
		size_t len[2] = {};
		int cur = 0;
		unsigned dropped = 0;
		bool on = false;
	};
	LogBuffers *g_logAsync;

	void Log(const char *fmt, ...)
	{
		if (!g_log)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		va_list args;
		va_start(args, fmt);
		if (LogBuffers *b = g_logAsync; b && b->on)
		{
			char line[2048];
			int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
			int m = vsnprintf(line + n, sizeof(line) - n - 1, fmt, args);
			n += m < 0 ? 0 : m >= static_cast<int>(sizeof(line)) - n - 1 ? static_cast<int>(sizeof(line)) - n - 2 : m;
			line[n++] = '\n';
			va_end(args);
			AcquireSRWLockExclusive(&b->lock);
			size_t &len = b->len[b->cur];
			if (len + n <= sizeof(b->buf[0]))
			{
				memcpy(b->buf[b->cur] + len, line, n);
				len += n;
			}
			else
				b->dropped++;
			ReleaseSRWLockExclusive(&b->lock);
			return;
		}
		fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		vfprintf(g_log, fmt, args);
		va_end(args);
		fputc('\n', g_log);
		fflush(g_log);
	}

	// Writes out what the game thread logged since the last call (the writer thread; `final` at unload, without waiting
	// for a lock another thread may have died holding).
	void LogDrain(bool final = false)
	{
		LogBuffers *b = g_logAsync;
		if (!b || !g_log)
			return;
		if (final)
		{
			if (!TryAcquireSRWLockExclusive(&b->lock))
				return;
		}
		else
			AcquireSRWLockExclusive(&b->lock);
		const int full = b->cur;
		b->cur ^= 1;
		const unsigned dropped = b->dropped;
		b->dropped = 0;
		ReleaseSRWLockExclusive(&b->lock);
		const bool wrote = b->len[full] != 0;
		if (wrote)
		{
			fwrite(b->buf[full], 1, b->len[full], g_log);
			b->len[full] = 0;
		}
		if (dropped)
			fprintf(g_log, "(log: %u line(s) dropped, the buffer was full)\n", dropped);
		if (wrote || dropped)
			fflush(g_log);
	}

	DWORD WINAPI LogWriterThread(void *)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		for (;;)
		{
			Sleep(100);
			LogDrain();
		}
	}

	// From here on Log() only queues (lines logged before this were written directly). False: no thread, stays synchronous.
	bool StartLogWriter()
	{
		if (g_logAsync || !g_log)
			return g_logAsync != nullptr;
		auto *b = new (std::nothrow) LogBuffers;
		if (!b)
			return false;
		g_logAsync = b;
		HANDLE h = CreateThread(nullptr, 0, LogWriterThread, nullptr, 0, nullptr);
		if (!h)
		{
			g_logAsync = nullptr;
			delete b;
			return false;
		}
		CloseHandle(h);
		b->on = true;
		return true;
	}

	template <typename T>
	T *At(uintptr_t rva) { return reinterpret_cast<T *>(g_base + rva); }

	// ---------------------------------------------------------------------------------------
	// Builds

	// DB_LoadZone picks the PMem stack for a zone from its flags (<0x1000 -> 0, <0x20000 -> 4,
	// else 1) and stores it to a global with a 6-byte "mov [zoneStack], eax". Every address we
	// need is found from the code itself (Resolve()), so a patched exe still works as long as
	// that code keeps its shape. kKnown is what Resolve() found on the builds we've tested: it
	// only names the build, and any disagreement with it is logged.
	struct Build
	{
		const char *name;
		uintptr_t stackStore;   // the mov [zoneStack], eax
		uintptr_t zoneStack;    // int: stack for the zone being loaded
		uintptr_t zoneSlot;     // int: index of the zone being loaded in the zone table
		uintptr_t zoneTable;    // 96-byte zone records, name first
		uintptr_t pmemStacks;   // PMem stacks, 3944 bytes each (0: not found, no pool monitor/move)
		bool arxan;             // Arxan-protected (it adds a second .text section)
		uintptr_t loadXAssets;  // DB_LoadXAssets(XZoneInfo *, count, sync, 0) (0: not found)
	};

	const Build kKnown[] = {
		{"enhanced", 0x14DF4DF, 0xF3B1B0C, 0xF883B64, 0xF882300, 0x18A35700, false, 0x14DE5D0},
		{"retail", 0x1425350, 0x9448B78, 0x942AB2C, 0x9910B80, 0x166EAE10, true, 0x14236C0},
	};

	Build g_found;
	const Build *g_build;
	bool g_routeInstalled;

	bool Readable(const void *p, size_t n)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return false;
		return static_cast<const uint8_t *>(p) + n <= static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
	}

	// Readability from the working set. VirtualQuery also measures the region around the address, walking it page by
	// page: ~1 ms per GB committed here, and BO3's zone memory is several GB, so each call on a weapon variant, a name or
	// an anim tree cost milliseconds in game (the 20-40 ms "slow frame" hitches every 2 s in weapon_tech.log).
	// QueryWorkingSetEx looks at exactly the pages asked about (~0.3 us). A resident page with a readable protection is
	// readable; anything else (paged out, never touched, reserved, free, guard) is left to VirtualQuery.
	// Returns the bytes readable from p through the end of its page, or of the next page too when that one is resident
	// and readable; 0 = can't tell.
	size_t ResidentSpan(const void *p)
	{
		const uintptr_t page = reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(0xFFF);
		PSAPI_WORKING_SET_EX_INFORMATION ws[2] = {};
		ws[0].VirtualAddress = reinterpret_cast<void *>(page);
		ws[1].VirtualAddress = reinterpret_cast<void *>(page + 0x1000);
		if (!K32QueryWorkingSetEx(GetCurrentProcess(), ws, sizeof(ws)))
			return 0;
		auto ok = [](const PSAPI_WORKING_SET_EX_BLOCK &a) {
			const unsigned prot = static_cast<unsigned>(a.Win32Protection);
			return a.Valid && (prot & 0xEE) && !(prot & 0x101);  // READONLY..EXECUTE_WRITECOPY; not NOACCESS / GUARD
		};
		if (!ok(ws[0].VirtualAttributes))
			return 0;
		const size_t first = page + 0x1000 - reinterpret_cast<uintptr_t>(p);
		return ok(ws[1].VirtualAttributes) ? first + 0x1000 : first;
	}

	// Readable() without the region walk when the pages are resident (VirtualQuery otherwise).
	bool FastReadable(const void *p, size_t n)
	{
		return (n <= 0x1000 && ResidentSpan(p) >= n) || Readable(p, n);
	}

	// Readable() for many pointers in one pass: each VirtualQuery region is asked about once, so a walk over the weapon
	// variants costs a handful of calls instead of one or two per variant. Only good for the pass it is made for: zones
	// load and unload between frames, so a cache never outlives the function that made it. Resident pages are answered
	// from the working set first (ResidentSpan), which needs no VirtualQuery at all.
	struct ReadCache
	{
		struct Region
		{
			const uint8_t *lo, *hi;
			bool ok;
		} r[16];
		int n = 0, next = 0;

		// How many bytes from p on are readable: to the end of its region, or through the end of
		// its page when the answer came from the working set (at least 128 bytes, else VirtualQuery); 0 if p isn't readable.
		size_t Span(const void *p)
		{
			const size_t fast = ResidentSpan(p);
			if (fast >= 128)  // a cfg name (< 64) or an xanim name (< 96) and its NUL fit
				return fast;
			return RegionSpan(p);
		}
		bool Readable(const void *p, size_t len) { return (len <= 0x1000 && ResidentSpan(p) >= len) || RegionSpan(p) >= len; }

		// VirtualQuery, cached per region.
		size_t RegionSpan(const void *p)
		{
			auto *b = static_cast<const uint8_t *>(p);
			for (int i = 0; i < n; i++)
				if (b >= r[i].lo && b < r[i].hi)
					return r[i].ok ? static_cast<size_t>(r[i].hi - b) : 0;
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(p, &mbi, sizeof(mbi)))
				return 0;
			Region x = {static_cast<const uint8_t *>(mbi.BaseAddress), static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize,
			            mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))};
			// A region only reaches back to p's page. If the whole allocation up to here is the same region (one zone
			// block), take it from its start, so pointers below p hit too (a pass needn't go in address order).
			MEMORY_BASIC_INFORMATION from;
			if (mbi.AllocationBase && mbi.AllocationBase < mbi.BaseAddress && VirtualQuery(mbi.AllocationBase, &from, sizeof(from)) &&
			    static_cast<const uint8_t *>(from.BaseAddress) + from.RegionSize == x.hi)
				x.lo = static_cast<const uint8_t *>(from.BaseAddress);
			r[next] = x;
			next = (next + 1) % 16;
			n += n < 16;
			return x.ok ? static_cast<size_t>(x.hi - b) : 0;
		}
	};

	struct Section
	{
		uint8_t *start;
		size_t size;
	};

	// The exe's executable sections (at most 8); *texts counts the ones named ".text".
	int ExecSections(Section *out, int *texts)
	{
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_base + reinterpret_cast<IMAGE_DOS_HEADER *>(g_base)->e_lfanew);
		IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
		int n = 0;
		*texts = 0;
		for (int i = 0; i < nt->FileHeader.NumberOfSections && n < 8; i++)
		{
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;
			out[n++] = {g_base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
			*texts += memcmp(sec[i].Name, ".text", 6) == 0;
		}
		return n;
	}

	// If p is a rip-relative mov r32 (8B), lea r64 (8D) or movsxd (REX.W 63), with an optional
	// REX prefix, returns the opcode and sets *target (an RVA). Otherwise 0.
	uint8_t RipRel(const uint8_t *p, uintptr_t *target)
	{
		uint8_t rex = 0;
		if (*p >= 0x40 && *p <= 0x4F)
			rex = *p++;
		uint8_t op = p[0];
		if ((op != 0x8B && op != 0x8D && op != 0x63) || (p[1] & 0xC7) != 0x05 || (op == 0x63 && !(rex & 8)))
			return 0;
		int32_t disp;
		memcpy(&disp, p + 2, 4);
		*target = static_cast<uintptr_t>(p + 6 + disp - g_base);
		return op;
	}

	// True when the instruction at insnRva (len bytes) refers to targetRva, either rip-relative or as an image-base
	// relative disp32 ([reg+reg*n+rva], the form BO3 uses for its big arrays). A one-time shape check that ties a fixed
	// data constant to the code that uses it (a readability check passes for any address in .data).
	bool CodeRefers(uintptr_t insnRva, size_t len, uintptr_t targetRva)
	{
		const uint8_t *p = At<uint8_t>(insnRva);
		if (!FastReadable(p, len))
			return false;
		for (size_t i = 0; i + 4 <= len; i++)
		{
			int32_t d;
			memcpy(&d, p + i, 4);
			if (static_cast<uint32_t>(d) == targetRva || static_cast<uintptr_t>(p + len + d - g_base) == targetRva)
				return true;
		}
		return false;
	}

	// True when the code at rva is exactly these bytes (an instruction whose displacement is a layout offset we rely on).
	bool CodeMatches(uintptr_t rva, const uint8_t *bytes, size_t n)
	{
		const uint8_t *p = At<uint8_t>(rva);
		return FastReadable(p, n) && memcmp(p, bytes, n) == 0;
	}

	const uint8_t *Find(const uint8_t *lo, const uint8_t *hi, const uint8_t *pat, size_t n)
	{
		for (const uint8_t *p = lo; p + n <= hi; p++)
			if (*p == pat[0] && memcmp(p, pat, n) == 0)
				return p;
		return nullptr;
	}

	// Start of the function containing rva, following .pdata chained-unwind entries back to the
	// primary one (big functions are split into several). 0 if rva isn't in a function.
	uintptr_t FuncStart(uintptr_t rva)
	{
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_base + reinterpret_cast<IMAGE_DOS_HEADER *>(g_base)->e_lfanew);
		const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		auto *fns = At<const RUNTIME_FUNCTION>(dir.VirtualAddress);
		size_t lo = 0, hi = dir.Size / sizeof(RUNTIME_FUNCTION);
		while (lo < hi)
		{
			size_t mid = (lo + hi) / 2;
			if (fns[mid].EndAddress <= rva)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo >= dir.Size / sizeof(RUNTIME_FUNCTION) || fns[lo].BeginAddress > rva)
			return 0;
		const RUNTIME_FUNCTION *f = &fns[lo];
		for (int depth = 0; depth < 16; depth++)
		{
			const uint8_t *unwind = At<uint8_t>(f->UnwindData);
			if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO))
				return f->BeginAddress;
			f = reinterpret_cast<const RUNTIME_FUNCTION *>(unwind + 4 + 2 * ((unwind[2] + 1) & ~1));
		}
		return 0;
	}

	int CountCallers(const Section *secs, int n, uintptr_t target)
	{
		int count = 0;
		for (int i = 0; i < n; i++)
			for (const uint8_t *p = secs[i].start; p + 5 <= secs[i].start + secs[i].size; p++)
			{
				if (*p != 0xE8)
					continue;
				int32_t rel;
				memcpy(&rel, p + 1, 4);
				count += static_cast<uintptr_t>(p + 5 + rel - g_base) == target;
			}
		return count;
	}

	// DB_LoadXAssets: the function that narrows free flags to their bands (0x1F000 and
	// 0x3FE0000 both appear in it) and has many direct callers. Another function holds both
	// constants too, but it has a single caller.
	uintptr_t FindLoadXAssets(const Section *secs, int n)
	{
		static const uint8_t k1F000[] = {0x00, 0xF0, 0x01, 0x00};
		static const uint8_t k3FE0000[] = {0x00, 0x00, 0xFE, 0x03};
		uintptr_t with1F000[16];
		int count1F000 = 0;
		for (int i = 0; i < n; i++)
		{
			const uint8_t *end = secs[i].start + secs[i].size;
			for (const uint8_t *p = secs[i].start; (p = Find(p, end, k1F000, 4)) != nullptr && count1F000 < 16; p++)
				if (uintptr_t f = FuncStart(p - g_base))
				{
					bool seen = false;
					for (int j = 0; j < count1F000; j++)
						seen |= with1F000[j] == f;
					if (!seen)
						with1F000[count1F000++] = f;
				}
		}
		bool withBoth[16] = {};
		for (int i = 0; i < n; i++)
		{
			const uint8_t *end = secs[i].start + secs[i].size;
			for (const uint8_t *p = secs[i].start; (p = Find(p, end, k3FE0000, 4)) != nullptr; p++)
			{
				uintptr_t f = FuncStart(p - g_base);
				for (int j = 0; j < count1F000; j++)
					withBoth[j] |= with1F000[j] == f;
			}
		}
		uintptr_t best = 0;
		int bestCallers = 4;  // the real one has well over this many
		for (int j = 0; j < count1F000; j++)
			if (withBoth[j])
			{
				int callers = CountCallers(secs, n, with1F000[j]);
				if (callers > bestCallers)
				{
					best = with1F000[j];
					bestCallers = callers;
				}
			}
		return best;
	}

	// Finds every address in b from the code's shape. FALSE if the stack-selection code isn't
	// there exactly once, or the zone lookup after it is missing.
	bool Resolve(Build &b)
	{
		static const uint8_t kCmp1000[] = {0x81, 0xF9, 0x00, 0x10, 0x00, 0x00};   // cmp ecx, 1000h
		static const uint8_t kCmp20000[] = {0x81, 0xF9, 0x00, 0x00, 0x02, 0x00};  // cmp ecx, 20000h
		static const uint8_t kStore[] = {0x89, 0x05};                             // mov [rip+x], eax
		static const uint8_t kTest[] = {0x00, 0x0C, 0x80, 0x03};                  // test r, 3800C00h
		static const uint8_t kStride[] = {0x68, 0x0F, 0x00, 0x00};                // imul r, r, 0F68h

		Section secs[8];
		int texts;
		int n = ExecSections(secs, &texts);
		b.arxan = texts > 1;

		// 1. The stack store: cmp 1000h, cmp 20000h, the store, then a test of the zone flags.
		// If stub_boot's InstallRoute already ran in this process (weapon_tech loads after it), the
		// store is "jmp <trampoline>; nop" instead, and the store address sits in the trampoline.
		static const uint8_t kTrampHead[] = {0x50, 0x51, 0x52, 0x41, 0x50};  // push rax rcx rdx r8
		const uint8_t *site = nullptr;
		uintptr_t routedStore = 0;
		int sites = 0;
		for (int i = 0; i < n; i++)
		{
			const uint8_t *end = secs[i].start + secs[i].size;
			for (const uint8_t *p = secs[i].start; (p = Find(p, end, kCmp1000, 6)) != nullptr; p++)
			{
				const uint8_t *c = Find(p + 6, (std::min)(p + 40, end), kCmp20000, 6);
				if (!c)
					continue;
				const uint8_t *s = Find(c + 6, (std::min)(c + 18, end), kStore, 2);
				uintptr_t store = 0;
				if (!s)
					for (const uint8_t *q = c + 6; q + 2 <= (std::min)(c + 18, end) && q + 6 <= end; q++)
						if (q[0] == 0xE9 && q[5] == 0x90)
						{
							int32_t rel;
							memcpy(&rel, q + 1, 4);
							const uint8_t *tramp = q + 5 + rel;
							if (Readable(tramp, 78) && memcmp(tramp, kTrampHead, sizeof(kTrampHead)) == 0 &&
							    tramp[68] == 0x48 && tramp[69] == 0xB9)
							{
								memcpy(&store, tramp + 70, 8);
								s = q;
								break;
							}
						}
				if (s && Find(s + 6, (std::min)(s + 22, end), kTest, 4))
				{
					site = s;
					routedStore = store;
					sites++;
				}
			}
		}
		if (sites != 1)
		{
			Log("resolve: found the stack-selection code %d times, expected once", sites);
			return false;
		}
		b.stackStore = site - g_base;
		if (routedStore)
		{
			b.zoneStack = routedStore - reinterpret_cast<uintptr_t>(g_base);
			Log("resolve: stack store already routed (stub_boot); zoneStack +%zx read from its trampoline", b.zoneStack);
		}
		else
		{
			int32_t disp;
			memcpy(&disp, site + 2, 4);
			b.zoneStack = site + 6 + disp - g_base;
		}

		// 2. Right after the store the zone's name is looked up: the first lea (the zone table)
		// and the first mov (the slot) that aren't zoneStack again.
		b.zoneSlot = b.zoneTable = 0;
		for (const uint8_t *p = site + 6; p < site + 0x100 && !(b.zoneSlot && b.zoneTable); p++)
		{
			uintptr_t t;
			uint8_t op = RipRel(p, &t);
			if (!op || t == b.zoneStack)
				continue;
			if (op == 0x8D && !b.zoneTable)
				b.zoneTable = t;
			else if (op == 0x8B && !b.zoneSlot)
				b.zoneSlot = t;
		}
		if (!b.zoneSlot || !b.zoneTable)
		{
			Log("resolve: no zone table/slot after the stack store at +%zx", b.stackStore);
			return false;
		}

		// 3. PMem stacks: the base that "imul r, r, 0F68h" (the per-stack stride) indexes,
		// loaded by the nearest lea on either side of it. The most common base wins.
		struct Vote
		{
			uintptr_t target;
			int count;
		} votes[16] = {};
		for (int i = 0; i < n; i++)
		{
			const uint8_t *end = secs[i].start + secs[i].size;
			for (const uint8_t *p = secs[i].start + 2; (p = Find(p, end, kStride, 4)) != nullptr; p++)
			{
				if (p[-2] != 0x69 || (p[-1] & 0xC0) != 0xC0)
					continue;
				uintptr_t best = 0;
				ptrdiff_t bestDist = PTRDIFF_MAX;
				for (const uint8_t *q = (std::max)(p - 20, static_cast<const uint8_t *>(secs[i].start)); q < (std::min)(p + 24, end - 7); q++)
				{
					uintptr_t t;
					ptrdiff_t dist = q > p ? q - p : p - q;
					if (dist < bestDist && RipRel(q, &t) == 0x8D)
					{
						best = t;
						bestDist = dist;
					}
				}
				for (Vote &v : votes)
					if (best && (v.target == best || !v.target))
					{
						v.target = best;
						v.count++;
						break;
					}
			}
		}
		b.pmemStacks = 0;
		int most = 0;
		for (const Vote &v : votes)
			if (v.count > most)
			{
				most = v.count;
				b.pmemStacks = v.target;
			}
		// Once PMem is up, stack 0's pool 2 record is the "gpu prt" one. (Early on, e.g. when the
		// ReShade add-on loads, there are no records yet: nothing to check.)
		if (b.pmemStacks)
		{
			const uint8_t *pool = *At<const uint8_t *>(b.pmemStacks + 3904 + 8 * 2);
			const char *name = pool && Readable(pool, 16) ? *reinterpret_cast<const char *const *>(pool + 8) : nullptr;
			if (pool && (!name || !Readable(name, 8) || !strstr(name, "prt")))
			{
				Log("resolve: PMem stacks candidate +%zx failed its check; no pool monitor/move", b.pmemStacks);
				b.pmemStacks = 0;
			}
		}

		// 4. DB_LoadXAssets (optional: only zone preloading needs it).
		b.loadXAssets = FindLoadXAssets(secs, n);
		return true;
	}

	// Finds this exe's addresses (once). FALSE: its code isn't recognisable, touch nothing.
	bool KnownBuild()
	{
		static bool s_tried;
		if (g_build || s_tried)
			return g_build != nullptr;
		s_tried = true;
		g_base = reinterpret_cast<uint8_t *>(GetModuleHandleA(nullptr));
		if (!Resolve(g_found))
			return false;
		g_found.name = nullptr;
		for (const Build &k : kKnown)
		{
			if (k.stackStore != g_found.stackStore)
				continue;
			g_found.name = k.name;
			if (k.zoneStack != g_found.zoneStack || k.zoneSlot != g_found.zoneSlot || k.zoneTable != g_found.zoneTable ||
			    k.pmemStacks != g_found.pmemStacks || k.loadXAssets != g_found.loadXAssets)
				Log("resolve: differs from the %s table (found zoneStack +%zx slot +%zx table +%zx pmem +%zx loadXAssets +%zx); using what was found",
				    k.name, g_found.zoneStack, g_found.zoneSlot, g_found.zoneTable, g_found.pmemStacks, g_found.loadXAssets);
		}
		if (!g_found.name)
		{
			g_found.name = "unrecognised (relocated)";
			Log("resolve: new build: stack store +%zx, zoneStack +%zx, slot +%zx, table +%zx, pmem +%zx, loadXAssets +%zx, arxan %d",
			    g_found.stackStore, g_found.zoneStack, g_found.zoneSlot, g_found.zoneTable, g_found.pmemStacks,
			    g_found.loadXAssets, g_found.arxan);
		}
		g_build = &g_found;
		return true;
	}

	// ---------------------------------------------------------------------------------------
	// PMem "gpu prt" pools (known on the enhanced build only)

	constexpr uintptr_t kStackStride = 3944;
	constexpr int kPoolPrt = 2;                     // "gpu prt"
	constexpr uint64_t kStockPrtSize = 0x480000000; // 18 GiB
	// Committed pages are tracked by a 19-bit 64 KiB page index from the pool base
	// (Mem_Internal_MapPage), so a pool can't usefully exceed 32 GiB.
	constexpr uint64_t kExtendBy = 0x800000000 - kStockPrtSize; // 18 GiB -> 32 GiB

	// PMem pool record: +0 type, +8 name, +16 base, +24 size (reserve-only for gpu prt)
	struct PoolRecord
	{
		uint32_t type;
		uint32_t pad;
		const char *name;
		uint8_t *base;
		uint64_t size;
	};

	PoolRecord *PrtPool(int stack)
	{
		if (!g_build || !g_build->pmemStacks)
			return nullptr;
		return *At<PoolRecord *>(g_build->pmemStacks + kStackStride * stack + 3904 + 8 * kPoolPrt);
	}

	uint64_t PrtUsed(int stack)
	{
		return *At<uint64_t>(g_build->pmemStacks + kStackStride * stack + 16 + 8 * kPoolPrt);
	}

	void Extend(int stack)
	{
		PoolRecord *pool = PrtPool(stack);
		if (!pool || !Readable(pool, sizeof(*pool)) || !pool->base)
			return;
		if (pool->size != kStockPrtSize)
		{
			Log("stack %d gpu prt size is %llx, not stock 18 GiB; leaving it alone", stack, pool->size);
			return;
		}
		// Nothing has been placed in the pool yet (add-ons load before any zone), so move it
		// to a bigger reservation. PMem_Alloc reads base and size from this record on every
		// call; the old 18 GiB reservation is left behind as unused address space.
		if (PrtUsed(stack) == 0)
		{
			uint64_t size = kStockPrtSize + kExtendBy;
			if (void *base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS))
			{
				Log("stack %d gpu prt moved %p -> %p: 18 GiB -> %.1f GiB", stack, pool->base, base, size / 1073741824.0);
				pool->base = static_cast<uint8_t *>(base);
				pool->size = size;
				return;
			}
		}
		// Otherwise try to grow in place with a second reservation directly after it.
		for (uint64_t extra = kExtendBy; extra >= 0x40000000; extra >>= 1)
		{
			uint8_t *want = pool->base + pool->size;
			void *got = VirtualAlloc(want, extra, MEM_RESERVE, PAGE_NOACCESS);
			if (got == want)
			{
				pool->size += extra;
				Log("stack %d gpu prt %p: 18 GiB -> %.1f GiB", stack, pool->base, pool->size / 1073741824.0);
				return;
			}
			if (got)
				VirtualFree(got, 0, MEM_RELEASE);
		}
		Log("stack %d gpu prt %p: %.3f GiB already used and the address space after it is taken, not extended",
			stack, pool->base, PrtUsed(stack) / 1073741824.0);
	}

	// ---------------------------------------------------------------------------------------
	// Usermap list entries
	//
	// Each usermap/workshop map is an entry in a list the game builds (enhanced: 1212-byte
	// entries, retail: 1224). The fields used here sit at the same offsets on both:
	// +0 name, +100 internalName (the zone `map` loads), +132 ugcName (what `map <name>`
	// matches), +940 folder every .ff/.xpak/sound/video path is built from (260 chars).
	// Retail doesn't reference the list with plain rip-relative code, so instead of a fixed
	// address the entry is found by scanning the exe's writable data for it.

	constexpr size_t kFolderCap = 260;

	// The mods list has entries with the same layout; a mod named like the map (fs_game <map>
	// creates an empty mods\<map>\) must not be mistaken for the usermap.
	bool LooksLikeEntry(const char *e, const char *ugc)
	{
		if (strcmp(e, ugc) != 0 || strcmp(e + 132, ugc) != 0 || !e[940] || !memchr(e + 940, 0, kFolderCap))
			return false;
		const char *folder = e + 940;
		if (!strchr(folder, '/') && !strchr(folder, '\\'))
			return false;
		return !strstr(folder, "/mods/") && !strstr(folder, "\\mods\\") && !strstr(folder, "\\mods/");
	}

	// Every matching usermap entry (normally one). Cached, and re-checked before each use in case
	// the game rebuilt its list.
	constexpr int kMaxEntries = 4;

	int FindUsermapEntries(const char *ugc, char **out)
	{
		static char *cached[kMaxEntries];
		static int cachedCount;
		static char cachedFor[64];
		if (cachedCount && strcmp(cachedFor, ugc) == 0)
		{
			bool valid = true;
			for (int i = 0; i < cachedCount; i++)
				valid &= LooksLikeEntry(cached[i], ugc);
			if (valid)
			{
				memcpy(out, cached, sizeof(char *) * cachedCount);
				return cachedCount;
			}
		}
		cachedCount = 0;
		size_t n = strlen(ugc) + 1;
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_base + reinterpret_cast<IMAGE_DOS_HEADER *>(g_base)->e_lfanew);
		uint8_t *end = g_base + nt->OptionalHeader.SizeOfImage;
		MEMORY_BASIC_INFORMATION mbi;
		for (uint8_t *p = g_base; p < end && VirtualQuery(p, &mbi, sizeof(mbi)); p = static_cast<uint8_t *>(mbi.BaseAddress) + mbi.RegionSize)
		{
			// Retail's Arxan leaves image sections execute+read+write, so those count too.
			bool writable = mbi.State == MEM_COMMIT &&
			                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
			                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
			if (!writable)
				continue;
			uint8_t *lo = static_cast<uint8_t *>(mbi.BaseAddress);
			uint8_t *hi = lo + mbi.RegionSize;
			if (hi > end)
				hi = end;
			for (uint8_t *q = lo; q + 940 + kFolderCap <= hi && cachedCount < kMaxEntries; q += 4)
			{
				if (*q != static_cast<uint8_t>(ugc[0]) || memcmp(q, ugc, n) != 0)
					continue;
				if (LooksLikeEntry(reinterpret_cast<char *>(q), ugc))
					cached[cachedCount++] = reinterpret_cast<char *>(q);
			}
		}
		strncpy_s(cachedFor, ugc, _TRUNCATE);
		memcpy(out, cached, sizeof(char *) * cachedCount);
		return cachedCount;
	}

	// Result of redirecting a usermap: how many entries changed now, and how many already were.
	struct Applied
	{
		int now, already;
	};

	// Alias mode (separate stub usermap): point the stub's internalName at another zone in
	// its folder, so `map <stub>` loads that zone.
	char g_aliasUgc[32];
	char g_aliasInternal[32];

	Applied ApplyAlias()
	{
		Applied r = {};
		char *entries[kMaxEntries];
		int n = FindUsermapEntries(g_aliasUgc, entries);
		for (int i = 0; i < n; i++)
		{
			char *entry = entries[i];
			if (strcmp(entry + 100, g_aliasInternal) == 0)
			{
				r.already++;
				continue;
			}
			Log("alias: usermap '%s' (%s) now loads zone '%s'", entry + 132, entry + 940, g_aliasInternal);
			strcpy_s(entry + 100, 32, g_aliasInternal);
			r.now++;
		}
		return r;
	}

	// Folder mode (stubpack layout): the usermap's zone folder holds a tiny stub map with the
	// same name, and the real map sits in a subfolder. Appending "/<subfolder>" to the entry's
	// folder makes every .ff/.xpak/sound/video path resolve to the real map instead.
	char g_folderUgc[32];
	char g_folderSub[32];

	Applied ApplyFolder()
	{
		Applied r = {};
		char *entries[kMaxEntries];
		int n = FindUsermapEntries(g_folderUgc, entries);
		size_t subLen = strlen(g_folderSub);
		for (int i = 0; i < n; i++)
		{
			char *folder = entries[i] + 940;
			size_t len = strlen(folder);
			if (len > subLen && folder[len - subLen - 1] == '/' && strcmp(folder + len - subLen, g_folderSub) == 0)
			{
				r.already++;
				continue;
			}
			if (len + 1 + subLen >= kFolderCap)
				continue;
			folder[len] = '/';
			strcpy_s(folder + len + 1, kFolderCap - len - 1, g_folderSub);
			Log("folder: usermap '%s' now loads from %s", entries[i] + 132, folder);
			r.now++;
		}
		return r;
	}

	// Called (from the monitor thread) when a zone reaches state 3, i.e. finished loading.
	void (*g_onZoneLoaded)(const char *name);

	// Logs every zone-table change: 96-byte records, name at +0, flags at +64, state at +88
	// (1 loading, 2 loaded, 3 done, -1 unloading).
	void WatchZones()
	{
		constexpr int kSlots = 64;
		static char lastName[kSlots][40];
		static uint32_t lastState[kSlots];
		// The table is in the exe's .data, which stays mapped: once all of it has been readable, it always is (one
		// VirtualQuery per pass instead of 64, 50 times a second).
		static bool s_tableOk;
		if (!s_tableOk)
			s_tableOk = Readable(At<char>(g_build->zoneTable), 96 * kSlots);
		for (int i = 0; i < kSlots; i++)
		{
			const char *rec = At<char>(g_build->zoneTable + 96 * i);
			if (!s_tableOk && !Readable(rec, 96))
				return;
			uint32_t flags = *reinterpret_cast<const uint32_t *>(rec + 64);
			uint32_t state = *reinterpret_cast<const uint32_t *>(rec + 88);
			char name[40];
			strncpy_s(name, rec, _TRUNCATE);
			if (state == lastState[i] && strcmp(name, lastName[i]) == 0)
				continue;
			Log("zone slot %d: '%s' flags %x state %u (was '%s' state %u)", i, name, flags, state, lastName[i], lastState[i]);
			strcpy_s(lastName[i], name);
			lastState[i] = state;
			if (state == 3 && g_onZoneLoaded)
				g_onZoneLoaded(name);
		}
	}

	DWORD WINAPI Monitor(void *)
	{
		uint64_t last[5] = {};
		for (;;)
		{
			WatchZones();
			// The list is rebuilt whenever the game refreshes usermaps, so keep re-applying.
			if (g_aliasUgc[0])
				ApplyAlias();
			if (g_folderUgc[0])
				ApplyFolder();
			for (int stack = 0; stack < 5; stack++)
			{
				PoolRecord *pool = PrtPool(stack);
				if (!pool || !Readable(pool, sizeof(*pool)) || !pool->base)
					continue;
				uint64_t used = PrtUsed(stack);
				if (used != last[stack])
				{
					last[stack] = used;
					Log("stack %d gpu prt used %.3f / %.1f GiB", stack, used / 1073741824.0, pool->size / 1073741824.0);
				}
			}
			Sleep(20);
		}
	}

	// ---------------------------------------------------------------------------------------
	// Zone stack routing. Level zones share stack 0 with core_common; stack 4 is otherwise
	// unused in-game, so routing the map there gives it a gpu prt pool of its own. Unload
	// frees by zone name across every stack, so nothing else needs to know.

	constexpr int kRouteStack = 4;
	char g_routeZone[64];

	extern "C" int RouteZoneStack(int stack)
	{
		const char *name = At<char>(g_build->zoneTable + 96 * *At<uint32_t>(g_build->zoneSlot));
		for (int i = 0;; i++)
		{
			if (name[i] != g_routeZone[i])
				return stack;
			if (!name[i])
				break;
		}
		Log("routing zone '%s' from stack %d to stack %d", name, stack, kRouteStack);
		return kRouteStack;
	}

	void *AllocNear(uintptr_t target, size_t size)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		for (uintptr_t addr = target - 0x10000000; addr > target - 0x70000000; addr -= si.dwAllocationGranularity)
			if (void *p = VirtualAlloc(reinterpret_cast<void *>(addr), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
				return p;
		return nullptr;
	}

	bool InstallRoute()
	{
		if (g_routeInstalled)
			return true;
		if (!KnownBuild())
		{
			Log("route: unknown BlackOps3.exe build; not hooking");
			return false;
		}
		uint8_t *site = At<uint8_t>(g_build->stackStore);
		uint8_t *tramp = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!tramp)
		{
			Log("route: no trampoline memory near the exe");
			return false;
		}
		static const uint8_t code[] = {
			0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, // push rax rcx rdx r8-r11
			0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                         // sub rsp, 0x88
			0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,                               // movdqu [rsp+20h], xmm0
			0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,                               // movdqu [rsp+30h], xmm1
			0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,                               // movdqu [rsp+40h], xmm2
			0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,                               // movdqu [rsp+50h], xmm3
			0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,                               // movdqu [rsp+60h], xmm4
			0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,                               // movdqu [rsp+70h], xmm5
			0x89, 0xC1,                                                       // mov ecx, eax
			0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               // mov rax, RouteZoneStack
			0xFF, 0xD0,                                                       // call rax
			0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,                               // mov rcx, &zoneStack
			0x89, 0x01,                                                       // mov [rcx], eax
			0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,                               // movdqu xmm0, [rsp+20h]
			0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
			0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,
			0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
			0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60,
			0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
			0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                         // add rsp, 0x88
			0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, // pop r11-r8 rdx rcx rax
			0xFF, 0x25, 0, 0, 0, 0,                                           // jmp [rip]
			0, 0, 0, 0, 0, 0, 0, 0,                                           // -> the instruction after the store
		};
		memcpy(tramp, code, sizeof(code));
		uint64_t fn = reinterpret_cast<uint64_t>(&RouteZoneStack);
		uint64_t store = reinterpret_cast<uint64_t>(At<uint8_t>(g_build->zoneStack));
		uint64_t back = reinterpret_cast<uint64_t>(site + 6);
		memcpy(tramp + 58, &fn, 8);
		memcpy(tramp + 70, &store, 8);
		memcpy(tramp + sizeof(code) - 8, &back, 8);

		// The trampoline stores the (possibly rerouted) stack itself, so the original 6-byte
		// store becomes a jmp to it plus a nop.
		uint8_t patch[6] = {0xE9, 0, 0, 0, 0, 0x90};
		int32_t rel = static_cast<int32_t>(tramp - (site + 5));
		memcpy(patch + 1, &rel, 4);
		DWORD old;
		VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &old);
		memcpy(site, patch, sizeof(patch));
		VirtualProtect(site, sizeof(patch), old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
		g_routeInstalled = true;
		Log("route: zones named '%s' go to stack %d (%s build, trampoline %p)", g_routeZone, kRouteStack, g_build->name, tramp);
		return true;
	}

	// ---------------------------------------------------------------------------------------

	// Config lines: "extend=0" disables the pool move, "route=<zone>" sends a zone to stack 4,
	// "folder=<usermap>=<subfolder>" loads the usermap from that subfolder of its zone folder,
	// "alias=<usermap>=<zone>" makes `map <usermap>` load <zone> from that usermap's folder.
	bool ReadConfig(const char *path)
	{
		FILE *f = fopen(path, "r");
		if (!f)
			return true;
		char line[128];
		bool enabled = true;
		while (fgets(line, sizeof(line), f))
		{
			line[strcspn(line, "\r\n")] = 0;
			if (strcmp(line, "extend=0") == 0)
				enabled = false;
			else if (strncmp(line, "route=", 6) == 0)
				strncpy_s(g_routeZone, line + 6, _TRUNCATE);
			else if (strncmp(line, "folder=", 7) == 0)
			{
				if (char *eq = strchr(line + 7, '='))
				{
					*eq = 0;
					strncpy_s(g_folderUgc, line + 7, _TRUNCATE);
					strncpy_s(g_folderSub, eq + 1, _TRUNCATE);
				}
			}
			else if (strncmp(line, "alias=", 6) == 0)
			{
				if (char *eq = strchr(line + 6, '='))
				{
					*eq = 0;
					strncpy_s(g_aliasUgc, line + 6, _TRUNCATE);
					strncpy_s(g_aliasInternal, eq + 1, _TRUNCATE);
				}
			}
		}
		fclose(f);
		return enabled;
	}
}
