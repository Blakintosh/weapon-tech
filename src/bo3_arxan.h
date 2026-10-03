// Arxan integrity-check neutralisation for the retail exe, so stub_boot's route patch survives.
// Port of T7Overcharged's components\arxan.cpp (itself BOiii's approach) with no asmjit: the one
// stub it needs is hand-assembled below.
//
// Each check ends by storing the checksum it computed, then counting down:
//     mov [rdx+rcx*4], eax        ; 89 04 8A
//     add dword ptr [rbp+X], -1   ; 83 45 X FF
// Those 7 bytes become `push X ; call stub`. The stub finds the check's {computed, original}
// pointer pair in the caller's frame, stores the original checksum instead, then does the
// `add` itself, so the check always passes whatever we patched.
//
// Retail (PE timestamp 0x693D731E) had ~1000 of these and none of the "split" form
// (89 04 8A E9); split sites are only counted.
//
// Once per process, not once per DLL: stub_boot.dll and weapon_tech.dll each carry this code, and
// only one of them may patch (a second pass would find no unpatched sites, report failure, and the
// caller would refuse to patch code it could safely patch). A named mutex (per process id)
// serialises the attempts and a named shared block records the result: whoever comes second
// reads it and returns the first one's count. As a fallback for a neutraliser without the record
// (an older stub_boot.dll), sites already turned into `push X ; call <stub outside the exe>` are
// counted: 100 or more calls to one such stub mean the work is done.
#pragma once
#include "bo3_zone.h"
#include <tlhelp32.h>
#include <vector>

namespace
{
	struct TextRange
	{
		uint8_t *start;
		size_t size;
	};
	std::vector<TextRange> g_texts;
	volatile LONG g_arxanMisses;

	bool InTexts(const void *p)
	{
		for (const TextRange &t : g_texts)
			if (p >= t.start && p < t.start + t.size)
				return true;
		return false;
	}

	bool OnStack(const uint8_t *frame, const void *p)
	{
		int64_t diff = reinterpret_cast<int64_t>(frame) - reinterpret_cast<int64_t>(p);
		return diff > -0x1000 && diff < 0x1000;
	}

	// Called from the stub: returns the checksum the check should store.
	uint32_t AdjustChecksum(uint64_t returnAddress, uint8_t *frame, uint32_t computed)
	{
		for (uint32_t offset = 0; offset < 0x90; offset += 8)
		{
			uint32_t **context = reinterpret_cast<uint32_t **>(frame + offset);
			uint32_t *computedPtr = context[0], *originalPtr = context[1];
			if (OnStack(frame, computedPtr) && *computedPtr == computed && InTexts(originalPtr))
			{
				*computedPtr = *originalPtr;
				return *originalPtr;
			}
		}
		// Let this one run unmodified: it only fails if it covers something we patched.
		if (InterlockedIncrement(&g_arxanMisses) <= 10)
			Log("arxan: no handler context for the check returning to %p, left alone", reinterpret_cast<void *>(returnAddress));
		return computed;
	}

