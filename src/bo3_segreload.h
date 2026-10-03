// Empty variants for segmented (shell-by-shell) reloads: MW2019 rechamber ends and MWII-style empty starts.
// Both supported exes (addresses: bo3_build.h / bo3_retail.h). Recon: decompiles in the author's research notes (r1.txt, r2.txt; addresses there are absolute, RVAs here).
// Reload state machine (PM_Weapon 0x1427C1EA0, hand 0 at ps+0x54, hand 1 at ps+0x70):
//   begin 0x1427B0DC0: segmented && reloadStartTime -> state 15 (16), anim 41 RELOAD_START, time WeaponDef+0xAF0;
//                      else 0x1427B06B0.
//   0x1427B06B0 (segment): clip 0 (bullet weapons) -> anim 40 RELOAD_EMPTY, time reloadEmptyTime (variant+0x19C);
//                      else anim 39 RELOAD, reloadTime (variant+0x198); state 12 (14; 13 when coming from 17).
//   states 15-17 time out -> 0x1427B2D90, states 12-14 -> 0x1427B2F40: another segment (0x1427B06B0) while the clip
//                      isn't full and fire isn't held, else state 18, anim 42 RELOAD_END, time WeaponDef+0xAF8.
//   state 18 times out -> state 0 (idle anim). Ammo is added when weaponDelay (the add time) runs out.
// Viewmodel: ps anim N -> node N + 1 (0x1427CC980); node rate = anim length / that node's time (0x1404DB320, table
// 0x143119410: node 41 -> 0x1427D1340 reloadEmptyTime, node 43 -> WeaponDef+0xAF8).
//
// Config (weapon_tech.cfg; not live, a restart picks up changes):
//   segreload_empty=<weapon>,<end|start>   (aliases: mw / mw19 = end, mwii = start; off = disable a PaP that would inherit)
//   segreload_enable=0                     global off switch (default 1)
//   A wop_alias=<pap>,<source> line also gives the PaP its source's mode unless the PaP has its own line.
//
// The empty variant lives in the GDT's reloadEmptyAnim slot, its length in reloadEmptyTime:
//   end   (MW2019): reloadEndAnim = the plain end (no pump), reloadEmptyAnim = the rechamber end. A reload that BEGAN with
//                   an empty clip plays reloadEmptyAnim for its end segment, timed by reloadEmptyTime.
//   start (MWII):   reloadEmptyAnim = the empty start (shell through the ejection port), reloadEmptyTime its length,
//                   reloadEmptyAddTime = when that shell counts. A reload that begins empty skips the normal start and
//                   runs the empty start as its first segment; it adds reloadAmmoAdd (not reloadStartAdd).
//
// Why it is safe to reuse reloadEmptyAnim: BG picks RELOAD_EMPTY (ps anim 40 -> node 41) only in the loop-segment
// begin (0x1427B06B0) when the clip is still 0, i.e. never for these guns while reloadStartAdd >= 1 and the start
// runs to its end. For end-mode guns the loop-begin call is wrapped so it can't happen at all (the clip reads 1 for
// that one call), so node 41 only ever plays as the end.
//
// Prediction: everything is done inside BG's own reload functions (call-site rel32 redirects), which both the server's
// Pmove and the client's prediction replays run, and it only writes the playerState they are working on (weapAnim,
// weaponTime). Same input ps -> same output on both sides, so the snapshot matches the prediction and no WeaponDef
// (shared, global) is ever patched. The "began empty" bit is latched per clientNum at the reload's begin; replays of the
// begin write the same value, and nothing clears it (the end of the same reload may be replayed again later).
#pragma once
#include <atomic>

