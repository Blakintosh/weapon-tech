// Weapon variant resolution driven by the engine's own events (included at the end of bo3_additive.h, after
// bo3_locomotion.h and bo3_sway.h). Research: IDA dumps in xpakcap\perf\ (m1-m3.txt, vt.txt; a private copy of the IDB).
//
// The cfg names weapons; the code needs their variant indices (ps weapon & 0x1FF), and additive= / idle_active= also
// write xanim names into a variant's slot names before the viewmodel tree is built from them. That used to be a rescan
// of all 512 table entries every 120 frames for every weapon not found yet, with one or two VirtualQuery calls per
// entry: ~15,000 calls and a ~25 ms hitch every 120 frames with Karelia's cfg, since most cfg weapons are never in a
// given session. Now:
//   - one pass over the table (at install), names looked up in a hash, readability asked once per memory region;
//   - readability of resident pages from the working set (bo3_zone.h ResidentSpan), not VirtualQuery: VirtualQuery walks
//     the whole region page by page, and on BO3's multi-GB zone memory each call took milliseconds, so the poll below
//     and the locomotion leaf recheck were 20-40 ms hitches every ~2 s in game (weapon_tech.log 'slow frame' lines);
//   - the registration hook: BG's register-by-name (0x1427CB570) does index = ++count (int at table - 0x10) and then
//     calls 0x1427CAAD0(def, index, ..), whose first store is table[index & 0x1FF] = def. That call is redirected
//     through a near stub (rel32 only, as the other hooks) that records (index, def) and jumps on. Any thread may
//     register, so the stub only sets bits; the game thread binds those indices on its next frame;
//   - the tree-build hook: both calls of the viewmodel tree build 0x1404EC9D0(weapon, .., vm), the only reader of the
//     slot names (variant+0x48, via 0x1404EC2D0), which runs on a weapon change. The stub checks that variant's patched
//     slots right before the build, so they land on the first raise, and tells the locomotion code a tree was built;
//   - a fallback poll: the table's 512 pointers compared with the last pass (a changed pointer is a new registration),
//     and the patched slots checked (a zone reloaded at the same address loses them). Every perf_pollms with the event
//     hooks in, every 120 frames without them, and at once when the variant count changes (0x1427C9D20 zeroes it at a
//     session start; the table itself is never cleared, so entries past the count are a previous map's, often freed).
// The shape of each hooked site and of the code it relies on is checked first (bytes read in the Enhanced exe); a
// mismatch leaves that hook out and the poll covers it.
#pragma once
#include <atomic>
#include <intrin.h>

#define BO3_PERF_EVENTS 1

namespace
{
	// ---- Engine addresses (RVAs): Enhanced values; bo3_retail.h overwrites them on retail ---------------------------------
	// kVariantCount (int: variants registered this session, index = ++count) is in bo3_additive.h.
	uintptr_t kRegisterBlock = 0x27CB70A;  // mov edx,[count]; inc edx; mov [count],edx; args; call SetupVariant
	uintptr_t kRegisterCall = 0x27CB722;   //   the call (every registration goes through it)
	uintptr_t kSetupVariant = 0x27CAAD0;   // (def, index, ..): table[index & 0x1FF] = def first
	uintptr_t kTreeBuild = 0x4EC9D0;       // viewmodel anim tree build (weapon, .., vm): reads variant+0x48
	uintptr_t kTreeBuildInner = 0x4EC2D0;  //   its call at +0x3D3 builds the nodes from the names (retail +0x2A9)
	uintptr_t kTreeBuildCalls[2] = {0x1213849, 0x12087BA};  // its only two callers (weapon change, respawn)
	uintptr_t kGetVariantDef = 0;          // retail: BG_GetWeaponVariantDef(weapon), which the tree build calls
	constexpr unsigned kAllKinds = (1u << kSubKinds) - 1;

	// ---- the name index: every cfg weapon name -> the cfg entries (subscribers) that name it --------------------------------
	struct NameEntry
	{
		const char *name;
		uint32_t hash;
		int first, last;  // subscriber list
	};
	struct Sub
	{
		uint8_t kind;
		uint16_t index;  // into g_additives / g_ammoHides / g_wops / g_locos / g_sways
		int entry;       // its NameEntry
		int next;
	};
	constexpr int kNameSlots = 1024;
	constexpr int kMaxSubs = kMaxAdditives + kMaxAmmoHides + kMaxWops + kMaxLocos + kMaxSways;
	NameEntry g_names[kNameSlots];
	Sub g_subs[kMaxSubs];
	int g_subCount;
	int g_namesFor[kSubKinds] = {-1, -1, -1, -1, -1};  // entry counts the index was built for
	uint8_t *g_snap[512];                                // the table as of the last pass that covered every kind
	bool g_fullPassDue;