	// Executable memory that every byte of the image can reach with a rel32.
	uint8_t *AllocReachable(size_t size)
	{
		auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(g_base);
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_base + dos->e_lfanew);
		uint64_t start = reinterpret_cast<uint64_t>(g_base), end = start + nt->OptionalHeader.SizeOfImage;
		constexpr uint64_t kReach = 0x7F000000;
		for (uint64_t at = (start - 0x10000) & ~0xFFFFull; at + kReach > end; at -= 0x10000)
			if (void *p = VirtualAlloc(reinterpret_cast<void *>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
				return static_cast<uint8_t *>(p);
		for (uint64_t at = (end + 0xFFFF) & ~0xFFFFull; at + size < start + kReach; at += 0x10000)
			if (void *p = VirtualAlloc(reinterpret_cast<void *>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
				return static_cast<uint8_t *>(p);
		return nullptr;
	}

	uint8_t *BuildIntactStub()
	{
		uint8_t code[] = {
			0x53,                                                             // push rbx
			0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, // push rax rcx rdx r8-r11
			// [rbx+40h] = return address, [rbx+48h] = X (sign-extended by the push)
			0x48, 0x89, 0xE3,                                                 // mov rbx, rsp
			0x48, 0x83, 0xE4, 0xF0,                                           // and rsp, -16
			0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                         // sub rsp, 80h
			0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,                               // movdqu [rsp+20h], xmm0
			0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,                               //   .. xmm5
			0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,
			0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
			0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,
			0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,
			0x48, 0x8B, 0x4B, 0x40,                                           // mov rcx, [rbx+40h]  return address
			0x48, 0x89, 0xEA,                                                 // mov rdx, rbp        frame
			0x41, 0x89, 0xC0,                                                 // mov r8d, eax        computed
			0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               // mov rax, AdjustChecksum
			0xFF, 0xD0,                                                       // call rax
			0x48, 0x89, 0x43, 0x30,                                           // mov [rbx+30h], rax  -> popped into rax
			0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,                               // movdqu xmm0, [rsp+20h]
			0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,                               //   .. xmm5
			0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,
			0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
			0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60,
			0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
			0x48, 0x89, 0xDC,                                                 // mov rsp, rbx
			0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, // pop r11-r8 rdx rcx rax
			0x5B,                                                             // pop rbx
			0x89, 0xC0,                                                       // mov eax, eax
			0x89, 0x04, 0x8A,                                                 // mov [rdx+rcx*4], eax  (the original store)
			0x51,                                                             // push rcx
			0x48, 0x8B, 0x4C, 0x24, 0x10,                                     // mov rcx, [rsp+10h]    X
			0x83, 0x44, 0x0D, 0x00, 0xFF,                                     // add dword [rbp+rcx], -1 (the original add; sets the flags)
			0x59,                                                             // pop rcx
			0xC2, 0x08, 0x00,                                                 // ret 8  (drops X)
		};
		if (code[72] != 0x48 || code[73] != 0xB8)
			return nullptr;  // the layout above changed without updating the offset
		uint64_t fn = reinterpret_cast<uint64_t>(&AdjustChecksum);
		memcpy(code + 74, &fn, 8);  // the mov rax imm64
		uint8_t *stub = AllocReachable(0x1000);
		if (stub)
			memcpy(stub, code, sizeof(code));
		return stub;
	}

	struct SitePatch
	{
		uint8_t *place;
		uint8_t bytes[10];
		size_t len = 7;
	};

	// Retail's checks run on other threads all the time, so patch with every other thread
	// frozen, and only when none is part-way through a site. No heap use while they're frozen.
	bool ApplyFrozen(const std::vector<SitePatch> &patches)
	{
		std::vector<DWORD> ids;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 e{sizeof(e)};
			for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e))
				if (e.th32OwnerProcessID == GetCurrentProcessId() && e.th32ThreadID != GetCurrentThreadId())
					ids.push_back(e.th32ThreadID);
			CloseHandle(snap);
		}
		std::vector<HANDLE> frozen;
		frozen.reserve(ids.size());
		auto insideSite = [&](uint64_t rip) {
			size_t lo = 0, hi = patches.size();  // first patch with place > rip
			while (lo < hi)
			{
				size_t mid = (lo + hi) / 2;
				if (reinterpret_cast<uint64_t>(patches[mid].place) <= rip)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (!lo)
				return false;
			uint64_t start = reinterpret_cast<uint64_t>(patches[lo - 1].place);
			return rip > start && rip < start + patches[lo - 1].len;
		};
		for (int attempt = 0; attempt < 50; attempt++)
		{
			bool clear = true;
			for (DWORD id : ids)
			{
				HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
				if (!t)
					continue;
				if (SuspendThread(t) == static_cast<DWORD>(-1))
				{
					CloseHandle(t);
					continue;
				}
				frozen.push_back(t);
				CONTEXT c{};
				c.ContextFlags = CONTEXT_CONTROL;
				if (GetThreadContext(t, &c) && insideSite(c.Rip))
				{
					clear = false;
					break;
				}
			}
			if (clear)
				for (const SitePatch &p : patches)
				{
					DWORD old;
					VirtualProtect(p.place, p.len, PAGE_EXECUTE_READWRITE, &old);
					memcpy(p.place, p.bytes, p.len);
					VirtualProtect(p.place, p.len, old, &old);
				}
			for (HANDLE t : frozen)
			{
				ResumeThread(t);
				CloseHandle(t);
			}
			frozen.clear();
			if (clear)
			{
				FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
				return true;
			}
			Sleep(1);
		}
		return false;
	}

	// ---- obfuscated-store checks (retail: 69 of them, all in Arxan's own section) ------------------
	// The same checksum store, but what follows it is an obfuscated jump chain, not `add [rbp+X],-1` right away
	// (the add comes later, after the chain), so the 7-byte intact patch can't be used. These checks still cover code
	// the intact patch rewrites: left alone, they fail ~20 s after the intact sites are patched (the game jumps into
	// unmapped memory). Each is
	//     8B 04 82              mov eax, [rdx+rax*4]
	//     48 8D 15 <rel32>      lea rdx, [table]          <- patched: jmp <its own stub> + 5 NOPs
	//     89 04 8A              mov [rdx+rcx*4], eax      <-
	//     <obfuscated chain ... add dword [rbp+X], -1 ...>
	// The stub keeps 128 bytes below rsp untouched (the chain writes below rsp before moving it) and the flags, asks
	// AdjustChecksum for the value to store (the same frame search as the intact stub), redoes the lea and the store
	// with it, and jumps back to the chain.
	uint8_t *BuildObfuscatedStub(uint8_t *at, const uint8_t *site, const uint8_t *table)
	{
		uint8_t *p = at;
		auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
		auto imm64 = [&](const void *v) { memcpy(p, &v, 8); p += 8; };
		emit({0x48, 0x8D, 0x64, 0x24, 0x80});                               // lea rsp, [rsp-80h]
		emit({0x9C, 0x53});                                                 // pushfq; push rbx
		emit({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // push rax rcx rdx r8-r11
		emit({0x48, 0x89, 0xE3});                                           // mov rbx, rsp
		emit({0x48, 0x83, 0xE4, 0xF0});                                     // and rsp, -16
		emit({0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00});                   // sub rsp, 80h
		for (uint8_t off = 0x20; off <= 0x70; off += 0x10)                  // movdqu [rsp+off], xmm0..5
			emit({0xF3, 0x0F, 0x7F, static_cast<uint8_t>(0x44 + ((off - 0x20) / 0x10) * 8), 0x24, off});
		emit({0x48, 0xB9});                                                 // mov rcx, site (for the log)
		imm64(site);
		emit({0x48, 0x89, 0xEA});                                           // mov rdx, rbp   frame
		emit({0x41, 0x89, 0xC0});                                           // mov r8d, eax   computed
		emit({0x48, 0xB8});                                                 // mov rax, AdjustChecksum
		imm64(reinterpret_cast<const void *>(&AdjustChecksum));
		emit({0xFF, 0xD0});                                                 // call rax
		emit({0x48, 0x89, 0x43, 0x30});                                     // mov [rbx+30h], rax -> popped into rax
		for (uint8_t off = 0x20; off <= 0x70; off += 0x10)                  // movdqu xmm0..5, [rsp+off]
			emit({0xF3, 0x0F, 0x6F, static_cast<uint8_t>(0x44 + ((off - 0x20) / 0x10) * 8), 0x24, off});
		emit({0x48, 0x89, 0xDC});                                           // mov rsp, rbx
		emit({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});  // pop r11-r8 rdx rcx rax
		emit({0x5B, 0x9D});                                                 // pop rbx; popfq
		emit({0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00});             // lea rsp, [rsp+80h]
		emit({0x89, 0xC0});                                                 // mov eax, eax
		emit({0x48, 0xBA});                                                 // mov rdx, table  (the displaced lea)
		imm64(table);
		emit({0x89, 0x04, 0x8A});                                           // mov [rdx+rcx*4], eax  (the displaced store)
		emit({0xFF, 0x25, 0, 0, 0, 0});                                     // jmp [rip] -> after the store
		imm64(site + 10);
		return p;
	}

	// Builds the patches (and their stubs) for the obfuscated-store sites still unpatched; a patched site no longer
	// matches, so any module may run this. The caller applies them, with the intact ones when it patches those too:
	// a check of one kind running between two batches would see the other kind's patches.
	std::vector<SitePatch> PrepareObfuscatedStores(const std::vector<TextRange> &texts)
	{
		std::vector<SitePatch> patches;
		std::vector<uint8_t *> sites;
		for (const TextRange &t : texts)
			for (uint8_t *p = t.start; p + 17 <= t.start + t.size; p++)
			{
				// 8B 04 82 | 48 8D 15 rel32 | 89 04 8A, and not followed by the intact add
				if (p[0] != 0x8B || p[1] != 0x04 || p[2] != 0x82 || p[3] != 0x48 || p[4] != 0x8D || p[5] != 0x15 ||
				    p[10] != 0x89 || p[11] != 0x04 || p[12] != 0x8A)
					continue;
				if (p[13] == 0x83 && p[14] == 0x45 && p[16] == 0xFF)
					continue;  // intact form (the 7-byte patch handles it)
				sites.push_back(p + 3);
			}
		if (sites.empty())
			return patches;
		constexpr size_t kStubSize = 192;
		auto *mem = AllocReachable((sites.size() * kStubSize + 0xFFF) & ~static_cast<size_t>(0xFFF));
		if (!mem)
		{
			Log("arxan: %zu obfuscated-store check(s) found, no memory within reach; left alone", sites.size());
			return patches;
		}
		for (size_t i = 0; i < sites.size(); i++)
		{
			uint8_t *site = sites[i];  // the lea
			int32_t d;
			memcpy(&d, site + 3, 4);
			const uint8_t *table = site + 7 + d;
			uint8_t *stub = mem + i * kStubSize;
			if (BuildObfuscatedStub(stub, site, table) - stub > static_cast<ptrdiff_t>(kStubSize))
				return {};  // can't happen (the stub is ~160 bytes)
			SitePatch s{site, {0xE9}, 10};
			int32_t rel = static_cast<int32_t>(stub - (site + 5));
			memcpy(s.bytes + 1, &rel, 4);
			memset(s.bytes + 5, 0x90, 5);
			patches.push_back(s);
		}
		FlushInstructionCache(GetCurrentProcess(), mem, sites.size() * kStubSize);
		return patches;
	}

	// ---- once per process ------------------------------------------------------------------------
	constexpr uint32_t kArxanMagic = 0x4E585241;  // "ARXN"
	struct ArxanRecord
	{
		uint32_t magic;
		uint32_t count;
		char module[MAX_PATH];  // the DLL that patched
	};

	// The process-wide record (zeroed on creation), or nullptr. Its handle is never closed, so the
	// record lives as long as the process.
	ArxanRecord *SharedArxanRecord()
	{
		static ArxanRecord *s_record;
		if (s_record)
			return s_record;
		char name[64];
		snprintf(name, sizeof(name), "Local\\bo3_arxan_record_%lu", GetCurrentProcessId());
		HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ArxanRecord), name);
		if (!map)
			return nullptr;
		s_record = static_cast<ArxanRecord *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ArxanRecord)));
		return s_record;
	}

	struct ArxanLock
	{
		HANDLE mutex = nullptr;
		ArxanLock()
		{
			char name[64];
			snprintf(name, sizeof(name), "Local\\bo3_arxan_mutex_%lu", GetCurrentProcessId());
			mutex = CreateMutexA(nullptr, FALSE, name);
			if (mutex && WaitForSingleObject(mutex, 60000) == WAIT_TIMEOUT)
				Log("arxan: another module held the lock for 60 s; going ahead");
		}
		~ArxanLock()
		{
			if (mutex)
			{
				ReleaseMutex(mutex);
				CloseHandle(mutex);
			}
		}
	};

	// Sites already patched by a neutraliser that left no record: `push X ; call rel32` (6A X E8)
	// whose target is outside [lo, hi) (the exe image). Returns the most calls that go to one such
	// target, and that target in *stub.
	size_t CountNeutralisedSites(const std::vector<TextRange> &texts, const uint8_t *lo, const uint8_t *hi, const uint8_t **stub)
	{
		struct Tally
		{
			const uint8_t *target;
			size_t count;
		} tally[8] = {};
		for (const TextRange &t : texts)
			for (const uint8_t *p = t.start; p + 7 <= t.start + t.size; p++)
			{
				if (p[0] != 0x6A || p[2] != 0xE8)
					continue;
				int32_t rel;
				memcpy(&rel, p + 3, 4);
				const uint8_t *target = p + 7 + rel;
				if (target >= lo && target < hi)
					continue;
				for (Tally &x : tally)
					if (x.target == target || !x.target)
					{
						x.target = target;
						x.count++;
						break;
					}
			}
		size_t best = 0;
		*stub = nullptr;
		for (const Tally &x : tally)
			if (x.count > best)
			{
				best = x.count;
				*stub = x.target;
			}
		return best;
	}

	const char *ThisModulePath()
	{
		static char s_path[MAX_PATH];
		HMODULE self;
		if (!s_path[0] && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                                     reinterpret_cast<LPCSTR>(&ThisModulePath), &self))
			GetModuleFileNameA(self, s_path, MAX_PATH);
		return s_path;
	}

	// Returns the number of checks neutralised (0: code patches are still exposed). Once per process:
	// if another module (stub_boot.dll / weapon_tech.dll) already did it, that module's count.
	size_t NeutraliseArxan()
	{
		static size_t s_done = SIZE_MAX;
		if (s_done != SIZE_MAX)
			return s_done;
		s_done = 0;

		ArxanLock lock;
		DWORD started = GetTickCount();
		auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(g_base);
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_base + dos->e_lfanew);
		IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
		for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
			if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
				g_texts.push_back({g_base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize});
		// The obfuscated-store checks: an earlier neutraliser (an older stub_boot.dll, T7Overcharged) may have patched only
		// the intact form and left these armed. Applied alone when the intact ones are already done, else with them.
		std::vector<SitePatch> obfuscated = PrepareObfuscatedStores(g_texts);
		auto applyObfuscatedAlone = [&]() {
			if (obfuscated.empty())
				return;
			if (ApplyFrozen(obfuscated))
				Log("arxan: %zu obfuscated-store integrity check(s) neutralised (the intact ones were already done)", obfuscated.size());
			else
				Log("arxan: %zu obfuscated-store check(s) found but not patched", obfuscated.size());
		};

		ArxanRecord *record = SharedArxanRecord();
		if (record && record->magic == kArxanMagic)
		{
			s_done = record->count;
			Log("arxan: already neutralised in this process by %s (%u check(s)); not patching again", record->module, record->count);
			applyObfuscatedAlone();
			return s_done;
		}

		const uint8_t *otherStub;
		size_t before = CountNeutralisedSites(g_texts, g_base, g_base + nt->OptionalHeader.SizeOfImage, &otherStub);
		if (before >= 100)
		{
			s_done = before;
			Log("arxan: %zu check site(s) already call one stub outside the exe (%p): neutralised earlier by a module "
			    "that left no record; not patching again", before, otherStub);
			applyObfuscatedAlone();
			if (record)
			{
				record->count = static_cast<uint32_t>(before);
				strcpy_s(record->module, "an earlier module (no record)");
				record->magic = kArxanMagic;
			}
			return s_done;
		}

		uint8_t *stub = BuildIntactStub();
		if (!stub)
		{
			Log("arxan: no memory within rel32 reach of the exe");
			return 0;
		}

		std::vector<SitePatch> patches;
		size_t split = 0;
		for (const TextRange &t : g_texts)
		{
			for (uint8_t *p = t.start; p + 7 <= t.start + t.size; p++)
			{
				if (p[0] != 0x89 || p[1] != 0x04 || p[2] != 0x8A)
					continue;
				if (p[3] == 0xE9)
					split++;
				if (p[3] != 0x83 || p[4] != 0x45 || p[6] != 0xFF)
					continue;
				SitePatch s{p, {0x6A, p[5], 0xE8}};  // push X ; call stub
				int32_t rel = static_cast<int32_t>(stub - (p + 7));
				memcpy(s.bytes + 3, &rel, 4);
				patches.push_back(s);
			}
		}
		const size_t intact = patches.size();
		if (intact)
		{
			patches.insert(patches.end(), obfuscated.begin(), obfuscated.end());
			std::sort(patches.begin(), patches.end(), [](const SitePatch &a, const SitePatch &b) { return a.place < b.place; });
		}
		if (!intact || !ApplyFrozen(patches))
		{
			Log("arxan: %zu check site(s) found but not patched", intact);
			return 0;
		}
		s_done = patches.size();
		if (!obfuscated.empty())
			Log("arxan: %zu of them are the obfuscated-store form", obfuscated.size());
		if (record)
		{
			record->count = static_cast<uint32_t>(s_done);
			strcpy_s(record->module, ThisModulePath());
			record->magic = kArxanMagic;
		}
		Log("arxan: %zu integrity check(s) neutralised in %lu ms (%zu split-form site(s) left alone)%s",
			s_done, GetTickCount() - started, split, record ? "; recorded for the other modules" : "; NO process record (mapping failed)");
		return s_done;
	}
}