namespace
{
	// ---- Engine addresses (RVAs): Enhanced values; bo3_retail.h overwrites them on retail ---------------------------
	uintptr_t kSrBeginReload = 0x27B0DC0;    // PM reload begin (pm*, pml*, hand): START (state 15) or B06B0
	uintptr_t kSrLoopBegin = 0x27B06B0;      // segment begin (unused, ps, hand, char *emptyOut): states 12-14
	uintptr_t kSrLoopDone = 0x27B2F40;       // states 12-14 timed out (pm*, flag, hand): next loop or END (18)
	uintptr_t kSrStartDone = 0x27B2D90;      // states 15-17 timed out (pm*, flag, hand): loop or END (18)
	uintptr_t kSrReloadEmptyTime = 0x27D1340; // (unused, weapon) -> reloadEmptyTime (ms, attachment-scaled)
	uintptr_t &kSrClipSlotWeapon = kClipSlotWeapon;    // weapon -> the value heldWeapons[] stores
	uintptr_t &kSrVariants = kWeaponVariants;         // WeaponVariantDef* [512]
	uintptr_t kSrIsSegmented = 0x27CFB80;    // (weapon) -> segmentedReload (WeaponDef+0xF45, or variant+0x190
	                                                   // clear and an attachment's +0x3B0), as BG asks it
	struct SrSite
	{
		uintptr_t call, target;
	};
	SrSite kSrBeginSites[] = {{0x27B3D55, kSrBeginReload}, {0x27B6A38, kSrBeginReload}};
	SrSite kSrLoopSites[] = {{0x27B1347, kSrLoopBegin}, {0x27B2E6C, kSrLoopBegin}, {0x27B3223, kSrLoopBegin}};
	SrSite kSrLoopDoneSites[] = {{0x27C303F, kSrLoopDone}, {0x27C4619, kSrLoopDone}};
	SrSite kSrStartDoneSites[] = {{0x27C302C, kSrStartDone}};

	// playerState_t, hand 0 (handState[1] is +0x1C on)
	constexpr size_t kSrPsClientNum = 0x0, kSrPsWeaponTime = 0x54, kSrPsWeaponState = 0x5C, kSrPsWeapAnim = 0x64;
	constexpr size_t kSrPsWeapon = 0x2C0, kSrPsHeld = 0x378, kSrPsHeldStride = 0x30, kSrPsClip = 0x684;
	// WeaponVariantDef / WeaponDef
	constexpr size_t kSrVarDef = 0x18, kSrVarAnims = 0x48, kSrVarReloadTime = 0x198, kSrVarReloadEmptyTime = 0x19C;
	constexpr size_t kSrDefAddTime = 0xAD0, kSrDefEmptyAddTime = 0xAD4, kSrDefStartTime = 0xAF0, kSrDefStartAddTime = 0xAF4,
	                 kSrDefEndTime = 0xAF8;
	// ps weapAnim values (low 13 bits; 0x2000 is the restart toggle) and the viewmodel nodes they map to (+1)
	constexpr int kSrAnimReload = 39, kSrAnimReloadEmpty = 40, kSrAnimReloadStart = 41, kSrAnimReloadEnd = 42;
	constexpr int kSrNodeReload = 40, kSrNodeReloadEmpty = 41, kSrNodeReloadStart = 42, kSrNodeReloadEnd = 43;
	constexpr int kSrStateEnd = 18;

	enum SrMode : int { kSrUnset = -1, kSrOff = 0, kSrStart = 1, kSrEnd = 2 };
	struct SrEntry
	{
		char weapon[64];
		int mode;
	};
	struct SrAlias
	{
		char weapon[64], source[64];
	};
	SrEntry g_srEntries[64];
	int g_srCount;
	SrAlias g_srAliases[128];
	int g_srAliasCount;
	bool g_srEnable = true, g_srHooked;

	using SrBeginFn = uint64_t (*)(int64_t *pm, int64_t pml, int hand);
	using SrLoopBeginFn = uint64_t (*)(int64_t unused, uint8_t *ps, int hand, char *emptyOut);
	using SrDoneFn = void (*)(int *pm, int flag, int hand);
	using SrLoopDoneFn = char (*)(int *pm, int flag, int hand);  // 0x1427B2F40 returns char (both callers ignore it today)
	using SrTimeFn = int (*)(void *unused, uint64_t weapon);
	using SrWeaponFn = uint64_t (*)(uint64_t weapon);
	using SrBoolFn = bool (*)(uint64_t weapon);
	SrBeginFn g_srBegin;
	SrLoopBeginFn g_srLoopBegin;
	SrLoopDoneFn g_srLoopDone;
	SrDoneFn g_srStartDone;