	// ---- event state (the registration stub may run on any thread) ---------------------------------------------------------
	std::atomic<uint64_t> g_regPending[8];
	std::atomic<uint8_t *> g_regDef[512];
	std::atomic<bool> g_regAny;
	std::atomic<long> g_regSeen;
	std::atomic<char *> g_cfgText;  // a cfg edit read by the watcher thread, applied on the game thread

	// Game thread only in practice (CG runs the tree build and the anim hook); a recursive lock in case it isn't.
	struct ResolveLock
	{
		static inline std::atomic<DWORD> owner{0};
		static inline int depth = 0;
		ResolveLock()
		{
			DWORD me = GetCurrentThreadId();
			if (owner.load(std::memory_order_acquire) != me)
				for (DWORD free = 0; !owner.compare_exchange_weak(free, me, std::memory_order_acquire); free = 0)
					YieldProcessor();
			depth++;
		}
		~ResolveLock()
		{
			if (--depth == 0)
				owner.store(0, std::memory_order_release);
		}
	};

	uint32_t NameHash(const char *s, size_t n)
	{
		uint32_t h = 2166136261u;
		for (size_t i = 0; i < n; i++)
			h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
		return h;
	}

	int KindCount(int kind)
	{
		switch (kind)
		{
		case kSubAdditive: return g_additiveCount;
		case kSubAmmoHide: return g_ammoHideCount;
		case kSubWop: return g_wopCount;
		case kSubLoco: return g_locoCount;
		default: return g_swayCount;
		}
	}
	const char *KindName(int kind, int i)
	{
		switch (kind)
		{
		case kSubAdditive: return g_additives[i].weapon;
		case kSubAmmoHide: return g_ammoHides[i].weapon;
		case kSubWop: return g_wops[i].weapon;
		case kSubLoco: return g_locos[i].weapon;
		default: return g_sways[i].weapon;
		}
	}
	int *KindVariant(const Sub &s)
	{
		switch (s.kind)
		{
		case kSubAdditive: return &g_additives[s.index].variant;
		case kSubAmmoHide: return &g_ammoHides[s.index].variant;
		case kSubWop: return &g_wops[s.index].variant;
		case kSubLoco: return &g_locos[s.index].variant;
		default: return &g_sways[s.index].variant;
		}
	}

	// The index is rebuilt when an entry table grew (cfg parsing, aliases) or the sway table was re-read.
	void NamesCurrent()
	{
		bool same = !g_namesDirty;
		for (int k = 0; k < kSubKinds && same; k++)
			same = g_namesFor[k] == KindCount(k);
		if (same)
			return;
		memset(g_names, 0, sizeof(g_names));
		g_subCount = 0;
		for (int k = 0; k < kSubKinds; k++)
		{
			g_namesFor[k] = KindCount(k);
			for (int i = 0; i < g_namesFor[k] && g_subCount < kMaxSubs; i++)
			{
				const char *name = KindName(k, i);
				size_t n = strlen(name);
				uint32_t h = NameHash(name, n);
				int slot = h & (kNameSlots - 1);
				while (g_names[slot].name && (g_names[slot].hash != h || strcmp(g_names[slot].name, name) != 0))
					slot = (slot + 1) & (kNameSlots - 1);
				NameEntry &e = g_names[slot];
				if (!e.name)
					e = {name, h, -1, -1};
				Sub &s = g_subs[g_subCount];
				s = {static_cast<uint8_t>(k), static_cast<uint16_t>(i), slot, -1};
				if (e.last >= 0)
					g_subs[e.last].next = g_subCount;
				else
					e.first = g_subCount;
				e.last = g_subCount++;
			}
		}
		g_namesDirty = false;
		memset(g_snap, 0, sizeof(g_snap));  // subscribers moved: the next poll is a full pass
		g_fullPassDue = true;
	}

	// The cfg entry for a weapon name read from game memory. The name must end (NUL) within `span` readable bytes, as
	// strcmp against a cfg name would need: cfg names are under 64 chars.
	const NameEntry *FindName(const char *name, size_t span)
	{
		size_t n = 0;
		while (n < span && n < 64 && name[n])
			n++;
		if (n == span || n == 64)
			return nullptr;
		uint32_t h = NameHash(name, n);
		for (int slot = h & (kNameSlots - 1);; slot = (slot + 1) & (kNameSlots - 1))
		{
			const NameEntry &e = g_names[slot];
			if (!e.name)
				return nullptr;
			if (e.hash == h && strcmp(e.name, name) == 0)
				return &e;
		}
	}

	// The cfg entry named like the variant at p (nullptr: no variant, unreadable, or not a cfg weapon).
	const NameEntry *NameOf(const uint8_t *p, ReadCache &rc)
	{
		if (!p || !rc.Readable(p, 8))
			return nullptr;
		const char *name = *reinterpret_cast<const char *const *>(p);
		size_t span = name ? rc.Span(name) : 0;
		return span ? FindName(name, span) : nullptr;
	}

	// Writes `xanim` into slot `slot` of the variant's anim names (variant+0x48 points to the array). False when the
	// variant or its names aren't readable.
	bool PatchSlotName(uint8_t *p, int v, uint32_t slot, const char *xanim, const char *weapon, bool additive, ReadCache &rc)
	{
		if (!rc.Readable(p, kVariantAnims + 8))
			return false;
		auto **anims = *reinterpret_cast<const char ***>(p + kVariantAnims);
		if (!anims || !rc.Readable(anims, 8 * kNumWeapAnims))
			return false;
		static bool s_dumped[512];
		if (additive && !s_dumped[v])
		{
			s_dumped[v] = true;
			for (int s = 0; s < kNumWeapAnims; s++)
			{
				if (s == 6)
					s = 190;
				const char *a = anims[s];
				bool str = a && rc.Readable(a, 1) && a[0] >= 0x20 && a[0] < 0x7F;
				Log("additive: %s slot %d = %p %s", weapon, s, a, str ? a : (a && rc.Readable(a, 1) && !a[0] ? "''" : "(not a string)"));
			}
		}
		// Compare the text, not the pointer: a base gun and its PaP can share one anim-name array while their cfg
		// entries hold separate copies of the same name, and a pointer test made them overwrite each other (and log)
		// on every poll.
		const char *cur = anims[slot];
		if (cur != xanim && !(cur && rc.Span(cur) && strcmp(cur, xanim) == 0))
		{
			const char *was = anims[slot] && rc.Readable(anims[slot], 1) ? anims[slot] : "";
			if (additive)
				Log("additive: %s variant %d slot %u was '%s', now '%s'", weapon, v, slot, was, xanim);
			else
				Log("locomotion: idle_active %s variant %d slot %u was '%s', now '%s'", weapon, v, slot, was, xanim);
			anims[slot] = xanim;
		}
		return true;
	}

	bool LocoPatches(const LocoWeapon &w) { return w.idle && !w.idleRefused && strcmp(w.idleXanim, "*") != 0; }

	// Binds one cfg entry to variant v (at p), as the old per-kind scans did: additive= takes every variant with its name
	// (the last one wins) and patches each; the others keep the first they found.
	void BindSub(const Sub &s, int v, uint8_t *p, ReadCache &rc)
	{
		switch (s.kind)
		{
		case kSubAdditive:
		{
			AdditiveConfig &c = g_additives[s.index];
			if (!PatchSlotName(p, v, c.leaf, c.xanim, c.weapon, true, rc))
				return;
			if (c.variant != v)
				Log("additive: %s is variant %d", c.weapon, v);
			c.variant = v;
			return;
		}
		case kSubAmmoHide:
		{
			AmmoHideConfig &h = g_ammoHides[s.index];
			if (h.variant < 0)
			{
				h.variant = v;
				Log("additive: ammohide %s is variant %d", h.weapon, v);
			}
			return;
		}
		case kSubWop:
		{
			WopWeapon &w = g_wops[s.index];
			if (w.variant < 0)
			{
				w.variant = v;
				Log("additive: weapon offsets %s is variant %d", w.weapon, v);
			}
			return;
		}
		case kSubLoco:
		{
			LocoWeapon &w = g_locos[s.index];
			if (!LocoPatches(w) || (w.variant >= 0 && w.variant != v))
				return;
			if (PatchSlotName(p, v, w.idleNode, w.idleXanim, w.weapon, false, rc))
				w.variant = v;
			return;
		}
		default:
		{
			SwayWeapon &w = g_sways[s.index];
			if (w.variant < 0)
			{
				w.variant = v;
				Log("sway: %s is variant %d", w.weapon, v);
			}
			return;
		}
		}
	}