	// Latch per clientNum: bit 31 set, bits 1..9 the variant, bit 0 = the reload began with an empty clip.
	std::atomic<uint32_t> g_srLatch[64];

	const char *SrModeName(int m) { return m == kSrStart ? "start" : m == kSrEnd ? "end" : m == kSrOff ? "off" : "unset"; }

	int SrParseMode(const char *s)
	{
		if (!_stricmp(s, "end") || !_stricmp(s, "mw") || !_stricmp(s, "mw19") || !_stricmp(s, "mw2019"))
			return kSrEnd;
		if (!_stricmp(s, "start") || !_stricmp(s, "mwii") || !_stricmp(s, "mw2") || !_stricmp(s, "mw22"))
			return kSrStart;
		if (!_stricmp(s, "off") || !strcmp(s, "0"))
			return kSrOff;
		return kSrUnset;
	}

	// Called with every cfg line before ParseWeaponLine. Consumes segreload_* (known = true); wop_alias= is only noted
	// (known = false) so the caller still hands it on.
	bool ParseSegReloadLine(const char *line, bool &known)
	{
		known = false;
		if (strncmp(line, "wop_alias=", 10) == 0)
		{
			char weapon[64] = {}, source[64] = {};
			if (g_srAliasCount < 128 &&
			    sscanf_s(line + 10, "%63[^,],%63[^, \t#]", weapon, static_cast<unsigned>(sizeof(weapon)), source,
			             static_cast<unsigned>(sizeof(source))) == 2)
			{
				SrAlias &a = g_srAliases[g_srAliasCount++];
				strcpy_s(a.weapon, weapon);
				strcpy_s(a.source, source);
			}
			return true;
		}
		if (strncmp(line, "segreload_enable=", 17) == 0)
		{
			known = true;
			int on;
			return sscanf_s(line + 17, "%d", &on) == 1 && ((g_srEnable = on != 0), true);
		}
		if (strncmp(line, "segreload_empty=", 16) != 0)
			return true;
		known = true;
		char weapon[64] = {}, mode[16] = {};
		if (sscanf_s(line + 16, "%63[^,],%15[^, \t#]", weapon, static_cast<unsigned>(sizeof(weapon)), mode,
		             static_cast<unsigned>(sizeof(mode))) != 2)
			return false;
		int m = SrParseMode(mode);
		if (m == kSrUnset)
			return false;
		for (int i = 0; i < g_srCount; i++)
			if (strcmp(g_srEntries[i].weapon, weapon) == 0)
				return g_srEntries[i].mode = m, true;
		if (g_srCount >= 64)
			return false;
		strcpy_s(g_srEntries[g_srCount].weapon, weapon);
		g_srEntries[g_srCount++].mode = m;
		return true;
	}

	int SrExplicitMode(const char *name)
	{
		for (int i = 0; i < g_srCount; i++)
			if (strcmp(g_srEntries[i].weapon, name) == 0)
				return g_srEntries[i].mode;
		return kSrUnset;
	}

	// The configured mode for a weapon name: its own line, else its wop_alias source's line.
	int SrModeForName(const char *name)
	{
		int m = SrExplicitMode(name);
		if (m != kSrUnset)
			return m;
		for (int i = 0; i < g_srAliasCount; i++)
			if (strcmp(g_srAliases[i].weapon, name) == 0 && (m = SrExplicitMode(g_srAliases[i].source)) != kSrUnset)
				return m;
		return kSrOff;
	}

	uint8_t *SrVariant(uint64_t weapon) { return At<uint8_t *>(kSrVariants)[weapon & 0x1FF]; }

	const char *SrSlot(const uint8_t *variant, int node)
	{
		const char *const *anims = *reinterpret_cast<const char *const *const *>(variant + kSrVarAnims);
		return anims ? anims[node] : nullptr;
	}