	// Looks at the listed table indices (ascending): cfg entries bound to one of them whose name isn't there any more
	// are unbound (any kind), then every entry of `kinds` named like a variant there is bound to it. `snapshot` (only for
	// all kinds) records the pointers, so the poll skips them until they change.
	void Pass(const int *list, int n, unsigned kinds, ReadCache &rc, bool snapshot)
	{
		auto **table = At<uint8_t *>(kWeaponVariants);
		static uint8_t *p[512];
		static const NameEntry *e[512];
		static bool mark[512];
		for (int i = 0; i < n; i++)
		{
			int v = list[i];
			mark[v] = true;
			p[v] = table[v];
			e[v] = NameOf(p[v], rc);
		}
		for (int i = 0; i < g_subCount; i++)
		{
			int *variant = KindVariant(g_subs[i]);
			if (*variant >= 0 && *variant < 512 && mark[*variant] && e[*variant] != &g_names[g_subs[i].entry])
				*variant = -1;
		}
		for (int i = 0; i < n; i++)
		{
			int v = list[i];
			if (e[v])
				for (int s = e[v]->first; s >= 0; s = g_subs[s].next)
					if (kinds >> g_subs[s].kind & 1)
						BindSub(g_subs[s], v, p[v], rc);
		}
		for (int i = 0; i < n; i++)
		{
			mark[list[i]] = false;
			if (snapshot)
				g_snap[list[i]] = p[list[i]];
		}
	}

	// One pass over the whole table for these kinds (the old PatchVariantSlots / ResolveAmmoHideVariants /
	// PatchLocomotionSlots / ResolveSwayVariants, and the install-time pass).
	void ResolveKinds(unsigned kinds)
	{
		ResolveLock lock;
		NamesCurrent();
		static int s_all[512];
		for (int v = 0; v < 512; v++)
			s_all[v] = v;
		ReadCache rc;
		Pass(s_all, 512, kinds, rc, kinds == kAllKinds);
		g_perfGen++;
	}

	void ResolveAllVariants()
	{
		FinishLocomotionConfig();
		ResolveKinds(kAllKinds);
		int bound = 0;
		for (int i = 0; i < g_subCount; i++)
			bound += *KindVariant(g_subs[i]) >= 0;
		Log("perf: variant table resolved: %d of %d cfg entries bound (the rest aren't in this level, or not yet)", bound, g_subCount);
	}

	// The entry of `kind` named like variant `variant` (FindLocoForVariant): -1 if none.
	int FindConfigByVariantName(int variant, int kind, ReadCache &rc)
	{
		NamesCurrent();
		const NameEntry *e = NameOf(At<uint8_t *>(kWeaponVariants)[variant & 0x1FF], rc);
		for (int s = e ? e->first : -1; s >= 0; s = g_subs[s].next)
			if (g_subs[s].kind == kind)
				return g_subs[s].index;
		return -1;
	}

	// ---- the poll: changed table pointers, lost slot patches ---------------------------------------------------------------
	void PerfPoll()
	{
		ResolveLock lock;
		NamesCurrent();
		auto **table = At<uint8_t *>(kWeaponVariants);
		int changed[512], n = 0;
		for (int v = 0; v < 512; v++)
			if (table[v] != g_snap[v])
				changed[n++] = v;
		ReadCache rc;
		if (n)
		{
			Pass(changed, n, kAllKinds, rc, true);
			g_perfGen++;
		}
		// Bound slot patches still in place (only variants the pass above didn't just do)
		for (int a = 0; a < g_additiveCount; a++)
		{
			AdditiveConfig &c = g_additives[a];
			if (c.variant >= 0 && table[c.variant] == g_snap[c.variant] && table[c.variant])
				PatchSlotName(table[c.variant], c.variant, c.leaf, c.xanim, c.weapon, true, rc);
		}
		for (int i = 0; i < g_locoCount; i++)
		{
			LocoWeapon &w = g_locos[i];
			if (w.variant >= 0 && LocoPatches(w) && table[w.variant] == g_snap[w.variant] && table[w.variant])
				PatchSlotName(table[w.variant], w.variant, w.idleNode, w.idleXanim, w.weapon, false, rc);
		}
		static int s_polls;
		if (++s_polls == 20)
			for (int a = 0; a < g_additiveCount; a++)
				if (g_additives[a].variant < 0)
					Log("additive: no weapon variant named '%s' yet (is the variant name at +0?)", g_additives[a].weapon);
	}