	// The mode the weapon in hand runs, or kSrOff. Only called on reload transitions (a few times per reload), so no
	// cache: a name compare against the cfg list. The variant pointer and its name are the ones BG just used.
	// The empty slot must hold its own anim (not blank: a blank node plays idle; not the loop: the GDT isn't set up),
	// else the weapon runs as stock.
	int SrModeFor(uint64_t weapon)
	{
		if (!weapon || !(weapon & 0x1FF))
			return kSrOff;
		uint8_t *variant = SrVariant(weapon);
		const char *name = variant ? *reinterpret_cast<const char *const *>(variant) : nullptr;
		if (!name)
			return kSrOff;
		int mode = SrModeForName(name);
		if (mode == kSrOff)
			return kSrOff;
		// Segmented guns only: a mag reload (e.g. a PaP that inherits the mode through wop_alias) has its own
		// reloadEmptyAnim and must stay stock. Start mode also needs a start to replace.
		const uint8_t *def = *reinterpret_cast<uint8_t *const *>(variant + kSrVarDef);
		const bool segmented = reinterpret_cast<SrBoolFn>(g_base + kSrIsSegmented)(weapon);
		const bool hasStart = def && *reinterpret_cast<const int32_t *>(def + kSrDefStartTime) > 0;
		const char *empty = SrSlot(variant, kSrNodeReloadEmpty), *loop = SrSlot(variant, kSrNodeReload);
		bool usable = segmented && (mode != kSrStart || hasStart) && empty && empty[0] && !(loop && strcmp(empty, loop) == 0);
		// First sight of this variant (per pointer): the fields the feature relies on, so the offsets can be checked
		// against the GDT in the log.
		static std::atomic<uint8_t *> s_seen[512];
		if (s_seen[weapon & 0x1FF].exchange(variant) != variant)
		{
			auto i32 = [](const uint8_t *p, size_t o) { return p ? *reinterpret_cast<const int32_t *>(p + o) : -1; };
			const char *start = SrSlot(variant, kSrNodeReloadStart), *end = SrSlot(variant, kSrNodeReloadEnd);
			Log("segreload: %s (variant %d) mode %s%s | reloadAnim '%s' %d ms, reloadEmptyAnim '%s' %d ms (add %d), "
			    "reloadStartAnim '%s' %d ms (add at %d), reloadEndAnim '%s' %d ms, reloadAddTime %d",
			    name, static_cast<int>(weapon & 0x1FF), SrModeName(mode),
			    usable           ? ""
			    : !segmented     ? " -- NOT USED: not a segmented reload"
			    : mode == kSrStart && !hasStart ? " -- NOT USED: start mode needs a reloadStartTime"
			                     : " -- NOT USED: reloadEmptyAnim is blank or the same as reloadAnim",
			    loop ? loop : "",
			    i32(variant, kSrVarReloadTime), empty ? empty : "", i32(variant, kSrVarReloadEmptyTime), i32(def, kSrDefEmptyAddTime),
			    start ? start : "", i32(def, kSrDefStartTime), i32(def, kSrDefStartAddTime), end ? end : "", i32(def, kSrDefEndTime),
			    i32(def, kSrDefAddTime));
		}
		return usable ? mode : kSrOff;
	}

	int32_t &SrI32(uint8_t *ps, size_t off) { return *reinterpret_cast<int32_t *>(ps + off); }
	uint16_t &SrAnim(uint8_t *ps) { return *reinterpret_cast<uint16_t *>(ps + kSrPsWeapAnim); }
	bool SrReloadState(int s) { return s >= 12 && s <= 20; }

	// The clip of `weapon` in ps->ammoInClip, as BG finds it (heldWeapons[i] == its clip weapon); nullptr if not held.
	int32_t *SrClip(uint8_t *ps, uint64_t weapon)
	{
		uint64_t key = reinterpret_cast<SrWeaponFn>(g_base + kSrClipSlotWeapon)(weapon);
		for (int i = 0; i < 15; i++)
			if (*reinterpret_cast<uint64_t *>(ps + kSrPsHeld + i * kSrPsHeldStride) == key)
				return reinterpret_cast<int32_t *>(ps + kSrPsClip + i * 4);
		return nullptr;
	}

	std::atomic<uint32_t> &SrLatchOf(uint8_t *ps) { return g_srLatch[SrI32(ps, kSrPsClientNum) & 63]; }