	// ---- event handlers ----------------------------------------------------------------------------------------------------
	// The registration stub, before 0x1427CAAD0(def, index, ..) runs: any thread, so only atomics.
	void PerfOnRegister(uint8_t *def, uint32_t index)
	{
		uint32_t v = index & 0x1FF;
		g_regDef[v].store(def, std::memory_order_relaxed);
		g_regPending[v >> 6].fetch_or(1ull << (v & 63), std::memory_order_release);
		g_regAny.store(true, std::memory_order_release);
		g_regSeen.fetch_add(1, std::memory_order_relaxed);
	}

	// Game thread: binds the variants registered since the last call. An index whose table entry isn't the registered
	// def yet (the stub runs just before the store) waits for the next call, a few times at most.
	void ProcessRegistrations()
	{
		if (!g_regAny.load(std::memory_order_acquire))
			return;
		ResolveLock lock;
		g_regAny.store(false, std::memory_order_relaxed);
		NamesCurrent();
		auto **table = At<uint8_t *>(kWeaponVariants);
		static uint8_t s_retries[512];
		int list[512], n = 0;
		bool again = false;
		for (int w = 0; w < 8; w++)
		{
			uint64_t bits = g_regPending[w].exchange(0, std::memory_order_acquire);
			while (bits)
			{
				unsigned long b;
				_BitScanForward64(&b, bits);
				bits &= bits - 1;
				int v = w * 64 + static_cast<int>(b);
				if (table[v] != g_regDef[v].load(std::memory_order_relaxed) && ++s_retries[v] < 8)
				{
					g_regPending[w].fetch_or(1ull << b, std::memory_order_relaxed);
					again = true;
					continue;
				}
				s_retries[v] = 0;
				list[n++] = v;
			}
		}
		if (again)
			g_regAny.store(true, std::memory_order_release);
		if (!n)
			return;
		int before = 0, after = 0;
		for (int i = 0; i < g_subCount; i++)
			before += *KindVariant(g_subs[i]) >= 0;
		ReadCache rc;
		Pass(list, n, kAllKinds, rc, true);
		for (int i = 0; i < g_subCount; i++)
			after += *KindVariant(g_subs[i]) >= 0;
		g_perfGen++;
		Log("perf: %d variant(s) registered (hook), cfg entries bound %d -> %d", n, before, after);
	}

	// The tree-build stub, on the game thread right before 0x1404EC9D0(weapon, ..) reads the variant's slot names.
	void PerfBeforeTreeBuild(uint64_t weapon)
	{
		PerfScope timing(kPtTreeBuild);
		ResolveLock lock;
		ProcessRegistrations();
		NamesCurrent();
		int v = static_cast<int>(weapon & 0x1FF);
		uint8_t *p = At<uint8_t *>(kWeaponVariants)[v];
		ReadCache rc;
		if (p != g_snap[v])
			Pass(&v, 1, kAllKinds, rc, true);
		else if (const NameEntry *e = NameOf(p, rc))
			for (int s = e->first; s >= 0; s = g_subs[s].next)
				if (g_subs[s].kind == kSubAdditive || g_subs[s].kind == kSubLoco)
					BindSub(g_subs[s], v, p, rc);  // puts back a slot name the variant lost
		g_treeBuilds++;
		g_perfGen++;
		static bool s_logged;
		if (!s_logged)
		{
			s_logged = true;
			Log("perf: first viewmodel tree build seen (variant %d); slot names are checked right before each build", v);
		}
	}

	// Per frame, from AfterViewWeaponAnim.
	void PerfFrame(int frame)
	{
		ProcessRegistrations();
		static int s_count = -1;
		static double s_next;
		bool due;
		if (g_eventHooksLive)
		{
			double now = NowSeconds();
			due = now >= s_next;
			if (due)
				s_next = now + g_perfPollMs * 0.001;
		}
		else
			due = frame % 120 == 0;
		int count = *At<int32_t>(kVariantCount);
		if (count != s_count)  // a session started (count zeroed) or variants were registered
		{
			s_count = count;
			due = true;
			g_perfGen++;
		}
		if (due || g_fullPassDue)
		{
			g_fullPassDue = false;
			PerfPoll();
			if (!g_cfgWatchLive)
				ReloadAdditiveTuning();
		}
		if (g_cfgWatchLive)
			if (char *text = g_cfgText.exchange(nullptr))
			{
				Log("perf: cfg changed: applying live tuning");
				ApplyTuningText(text);
				free(text);
			}
	}