	// ---- hooks (server Pmove and client prediction both come through here) ---------------------------------------------
	// Reload begin. Latches "began empty" when a reload state is entered; start mode then swaps the normal START for the
	// first segment (RELOAD_EMPTY = the empty start), exactly what BG does on its own for a gun with no start.
	uint64_t SrBeginHook(int64_t *pm, int64_t pml, int hand)
	{
		uint8_t *ps = *reinterpret_cast<uint8_t **>(pm);
		if (hand != 0)
			return g_srBegin(pm, pml, hand);
		const int before = SrI32(ps, kSrPsWeaponState);
		const uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kSrPsWeapon);
		const int32_t *clip = weapon ? SrClip(ps, weapon) : nullptr;
		const bool empty = clip && *clip == 0;
		uint64_t ret = g_srBegin(pm, pml, hand);
		const int after = SrI32(ps, kSrPsWeaponState);
		if (SrReloadState(before) || !SrReloadState(after))
			return ret;  // no reload began
		SrLatchOf(ps).store(0x80000000u | static_cast<uint32_t>(weapon & 0x1FF) << 1 | (empty ? 1u : 0u),
		                    std::memory_order_relaxed);
		if (!empty || (after != 15 && after != 16) || (SrAnim(ps) & 0x1FFF) != kSrAnimReloadStart || SrModeFor(weapon) != kSrStart)
			return ret;
		char emptyOut = 0;
		g_srLoopBegin(reinterpret_cast<int64_t>(pm), ps, 0, &emptyOut);  // retail's reads pm (byte [pm+2ACh]); Enhanced's ignores it
		HotLog("segreload: client %d: began empty, empty start instead of the start (state %d -> %d, anim %d, %d ms)",
		       SrI32(ps, kSrPsClientNum), after, SrI32(ps, kSrPsWeaponState), SrAnim(ps) & 0x1FFF, SrI32(ps, kSrPsWeaponTime));
		return ret;
	}

	// Segment begin. End mode: BG must never pick RELOAD_EMPTY for a loop segment (that node is the rechamber end now),
	// so an empty clip reads 1 for this one call. Only this ps is touched, and only for the call.
	uint64_t SrLoopBeginHook(int64_t unused, uint8_t *ps, int hand, char *emptyOut)
	{
		const uint64_t weapon = hand == 0 ? *reinterpret_cast<uint64_t *>(ps + kSrPsWeapon) : 0;
		int32_t *clip = weapon ? SrClip(ps, weapon) : nullptr;
		if (!clip || *clip != 0 || SrModeFor(weapon) != kSrEnd)
			return g_srLoopBegin(unused, ps, hand, emptyOut);
		*clip = 1;
		uint64_t ret = g_srLoopBegin(unused, ps, hand, emptyOut);
		*clip = 0;
		HotLog("segreload: client %d: empty loop segment kept on the loop anim (anim %d, %d ms)", SrI32(ps, kSrPsClientNum),
		       SrAnim(ps) & 0x1FFF, SrI32(ps, kSrPsWeaponTime));
		return ret;
	}

	// After a start / loop segment ends: when BG has just entered the END state (18, RELOAD_END) for an end-mode gun whose
	// reload began empty, play RELOAD_EMPTY (the rechamber end) for reloadEmptyTime instead. The toggle bit BG set is kept.
	void SrAfterSegment(uint8_t *ps, int before)
	{
		if (before == kSrStateEnd || SrI32(ps, kSrPsWeaponState) != kSrStateEnd || (SrAnim(ps) & 0x1FFF) != kSrAnimReloadEnd)
			return;
		const uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kSrPsWeapon);
		const uint32_t latch = SrLatchOf(ps).load(std::memory_order_relaxed);
		if (latch != (0x80000000u | static_cast<uint32_t>(weapon & 0x1FF) << 1 | 1u) || SrModeFor(weapon) != kSrEnd)
			return;
		const int endTime = SrI32(ps, kSrPsWeaponTime);
		const int emptyTime = reinterpret_cast<SrTimeFn>(g_base + kSrReloadEmptyTime)(ps, weapon);
		SrAnim(ps) = static_cast<uint16_t>((SrAnim(ps) & 0xE000) | kSrAnimReloadEmpty);
		if (emptyTime > 0)
			SrI32(ps, kSrPsWeaponTime) = emptyTime;
		HotLog("segreload: client %d: empty end (rechamber) %d ms instead of the end's %d ms", SrI32(ps, kSrPsClientNum),
		       SrI32(ps, kSrPsWeaponTime), endTime);
	}

	char SrLoopDoneHook(int *pm, int flag, int hand)
	{
		uint8_t *ps = *reinterpret_cast<uint8_t **>(pm);
		const int before = SrI32(ps, kSrPsWeaponState);
		const char r = g_srLoopDone(pm, flag, hand);
		if (hand == 0)
			SrAfterSegment(ps, before);
		return r;  // forwarded as the original returns it
	}

	void SrStartDoneHook(int *pm, int flag, int hand)
	{
		uint8_t *ps = *reinterpret_cast<uint8_t **>(pm);
		const int before = SrI32(ps, kSrPsWeaponState);
		g_srStartDone(pm, flag, hand);
		if (hand == 0)
			SrAfterSegment(ps, before);
	}

	// ---- install --------------------------------------------------------------------------------------------------------
	// Call-site rel32 redirects (the BG callees sit in Arxan tables: no prologue detours), all through one stub page.
	// Every site is checked before any is patched: all or nothing.
	void InstallSegReload()
	{
		if (WtDebugSkip("segreload"))
			return;
		if (g_srHooked || !g_srCount)
			return;
		if (!g_srEnable)
		{
			Log("segreload: segreload_enable=0; not installed (%d weapon line(s))", g_srCount);
			return;
		}
		if (!WtExeSupported())
		{
			Log("segreload: unknown exe; not installed");
			return;
		}
		struct Group
		{
			const SrSite *sites;
			int n;
			void *hook;
			void **orig;
		} groups[] = {
		    {kSrBeginSites, 2, reinterpret_cast<void *>(&SrBeginHook), reinterpret_cast<void **>(&g_srBegin)},
		    {kSrLoopSites, 3, reinterpret_cast<void *>(&SrLoopBeginHook), reinterpret_cast<void **>(&g_srLoopBegin)},
		    {kSrLoopDoneSites, 2, reinterpret_cast<void *>(&SrLoopDoneHook), reinterpret_cast<void **>(&g_srLoopDone)},
		    {kSrStartDoneSites, 1, reinterpret_cast<void *>(&SrStartDoneHook), reinterpret_cast<void **>(&g_srStartDone)},
		};
		for (const Group &g : groups)
			for (int i = 0; i < g.n; i++)
			{
				uint8_t *site = At<uint8_t>(g.sites[i].call);
				int32_t rel;
				memcpy(&rel, site + 1, 4);
				if (site[0] != 0xE8 || site + 5 + rel != At<uint8_t>(g.sites[i].target))
				{
					Log("segreload: +%zx isn't a call to +%zx (another build, or already hooked); not installed", g.sites[i].call,
					    g.sites[i].target);
					return;
				}
			}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(At<uint8_t>(kSrBeginReload)), 0x1000));
		if (!stub)
		{
			Log("segreload: no memory near the exe; not installed");
			return;
		}
		uint8_t *p = stub;
		int patched = 0;
		for (const Group &g : groups)
		{
			*g.orig = At<uint8_t>(g.sites[0].target);
			memcpy(p, "\xFF\x25\x00\x00\x00\x00", 6);  // jmp [rip] -> hook
			memcpy(p + 6, &g.hook, 8);
			for (int i = 0; i < g.n; i++)
			{
				uint8_t *site = At<uint8_t>(g.sites[i].call);
				int32_t newRel = static_cast<int32_t>(p - (site + 5));
				DWORD old;
				VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
				memcpy(site + 1, &newRel, 4);
				VirtualProtect(site + 1, 4, old, &old);
				FlushInstructionCache(GetCurrentProcess(), site, 5);
				patched++;
			}
			p += 16;
		}
		g_srHooked = true;
		Log("segreload: hooked %d call sites (reload begin, segment begin, loop / start done); %d weapon line(s), %d alias(es) seen",
		    patched, g_srCount, g_srAliasCount);
		for (int i = 0; i < g_srCount; i++)
			Log("segreload:   %s -> %s", g_srEntries[i].weapon, SrModeName(g_srEntries[i].mode));
	}
}