	// ---- cfg watcher -------------------------------------------------------------------------------------------------------
	DWORD WINAPI CfgWatchThread(void *)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		FILETIME seen = g_cfgTime;
		for (;;)
		{
			Sleep(250);
			WIN32_FILE_ATTRIBUTE_DATA a;
			if (!GetFileAttributesExA(g_cfgPath, GetFileExInfoStandard, &a) || CompareFileTime(&a.ftLastWriteTime, &seen) == 0)
				continue;
			char *text = ReadCfgText(g_cfgPath);
			if (!text)
				continue;  // still being written: next time
			seen = a.ftLastWriteTime;
			free(g_cfgText.exchange(text));
		}
	}

	void StartCfgWatch()
	{
		if (g_cfgWatchLive || !g_cfgPath[0])
			return;
		if (!g_cfgTime.dwLowDateTime && !g_cfgTime.dwHighDateTime)
			ReloadAdditiveTuning();  // records the time
		HANDLE h = CreateThread(nullptr, 0, CfgWatchThread, nullptr, 0, nullptr);
		if (!h)
		{
			Log("perf: no cfg watcher thread; live tuning checked on the game thread");
			return;
		}
		CloseHandle(h);
		g_cfgWatchLive = true;
		Log("perf: cfg live tuning watched off the game thread (every 250 ms)");
	}

	// ---- hooks ---------------------------------------------------------------------------------------------------------------
	// push rcx, rdx, r8, r9; sub rsp, 28h; call fn (rcx, rdx as the call site set them); restore; jmp target. The callee
	// then runs with the caller's registers, stack and return address, exactly as if called directly.
	size_t BuildPreCallStub(uint8_t *stub, void *fn, void *target)
	{
		static const uint8_t kHead[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8};
		static const uint8_t kTail[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0xFF, 0x25, 0, 0, 0, 0};
		uint8_t *p = stub;
		memcpy(p, kHead, sizeof(kHead));
		p += sizeof(kHead);
		memcpy(p, &fn, 8);
		p += 8;
		memcpy(p, kTail, sizeof(kTail));
		p += sizeof(kTail);
		memcpy(p, &target, 8);
		return p + 8 - stub;
	}

	bool RipTarget(const uint8_t *insn, size_t dispAt, size_t len, uintptr_t rva)
	{
		int32_t d;
		memcpy(&d, insn + dispAt, 4);
		return insn + len + d == At<uint8_t>(rva);
	}
	bool CallsTo(const uint8_t *insn, uintptr_t rva) { return insn[0] == 0xE8 && RipTarget(insn, 1, 5, rva); }

	// The registration call and the table store it leads to, as read in the Enhanced exe.
	bool RegisterHookShapeOk()
	{
		const uint8_t *s = At<uint8_t>(kRegisterBlock), *f = At<uint8_t>(kSetupVariant);
		static const uint8_t kArgs[] = {0x44, 0x8B, 0xC5, 0x45, 0x0F, 0xB6, 0xCF, 0x48, 0x8B, 0xCE};  // mov r8d,ebp; movzx r9d,r15b; mov rcx,rsi
		static const uint8_t kIndex[] = {0x8B, 0xFA};                                               // mov edi, edx
		static const uint8_t kMask[] = {0x81, 0xE7, 0xFF, 0x01, 0x00, 0x00};                        // and edi, 1FFh
		static const uint8_t kStore[] = {0x49, 0x89, 0x8C, 0xFF};                                   // mov [r15+rdi*8+disp32], rcx
		int32_t disp;
		memcpy(&disp, f + 0x43, 4);
		const bool block = s[0] == 0x8B && s[1] == 0x15 && RipTarget(s, 2, 6, kVariantCount) &&       // mov edx, [count]
		                   s[6] == 0xFF && s[7] == 0xC2 &&                                              // inc edx
		                   s[8] == 0x89 && s[9] == 0x15 && RipTarget(s + 8, 2, 6, kVariantCount) &&   // mov [count], edx
		                   memcmp(s + 14, kArgs, sizeof(kArgs)) == 0 && s + 24 == At<uint8_t>(kRegisterCall) &&
		                   CallsTo(s + 24, kSetupVariant);
		if (IsRetailExe())
		{
			// mov ebx,edx; lea rax,[table]; ..; and ebx,1FFh; ..; mov r14d,ebx; ..; and r14d,1FFh; mov [rax+r14*8],rcx
			static const uint8_t kRMask[] = {0x81, 0xE3, 0xFF, 0x01, 0x00, 0x00};
			static const uint8_t kRMask2[] = {0x44, 0x8B, 0xF3};
			static const uint8_t kRStore[] = {0x41, 0x81, 0xE6, 0xFF, 0x01, 0x00, 0x00, 0x4A, 0x89, 0x0C, 0xF0};
			return block && f[0x1A] == 0x8B && f[0x1B] == 0xDA && f[0x1C] == 0x48 && f[0x1D] == 0x8D && f[0x1E] == 0x05 &&
			       RipTarget(f + 0x1C, 3, 7, kWeaponVariants) && memcmp(f + 0x26, kRMask, sizeof(kRMask)) == 0 &&
			       memcmp(f + 0x36, kRMask2, sizeof(kRMask2)) == 0 && memcmp(f + 0x3D, kRStore, sizeof(kRStore)) == 0;
		}
		return block &&
		       f[0x22] == 0x4C && f[0x23] == 0x8D && f[0x24] == 0x3D && RipTarget(f + 0x22, 3, 7, 0) &&  // lea r15, image base
		       memcmp(f + 0x29, kIndex, sizeof(kIndex)) == 0 && memcmp(f + 0x33, kMask, sizeof(kMask)) == 0 &&
		       memcmp(f + 0x3F, kStore, sizeof(kStore)) == 0 && disp == static_cast<int32_t>(kWeaponVariants);
	}

	// Both calls of the tree build (rcx = the weapon, r8 = the viewmodel info) and the build's own use of them.
	bool TreeHookShapeOk()
	{
		static const uint8_t kSite1[] = {0x4D, 0x8B, 0xC6, 0x48, 0x8B, 0xCF};  // mov r8, r14; mov rcx, rdi
		static const uint8_t kSite2[] = {0x48, 0x8B, 0x97, 0x80, 0xAB, 0x11, 0x00, 0x4C, 0x8B, 0xC6, 0x48, 0x8B, 0xC8};  // rdx; r8, rsi; rcx, rax
		static const uint8_t kHead[] = {0x48, 0x8B, 0xC1, 0x4C, 0x8D, 0x2D};   // mov rax, rcx; lea r13, [table]
		static const uint8_t kIndex[] = {0x25, 0xFF, 0x01, 0x00, 0x00};        // and eax, 1FFh
		static const uint8_t kNames[] = {0x49, 0x8B, 0x6E, 0x48};              // mov rbp, [r14+48h]: the slot names
		const uint8_t *s1 = At<uint8_t>(kTreeBuildCalls[0]), *s2 = At<uint8_t>(kTreeBuildCalls[1]), *f = At<uint8_t>(kTreeBuild);
		if (IsRetailExe())
		{
			// Retail fetches the variant through BG_GetWeaponVariantDef (lea rax,[table]; and ecx,1FFh; mov rax,[rax+rcx*8]).
			static const uint8_t kRSite1[] = {0x4D, 0x8B, 0xC5, 0x48, 0x8B, 0xCF, 0x48, 0x8B, 0x90, 0xD0, 0x02, 0x00, 0x00};  // r8,r13; rcx,rdi; rdx
			static const uint8_t kRHead[] = {0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xD9};  // mov rdi, r8; mov rbx, rcx
			static const uint8_t kRNames[] = {0x49, 0x8B, 0x75, 0x48};             // mov rsi, [r13+48h]: the slot names
			static const uint8_t kRGet[] = {0x81, 0xE1, 0xFF, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x04, 0xC8, 0xC3};
			const uint8_t *g = At<uint8_t>(kGetVariantDef);
			return kGetVariantDef && memcmp(s1 - sizeof(kRSite1), kRSite1, sizeof(kRSite1)) == 0 && CallsTo(s1, kTreeBuild) &&
			       memcmp(s2 - sizeof(kSite2), kSite2, sizeof(kSite2)) == 0 && CallsTo(s2, kTreeBuild) &&
			       memcmp(f + 0x22, kRHead, sizeof(kRHead)) == 0 && CallsTo(f + 0x28, kGetVariantDef) &&
			       memcmp(f + 0x38, kRNames, sizeof(kRNames)) == 0 && CallsTo(f + 0x2A9, kTreeBuildInner) &&
			       g[0] == 0x48 && g[1] == 0x8D && g[2] == 0x05 && RipTarget(g, 3, 7, kWeaponVariants) &&
			       memcmp(g + 7, kRGet, sizeof(kRGet)) == 0;
		}
		return memcmp(s1 - sizeof(kSite1), kSite1, sizeof(kSite1)) == 0 && CallsTo(s1, kTreeBuild) &&
		       memcmp(s2 - sizeof(kSite2), kSite2, sizeof(kSite2)) == 0 && CallsTo(s2, kTreeBuild) &&
		       memcmp(f + 0x24, kHead, sizeof(kHead)) == 0 && RipTarget(f + 0x27, 3, 7, kWeaponVariants) &&
		       memcmp(f + 0x2E, kIndex, sizeof(kIndex)) == 0 && memcmp(f + 0x4C, kNames, sizeof(kNames)) == 0 &&
		       CallsTo(f + 0x3D3, kTreeBuildInner);
	}

	// Points the call at `rva` through a pre-call stub to `fn` (the call must go to `expect`).
	bool HookCallPre(uintptr_t rva, uintptr_t expect, void *fn, const char *what)
	{
		uint8_t *site = At<uint8_t>(rva);
		if (!CallsTo(site, expect))
		{
			Log("perf: %s: +%zx doesn't call +%zx; not hooked", what, rva, expect);
			return false;
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!stub)
		{
			Log("perf: %s: no memory near the exe; not hooked", what);
			return false;
		}
		size_t n = BuildPreCallStub(stub, fn, At<uint8_t>(expect));
		FlushInstructionCache(GetCurrentProcess(), stub, n);
		int32_t rel = static_cast<int32_t>(stub - (site + 5));
		// One atomic 8-byte store when the rel32 sits inside an aligned qword (it does at all three sites), so a thread
		// running the call meanwhile sees the old target or the new one, never half of each.
		auto *q = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(site + 1) & ~static_cast<uintptr_t>(7));
		DWORD old;
		VirtualProtect(q, 16, PAGE_EXECUTE_READWRITE, &old);
		if (site + 5 <= q + 8)
		{
			uint64_t now = *reinterpret_cast<volatile uint64_t *>(q);
			memcpy(reinterpret_cast<uint8_t *>(&now) + (site + 1 - q), &rel, 4);
			InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(q), static_cast<LONG64>(now));
		}
		else
			memcpy(site + 1, &rel, 4);
		VirtualProtect(q, 16, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, 5);
		return true;
	}

	void InstallPerfHooks()
	{
		if (WtDebugSkip("perf"))
			return;
		static bool s_done;
		if (s_done)
			return;
		s_done = true;
		if (!g_perfEventHooks)
		{
			Log("perf: event hooks off (perf_eventhooks=0): the variant table is polled every 120 frames (a pointer compare)");
			return;
		}
		if (!RegisterHookShapeOk())
			Log("perf: the weapon registration code doesn't match the %s exe; no registration hook", WtExeName());
		else
			g_eventHooksLive = HookCallPre(kRegisterCall, kSetupVariant, reinterpret_cast<void *>(&PerfOnRegister), "registration");
		if (!TreeHookShapeOk())
			Log("perf: the viewmodel tree build code doesn't match the %s exe; no tree-build hook", WtExeName());
		else
		{
			bool a = HookCallPre(kTreeBuildCalls[0], kTreeBuild, reinterpret_cast<void *>(&PerfBeforeTreeBuild), "tree build (weapon change)");
			bool b = HookCallPre(kTreeBuildCalls[1], kTreeBuild, reinterpret_cast<void *>(&PerfBeforeTreeBuild), "tree build (respawn)");
			g_treeHookLive = a && b;
		}
		Log("perf: event hooks: registration %s (+%zx), tree build %s (+%zx, +%zx); fallback table poll every %s", g_eventHooksLive ? "ON" : "off",
		    kRegisterCall, g_treeHookLive ? "ON" : "off", kTreeBuildCalls[0], kTreeBuildCalls[1], g_eventHooksLive ? "perf_pollms" : "120 frames");
		if (g_eventHooksLive)
			Log("perf: fallback poll every %d ms (perf_pollms)", g_perfPollMs);
	}
}
