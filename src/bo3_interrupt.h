// IW-style weapon state interrupts: a raise, reload, reload start / loop / end or the post-bolt part of a rechamber can
// end early once it reaches its interrupt point, when an action the cfg allows is pending (fire, ADS, sprint, melee,
// reload, weapon switch). Enhanced exe only. Recon: xpakcap\interrupt\ (d1-d4.txt) and xpakcap\segreload\.
//
// IW8 (bg_weapons.cpp PM_Weapon_IsInInterruptibleState / PM_Weapon_ProcessHand): every frame
//   interruptible = elapsed >= interruptMs (per-anim timers in WeaponAnimPackageStateTimers, or the 'interruptible'
//   notetrack), and ProcessHand runs the state's finish logic when delayedAction || timers done || interruptible, so
//   the pending action takes over. Nothing happens on its own: with no action the state plays to its timer.
// BO3 (PM_Weapon 0x1427C1EA0, per hand): BG_UpdateWeaponTimers 0x1427B4AC0 counts weaponTime (ps+0x54) and weaponDelay
//   (ps+0x58) down; a delay reaching 0 is the "delayed action" (reload ammo add 0x1427B29E0, rechamber bolt 8 -> 9).
//   The input checks run next (sprint 0xC0170, reload 0xB3990, melee 0xBB8D0, switch 0xB5480, ...), then, when the
//   delay fired or both timers are 0, the state's finish (the switch in PM_Weapon: raise 1/2 -> 0, reload 12-14 ->
//   0x1427B2F40, 15-17 -> 0x1427B2D90, 18 -> 0, rechamber 9 -> 0x1427AFB50 in the timer code), and then the fire check.
// So an interrupt here is "the state's timer runs out now": right before PM_Weapon for hand 0, when the state is past its
//   interrupt point, its delayed action has happened (weaponDelay == 0) and an allowed action is pending, weaponTime is
//   set to 1 ms. The engine's own expiry and finish code then runs this frame, exactly as at the natural end (ammo,
//   chamber, segmented-reload decisions, events, the next state's anim); the action itself starts on the next frame
//   from READY, through the engine's own checks. A reload press is also queued (ps hand queuedAction = 2, which the
//   reload check 0x1427B3990 consumes), so a one-frame tap isn't lost.
//
// Prediction: decided in BG (both the server's Pmove and the client's prediction replays call PM_Weapon through the
// same call sites), from the ps and usercmd being processed and the cfg only; it writes only that ps (weaponTime, and
// queuedAction for a reload). The per-variant cache holds values derived from assets, the same on both sides.
//
// Ammo / chamber (why weaponDelay must be 0):
//   reload (mag, start, loop shell): weaponDelay = min(addTime, stateTime) (0x1427B0000; no add time -> the whole state),
//     so before the add point nothing is interrupted (no ammo is added by us, ever); after it the ammo is already in the
//     clip and stays there. A gun without a reload add time is only ever finished by its own timer.
//   rechamber: state 8 (weaponDelay = rechamberBoltTime, 0x1427AFC80) is never touched; state 9 (after the bolt) ends
//     through 0x1427AFB50, the engine's own rechamber finish, so the round counts as chambered exactly as at the end.
//     Reload, melee and weapon switch out of a rechamber are already native (reload begin 0x1427B0DC0 accepts 8 / 9 and
//     handles the chamber itself; melee needs weaponDelay == 0; switch doesn't block 8 / 9).
//   reload end (incl. segreload's empty "rechamber" end): the ammo was added by the segments; the end only animates.
// Anim: the engine blends to whatever the next state plays. If that is idle (ps anim 0), the viewmodel's idle path lets
//   the current node finish first (bo3_inspect.h), so the reload / raise tail plays out under READY, as IW8 does.
//
// Config (weapon_tech.cfg; not live):
//   interrupt_enable=0|1                 global switch (default 1; nothing is installed without interrupt= lines)
//   interrupt_debug=1                    log every interrupt (and why a pending action didn't interrupt, once per state)
//   interrupt_trace=1                    log hand 0's weapon state / anim changes in BG (server and prediction ps), debug
//   interrupt_empty_melee=0|1            (default 0) an empty gun's post-melee quick raise stays a quick raise, so melee
//                                        can interrupt it as with ammo (rapid melee); see InstallEmptyMeleeRaise
//   interrupt=<weapon>,<states>,<source>[,<actions>]
//     states   one or more of raise first_raise quick_raise empty_raise raises reload reload_empty reload_start
//              reload_loop reload_end reloads rechamber all (joined by + or ,). "reload" = the plain mag reload,
//              "reloads" = reload + reload_empty + reload_start + reload_loop + reload_end, "raises" = the four raises.
//     source   one or more of the following, joined by |, first that resolves wins:
//              notetrack        the anim's 'interruptible' note, else its 'state_timer_end' note
//              interruptible    only 'interruptible';  state_timer_end   only 'state_timer_end'
//              fingers          the last fingers_in_end_* note (heuristic for MW anims; opt-in)
//              statetimer       the state's own timer (1.0: the native end; IW8's "state timer is the interrupt point")
//              0.85 | 85%       a fraction of the state
//              850 | 850ms      ms of the GDT state time (IW8 iReloadInterruptTime etc. from the anim package)
//     actions  optional, joined by + or ,: fire ads sprint melee reload switch all none (default: IW8, see below)
//   Later lines for the same weapon and state replace earlier ones. wop_alias=<pap>,<source> gives the PaP its source's
//   lines unless it has its own.
//
// Defaults (IW8, minus what BO3 already does natively):
//   raises       fire, melee, reload, switch   (ADS and sprint already work during a BO3 raise)
//   reload(_empty), reload_end   fire, ads, sprint   (melee and switch already cancel a BO3 reload at any time)
//   reload_start, reload_loop    fire   (BO3's own: fire during a segmented reload -> state 13 / 17, end after the shell)
//   rechamber    none   (IW8 never fires out of a rechamber; reload / melee / switch are native). Add fire / ads to opt in.
#pragma once
#include <atomic>

namespace
{
	// ---- Engine addresses (RVAs): Enhanced values; bo3_retail.h overwrites them on retail ---------------------------
	uintptr_t kIntPmWeapon = 0x27C1EA0;     // PM_Weapon(pmove*, pml*, hand)
	uintptr_t kIntPmWeaponSites[] = {0x27C48C7, 0x27C48D6};  // in 0x1427C4860: hand 0, hand 1
	uintptr_t kIntAnimToNode = 0x27CC980;   // (ps, weapAnim, weapon, hand[, u8 0 on retail]) -> viewmodel node
	uintptr_t kIntNodeTimes = 0x3119410;    // per node {i64 defOff, i64 variantOff, int (*fn)(void *, weapon)}
	uintptr_t kIntNode41TimeFn = 0x27D1340; // node 41's time fn (the reload-empty time), the table check
	uintptr_t kIntIsSegmented = 0x27CFB80;  // (weapon) -> segmentedReload
	uintptr_t &kIntVariants = kWeaponVariants;  // WeaponVariantDef* [512]
	// the SL table is bo3_additive.h's kSLTable
	constexpr int kIntXAnimPartsType = 3;             // DB pool index of XAnimParts (DB_FindXAssetHeader(3, name))

	// pmove_t
	constexpr size_t kIntPmPs = 0x0, kIntPmButtons = 0xC, kIntPmCmdWeapon = 0x28;
	// usercmd button bits: bit n of the u32 array is 0x80000000 >> (n & 31) (BG's own test)
	constexpr int kIntBitAttack = 0, kIntBitMelee = 2, kIntBitReload = 4;
	// playerState_t (hand 0)
	constexpr size_t kIntPsPmFlags = 0x10, kIntPsWeaponTime = 0x54, kIntPsWeaponDelay = 0x58, kIntPsWeaponState = 0x5C,
	                 kIntPsWeapAnim = 0x64, kIntPsQueued = 0x6C, kIntPsWeapon = 0x2C0, kIntPsClientNum = 0x0;
	constexpr uint64_t kIntPmfSprint = 0x20, kIntPmfAds = 0x400000;  // PMF_SPRINTING (0x1427C0170), ADS intent (0x1427A9F30)
	constexpr int kIntQueuedReload = 2;
	// WeaponVariantDef / XAnimParts
	constexpr size_t kIntVarDef = 0x18, kIntVarAnims = 0x48;
	constexpr size_t kIntDefRechamberTime = 0xA24;  // weaponTime of rechamber state 8 (0x1427AFC80; bolt time at +0xA28)
	constexpr size_t kIntPartsNotify = 0xC0, kIntPartsNotifyCount = 0xC8;

	// ---- categories, actions, sources ----------------------------------------------------------------------------------
	enum IntCat : int
	{
		kIcRaise, kIcFirstRaise, kIcQuickRaise, kIcEmptyRaise, kIcReload, kIcReloadEmpty, kIcReloadStart, kIcReloadLoop,
		kIcReloadEnd, kIcRechamber, kIcCount
	};
	const char *const kIntCatNames[kIcCount] = {"raise", "first_raise", "quick_raise", "empty_raise", "reload",
	                                            "reload_empty", "reload_start", "reload_loop", "reload_end", "rechamber"};
	enum : uint8_t { kIaFire = 1, kIaAds = 2, kIaSprint = 4, kIaMelee = 8, kIaReload = 16, kIaSwitch = 32, kIaAll = 63 };
	const char *const kIntActNames[6] = {"fire", "ads", "sprint", "melee", "reload", "switch"};
	constexpr uint8_t kIntDefaultActs[kIcCount] = {
	    kIaFire | kIaMelee | kIaReload | kIaSwitch, kIaFire | kIaMelee | kIaReload | kIaSwitch,
	    kIaFire | kIaMelee | kIaReload | kIaSwitch, kIaFire | kIaMelee | kIaReload | kIaSwitch,
	    kIaFire | kIaAds | kIaSprint, kIaFire | kIaAds | kIaSprint, kIaFire, kIaFire, kIaFire | kIaAds | kIaSprint, 0};
	// What each category can do at all: a reload can't interrupt a reload; a start / loop only ever ends on fire (anything
	// else would just start the next shell early).
	constexpr uint8_t kIntCanActs[kIcCount] = {kIaAll, kIaAll, kIaAll, kIaAll, kIaAll & ~kIaReload, kIaAll & ~kIaReload,
	                                           kIaFire | kIaMelee | kIaSwitch, kIaFire | kIaMelee | kIaSwitch,
	                                           kIaAll & ~kIaReload, kIaAll};

	enum IntSrcKind : uint8_t { kIsNone, kIsNotetrack, kIsInterruptible, kIsStateTimerEnd, kIsFingers, kIsStateTimer, kIsFraction, kIsMs };
	struct IntSrc
	{
		uint8_t kind;
		float value;  // fraction or ms
	};
	struct IntRule
	{
		bool set;
		uint8_t n;
		IntSrc chain[4];
		uint8_t acts;
	};
	struct IntEntry
	{
		char weapon[64];
		IntRule rule[kIcCount];
	};
	struct IntAlias
	{
		char weapon[64], source[64];
	};
	constexpr int kIntMaxEntries = 128, kIntMaxAliases = 128;
	IntEntry g_intEntries[kIntMaxEntries];
	int g_intCount;
	IntAlias g_intAliases[kIntMaxAliases];
	int g_intAliasCount;
	bool g_intEnable = true, g_intDebug, g_intHooked, g_intTrace, g_intEmptyMelee;

	using IntPmWeaponFn = void (*)(void *pm, void *pml, int hand);
	// Retail's has a 5th argument (a byte the CG callers pass as 0; only forwarded to a callee that ignores it). Enhanced's
	// ignores the extra stack slot.
	using IntAnimToNodeFn = int (*)(void *ps, int weapAnim, uint64_t weapon, int hand, uint8_t extra);
	using IntTimeFn = int (*)(void *unused, uint64_t weapon);
	using IntBoolFn = bool (*)(uint64_t weapon);
	IntPmWeaponFn g_intPmWeapon;

	// ---- cfg ------------------------------------------------------------------------------------------------------------
	// Splits s at ',' and '+' (and '|' when keepBar is false) into lowercase tokens; returns the count.
	int IntTokens(const char *s, char (*tok)[48], int max)
	{
		int n = 0;
		while (*s && n < max)
		{
			while (*s == ' ' || *s == '\t')
				s++;
			if (!*s || *s == '#')
				break;
			size_t len = strcspn(s, ",+ \t#");
			if (len)
			{
				size_t c = len < 47 ? len : 47;
				for (size_t i = 0; i < c; i++)
					tok[n][i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
				tok[n++][c] = 0;
			}
			s += len;
			if (*s == ',' || *s == '+')
				s++;
		}
		return n;
	}

	// State names -> category mask (0: not a state name).
	uint32_t IntStateMask(const char *t)
	{
		for (int c = 0; c < kIcCount; c++)
			if (!strcmp(t, kIntCatNames[c]))
				return 1u << c;
		if (!strcmp(t, "raises"))
			return 0xFu;
		if (!strcmp(t, "reloads"))
			return (1u << kIcReload) | (1u << kIcReloadEmpty) | (1u << kIcReloadStart) | (1u << kIcReloadLoop) | (1u << kIcReloadEnd);
		if (!strcmp(t, "all"))
			return (1u << kIcCount) - 1;
		return 0;
	}

	int IntActMask(const char *t)
	{
		for (int a = 0; a < 6; a++)
			if (!strcmp(t, kIntActNames[a]))
				return 1 << a;
		if (!strcmp(t, "all"))
			return kIaAll;
		if (!strcmp(t, "none"))
			return 0x100;  // explicit empty
		if (!strcmp(t, "weaponswitch") || !strcmp(t, "swap"))
			return kIaSwitch;
		return -1;
	}

	// One source element ("notetrack", "0.85", "85%", "850ms", ...). false: not a source.
	bool IntParseSrc(const char *t, IntSrc &s)
	{
		static const struct
		{
			const char *name;
			uint8_t kind;
		} kNames[] = {{"notetrack", kIsNotetrack}, {"note", kIsNotetrack}, {"interruptible", kIsInterruptible},
		              {"state_timer_end", kIsStateTimerEnd}, {"fingers", kIsFingers}, {"statetimer", kIsStateTimer},
		              {"state_timer", kIsStateTimer}};
		for (const auto &k : kNames)
			if (!strcmp(t, k.name))
				return s = {k.kind, 0}, true;
		char *end = nullptr;
		double v = strtod(t, &end);
		if (end == t || v < 0)
			return false;
		if (!strcmp(end, "%"))
			return v <= 100 && (s = {kIsFraction, static_cast<float>(v / 100.0)}, true);
		if (!strcmp(end, "ms"))
			return s = {kIsMs, static_cast<float>(v)}, true;
		if (*end)
			return false;
		if (strchr(t, '.') && v <= 1.0)
			return s = {kIsFraction, static_cast<float>(v)}, true;
		return v > 1.0 && (s = {kIsMs, static_cast<float>(v)}, true);
	}

	// A source chain "notetrack|fingers|0.9".
	bool IntParseChain(const char *t, IntRule &r)
	{
		r.n = 0;
		char buf[48];
		strcpy_s(buf, t);
		for (char *ctx = nullptr, *p = strtok_s(buf, "|", &ctx); p; p = strtok_s(nullptr, "|", &ctx))
		{
			if (r.n >= 4 || !IntParseSrc(p, r.chain[r.n]))
				return false;
			r.n++;
		}
		return r.n > 0;
	}

	// Called with every cfg line before ParseWeaponLine: consumes interrupt* (known = true); notes wop_alias= (known = false).
	bool ParseInterruptLine(const char *line, bool &known)
	{
		known = false;
		if (strncmp(line, "wop_alias=", 10) == 0)
		{
			char weapon[64] = {}, source[64] = {};
			if (g_intAliasCount < kIntMaxAliases &&
			    sscanf_s(line + 10, "%63[^,],%63[^, \t#]", weapon, static_cast<unsigned>(sizeof(weapon)), source,
			             static_cast<unsigned>(sizeof(source))) == 2)
			{
				strcpy_s(g_intAliases[g_intAliasCount].weapon, weapon);
				strcpy_s(g_intAliases[g_intAliasCount++].source, source);
			}
			return true;
		}
		int v;
		if (strncmp(line, "interrupt_enable=", 17) == 0)
			return known = true, sscanf_s(line + 17, "%d", &v) == 1 && ((g_intEnable = v != 0), true);
		if (strncmp(line, "interrupt_debug=", 16) == 0)
			return known = true, sscanf_s(line + 16, "%d", &v) == 1 && ((g_intDebug = v != 0), true);
		if (strncmp(line, "interrupt_trace=", 16) == 0)
			return known = true, sscanf_s(line + 16, "%d", &v) == 1 && ((g_intTrace = v != 0), true);
		if (strncmp(line, "interrupt_empty_melee=", 22) == 0)
			return known = true, sscanf_s(line + 22, "%d", &v) == 1 && ((g_intEmptyMelee = v != 0), true);
		if (strncmp(line, "interrupt=", 10) != 0)
			return true;
		known = true;
		char weapon[64] = {};
		if (sscanf_s(line + 10, "%63[^,]", weapon, static_cast<unsigned>(sizeof(weapon))) != 1)
			return false;
		const char *rest = line + 10 + strlen(weapon);
		if (*rest != ',')
			return false;
		// weapon,<state tokens...>,<source chain>,<action tokens...>: the first token that isn't a state name is the
		// source; everything after it is actions (so "reload" before the source is a state, after it an action).
		char tok[16][48];
		const int n = IntTokens(rest + 1, tok, 16);
		uint32_t states = 0;
		int i = 0;
		for (; i < n; i++)
		{
			uint32_t m = IntStateMask(tok[i]);
			if (!m)
				break;
			states |= m;
		}
		IntRule r = {};
		if (!states || i >= n || !IntParseChain(tok[i++], r))
			return false;
		int acts = -1;
		for (; i < n; i++)
		{
			int a = IntActMask(tok[i]);
			if (a < 0)
				return false;
			acts = (acts < 0 ? 0 : acts) | a;
		}
		IntEntry *e = nullptr;
		for (int k = 0; k < g_intCount && !e; k++)
			if (strcmp(g_intEntries[k].weapon, weapon) == 0)
				e = &g_intEntries[k];
		if (!e)
		{
			if (g_intCount >= kIntMaxEntries)
				return false;
			e = &g_intEntries[g_intCount++];
			*e = {};
			strcpy_s(e->weapon, weapon);
		}
		r.set = true;
		for (int c = 0; c < kIcCount; c++)
			if (states & (1u << c))
			{
				IntRule &dst = e->rule[c];
				dst = r;
				dst.acts = static_cast<uint8_t>((acts < 0 ? kIntDefaultActs[c] : (acts & kIaAll)) & kIntCanActs[c]);
			}
		return true;
	}

	// The cfg entry of a weapon name: its own, else its wop_alias source's.
	const IntEntry *IntEntryFor(const char *name)
	{
		for (int pass = 0; pass < 2 && name; pass++)
		{
			for (int i = 0; i < g_intCount; i++)
				if (strcmp(g_intEntries[i].weapon, name) == 0)
					return &g_intEntries[i];
			const char *source = nullptr;
			for (int i = 0; i < g_intAliasCount && !source; i++)
				if (strcmp(g_intAliases[i].weapon, name) == 0)
					source = g_intAliases[i].source;
			name = source;
		}
		return nullptr;
	}

	// ---- anim data ------------------------------------------------------------------------------------------------------
	const char *IntScrText(uint32_t id)
	{
		const uint8_t *sl = *At<uint8_t *>(kSLTable);
		if (!id || !sl || id > 0x100000)
			return nullptr;
		const char *t = reinterpret_cast<const char *>(sl + 28 * static_cast<size_t>(id) + 4);
		return FastReadable(t, 64) && strnlen(t, 64) < 64 ? t : nullptr;
	}

	// The loaded XAnimParts called `name` (a walk over the DB pool: cold path, once per variant and node), or nullptr.
	const uint8_t *IntFindParts(const char *name)
	{
		static XAssetPool *s_pools;
		if (!s_pools)
			s_pools = FindAssetPools();
		if (!s_pools)
			return nullptr;
		const XAssetPool &p = s_pools[kIntXAnimPartsType];
		if (!p.pool || p.itemSize < kIntPartsNotifyCount + 1 || p.itemSize > 0x1000 || p.itemCount <= 0)
			return nullptr;
		ReadCache rc;
		const size_t want = strlen(name) + 1;
		for (int i = 0; i < p.itemCount; i++)
		{
			const uint8_t *item = p.pool + static_cast<size_t>(p.itemSize) * i;
			const char *n = *reinterpret_cast<const char *const *>(item);
			// Free slots hold the free-list link (a pointer into the pool) at +0.
			if (!n || (reinterpret_cast<const uint8_t *>(n) >= p.pool &&
			           reinterpret_cast<const uint8_t *>(n) < p.pool + static_cast<size_t>(p.itemSize) * p.itemCount))
				continue;
			if (rc.Readable(n, want) && _stricmp(n, name) == 0)
				return item;
		}
		return nullptr;
	}

	// Normalised time (0..1) of a note in the parts, -1 if absent. kind: kIsInterruptible, kIsStateTimerEnd, kIsFingers
	// (the latest fingers_in_end_*). The name is matched in all three scr fields of the entry (bo3_ik.h: +8 is the one the
	// viewmodel handler reads).
	float IntNoteTime(const uint8_t *parts, uint8_t kind)
	{
		if (!parts || !FastReadable(parts, kIntPartsNotifyCount + 1))
			return -1;
		const int count = parts[kIntPartsNotifyCount];
		const uint8_t *notes = *reinterpret_cast<const uint8_t *const *>(parts + kIntPartsNotify);
		if (!count || !notes || !FastReadable(notes, 16 * static_cast<size_t>(count)))
			return -1;
		float best = -1;
		for (int i = 0; i < count; i++)
		{
			const float t = *reinterpret_cast<const float *>(notes + 16 * i + 4);
			if (!(t >= 0 && t <= 1))
				continue;
			for (size_t field : {size_t(8), size_t(0), size_t(12)})
			{
				const char *txt = IntScrText(*reinterpret_cast<const uint32_t *>(notes + 16 * i + field));
				if (!txt)
					continue;
				bool hit = kind == kIsInterruptible  ? _stricmp(txt, "interruptible") == 0
				           : kind == kIsStateTimerEnd ? _stricmp(txt, "state_timer_end") == 0
				                                      : _strnicmp(txt, "fingers_in_end", 14) == 0;
				if (hit)
				{
					// interruptible / state_timer_end: the first one; fingers: the latest
					if (kind == kIsFingers ? t > best : best < 0)
						best = t;
					break;
				}
			}
		}
		return best;
	}

	uint8_t *IntVariant(uint64_t weapon) { return At<uint8_t *>(kIntVariants)[weapon & 0x1FF]; }

	// The GDT time of a viewmodel node, as the engine's anim-rate code reads it (0x1404DB320: the node-time table).
	int IntNodeTime(uint8_t *ps, uint64_t weapon, int node)
	{
		if (node <= 0 || node >= 197)
			return 0;
		const int64_t *e = At<int64_t>(kIntNodeTimes + 24 * static_cast<size_t>(node));
		if (e[2])
			return reinterpret_cast<IntTimeFn>(e[2])(ps, weapon);
		const uint8_t *variant = IntVariant(weapon);
		if (!variant)
			return 0;
		if (e[0] >= 0)
		{
			const uint8_t *def = *reinterpret_cast<uint8_t *const *>(variant + kIntVarDef);
			return def ? *reinterpret_cast<const int32_t *>(def + e[0]) : 0;
		}
		return e[1] >= 0 ? *reinterpret_cast<const int32_t *>(variant + e[1]) : 0;
	}

	// ---- per variant / category / node: the interrupt point as a fraction of the state (-1: none) -------------------------
	struct IntCached
	{
		std::atomic<uint64_t> key;  // variant pointer ^ (node << 48) ^ (cat << 56), 0 = empty
		float point;
		uint8_t acts;
	};
	IntCached g_intCache[512][kIcCount][4];  // 4 ways: hip / ADS variants of a state use different nodes

	const char *IntSrcName(uint8_t k)
	{
		switch (k)
		{
		case kIsNotetrack: return "notetrack";
		case kIsInterruptible: return "interruptible";
		case kIsStateTimerEnd: return "state_timer_end";
		case kIsFingers: return "fingers";
		case kIsStateTimer: return "statetimer";
		case kIsFraction: return "fraction";
		case kIsMs: return "ms";
		default: return "none";
		}
	}

	float IntResolve(const IntRule &r, const char *animName, int gdtMs, char *why, size_t whyLen)
	{
		const uint8_t *parts = nullptr;
		bool looked = false;
		for (int i = 0; i < r.n; i++)
		{
			const IntSrc &s = r.chain[i];
			float t = -1;
			switch (s.kind)
			{
			case kIsFraction: t = s.value; break;
			case kIsMs: t = gdtMs > 0 ? s.value / static_cast<float>(gdtMs) : -1; break;
			case kIsStateTimer: t = 1.0f; break;
			default:
				if (!looked)
				{
					looked = true;
					parts = animName && animName[0] ? IntFindParts(animName) : nullptr;
				}
				if (s.kind == kIsNotetrack)
				{
					t = IntNoteTime(parts, kIsInterruptible);
					if (t >= 0)
					{
						snprintf(why, whyLen, "note 'interruptible' @ %.3f", t);
						return t;
					}
					t = IntNoteTime(parts, kIsStateTimerEnd);
					if (t >= 0)
					{
						snprintf(why, whyLen, "note 'state_timer_end' @ %.3f", t);
						return t;
					}
				}
				else
					t = IntNoteTime(parts, s.kind);
				break;
			}
			if (t >= 0)
			{
				if (t > 1)
					t = 1;
				snprintf(why, whyLen, "%s%s%.3f", IntSrcName(s.kind), s.kind == kIsMs ? " -> " : " ", t);
				return t;
			}
		}
		snprintf(why, whyLen, "no source resolved%s", looked && !parts ? " (xanim not found in the pool)" : "");
		return -1;
	}

	// The rule for the weapon in hand and category, resolved for this node. false: not configured / no point.
	bool IntPointFor(uint8_t *ps, uint64_t weapon, int cat, int node, int gdtMs, float &point, uint8_t &acts)
	{
		uint8_t *variant = IntVariant(weapon);
		if (!variant)
			return false;
		const uint64_t key = (reinterpret_cast<uint64_t>(variant) & 0xFFFFFFFFFFFFull) ^ (static_cast<uint64_t>(node) << 48) ^
		                     (static_cast<uint64_t>(cat + 1) << 56);
		IntCached *ways = g_intCache[weapon & 0x1FF][cat];
		IntCached *hit = nullptr;
		for (int w = 0; w < 4 && !hit; w++)
			if (ways[w].key.load(std::memory_order_acquire) == key)
				hit = &ways[w];
		if (!hit)
		{
			// a free way, else the one after the last used (round robin per slot)
			int w = 0;
			while (w < 4 && ways[w].key.load(std::memory_order_relaxed))
				w++;
			if (w == 4)
				w = static_cast<int>((key >> 4) + node) & 3;
			IntCached &c = ways[w];
			hit = &c;
			const char *name = *reinterpret_cast<const char *const *>(variant);
			const IntEntry *e = name ? IntEntryFor(name) : nullptr;
			float p = -1;
			uint8_t a = 0;
			char why[96] = "not configured";
			const char *const *anims = *reinterpret_cast<const char *const *const *>(variant + kIntVarAnims);
			const char *animName = anims ? anims[node] : nullptr;
			if (e && e->rule[cat].set && e->rule[cat].acts)
			{
				p = IntResolve(e->rule[cat], animName, gdtMs, why, sizeof(why));
				a = e->rule[cat].acts;
			}
			else if (e && e->rule[cat].set)
				strcpy_s(why, "no actions (IW8 rechamber default: add fire / ads / sprint)");
			if (e)
			{
				char an[48] = {};
				for (int k = 0, o = 0; k < 6; k++)
					if (a & (1 << k))
						o += snprintf(an + o, sizeof(an) - o, "%s%s", o ? "+" : "", kIntActNames[k]);
				Log("interrupt: %s (variant %d) %s: node %d '%s', %d ms -> %s%s%.3f, actions %s", name,
				    static_cast<int>(weapon & 0x1FF), kIntCatNames[cat], node, animName ? animName : "", gdtMs, why,
				    p >= 0 ? ", point " : "", p >= 0 ? p : 0.0f, a ? an : "none");
			}
			c.key.store(0, std::memory_order_relaxed);
			c.point = p;
			c.acts = a;
			c.key.store(key, std::memory_order_release);
		}
		point = hit->point;
		acts = hit->acts;
		return point >= 0 && acts;
	}

	// ---- the state in hand ----------------------------------------------------------------------------------------------
	int IntCategory(int state, int anim, uint64_t weapon)
	{
		switch (state)
		{
		case 1:
		case 2:
			return anim == 38 ? kIcFirstRaise : anim == 54 ? kIcQuickRaise : anim == 56 ? kIcEmptyRaise : kIcRaise;
		case 32:  // post-melee quick raise
			return kIcQuickRaise;
		case 12:
		case 13:
		case 14:
			if (reinterpret_cast<IntBoolFn>(g_base + kIntIsSegmented)(weapon))
				return kIcReloadLoop;
			return anim == 40 || anim == 44 || anim == 46 || anim == 48 ? kIcReloadEmpty : kIcReload;
		case 15:
		case 16:
		case 17:
			return kIcReloadStart;
		case 18:
			return kIcReloadEnd;
		case 9:  // rechamber after the bolt (8 = before it: never)
			return kIcRechamber;
		default:
			return -1;
		}
	}

	bool IntBit(const uint8_t *pm, size_t base, int bit)
	{
		return (*reinterpret_cast<const uint32_t *>(pm + base + 4 * (bit >> 5)) & (0x80000000u >> (bit & 31))) != 0;
	}

	uint8_t IntPending(const uint8_t *pm, const uint8_t *ps)
	{
		uint8_t a = 0;
		const uint64_t pmf = *reinterpret_cast<const uint64_t *>(ps + kIntPsPmFlags);
		if (IntBit(pm, kIntPmButtons, kIntBitAttack))
			a |= kIaFire;
		if (pmf & kIntPmfAds)
			a |= kIaAds;
		if (pmf & kIntPmfSprint)
			a |= kIaSprint;
		if (IntBit(pm, kIntPmButtons, kIntBitMelee))
			a |= kIaMelee;
		if (IntBit(pm, kIntPmButtons, kIntBitReload))
			a |= kIaReload;
		const uint64_t cmdWeapon = *reinterpret_cast<const uint64_t *>(pm + kIntPmCmdWeapon);
		if ((cmdWeapon & 0x1FF) && cmdWeapon != *reinterpret_cast<const uint64_t *>(ps + kIntPsWeapon))
			a |= kIaSwitch;
		return a;
	}

	// Right before PM_Weapon for hand 0.
	void IntBeforePmWeapon(uint8_t *pm)
	{
		uint8_t *ps = *reinterpret_cast<uint8_t **>(pm + kIntPmPs);
		if (!ps)
			return;
		int32_t &weaponTime = *reinterpret_cast<int32_t *>(ps + kIntPsWeaponTime);
		const int state = *reinterpret_cast<const int32_t *>(ps + kIntPsWeaponState);
		if (weaponTime <= 1 || *reinterpret_cast<const int32_t *>(ps + kIntPsWeaponDelay) != 0)
			return;  // nothing left to cut, or the delayed action (ammo add / bolt) hasn't happened
		const uint64_t weapon = *reinterpret_cast<const uint64_t *>(ps + kIntPsWeapon);
		if (!(weapon & 0x1FF))
			return;
		const int anim = *reinterpret_cast<const uint16_t *>(ps + kIntPsWeapAnim) & 0x1FFF;
		const int cat = IntCategory(state, anim, weapon);
		if (cat < 0)
			return;
		const uint8_t pending = IntPending(pm, ps);
		if (!pending)
			return;
		const int node = reinterpret_cast<IntAnimToNodeFn>(g_base + kIntAnimToNode)(ps, anim, weapon, 0, 0);
		// The rechamber's timer is set from the WeaponDef directly (0x1427AFC80), not through the node table.
		int gdtMs;
		if (cat == kIcRechamber)
		{
			const uint8_t *variant = IntVariant(weapon);
			const uint8_t *def = variant ? *reinterpret_cast<uint8_t *const *>(variant + kIntVarDef) : nullptr;
			gdtMs = def ? *reinterpret_cast<const int32_t *>(def + kIntDefRechamberTime) : 0;
		}
		else
			gdtMs = IntNodeTime(ps, weapon, node);
		// Sanity: the timer counts down from the GDT time, so it can't be above it (a wrong node-time guess would show here).
		if (gdtMs > 0 && weaponTime > gdtMs)
			return;
		float point;
		uint8_t acts;
		if (gdtMs <= 0 || !IntPointFor(ps, weapon, cat, node, gdtMs, point, acts) || !(pending & acts))
			return;
		// Elapsed in the engine's own units: weaponTime counts down from the GDT time (perks speed the countdown up,
		// 0x1427B4AC0, they don't change the start value), so the fraction is exact with Speed Cola / Fast Hands too.
		const float elapsed = 1.0f - static_cast<float>(weaponTime) / static_cast<float>(gdtMs);
		if (elapsed + 1e-4f < point)
			return;
		const int before = weaponTime;
		weaponTime = 1;  // the timer runs out this frame: the engine's own expiry / finish code does the rest
		if ((pending & acts & kIaReload) && *reinterpret_cast<int32_t *>(ps + kIntPsQueued) == 0)
			*reinterpret_cast<int32_t *>(ps + kIntPsQueued) = kIntQueuedReload;
		if (g_intDebug)
			Log("interrupt: client %d %s (state %d, anim %d) at %.3f (point %.3f, %d of %d ms left), pending 0x%x allowed 0x%x",
			    *reinterpret_cast<const int32_t *>(ps + kIntPsClientNum), kIntCatNames[cat], state, anim, elapsed, point,
			    before, gdtMs, pending, acts);
	}

	// interrupt_trace=1: hand 0's weapon state / anim changes per ps (server and client prediction replay the same
	// commands, so each change shows once per ps; replays of an old command repeat it, hence the ps tag and the time).
	void IntTrace(const uint8_t *pm, const char *when)
	{
		const uint8_t *ps = *reinterpret_cast<uint8_t *const *>(pm + kIntPmPs);
		if (!ps)
			return;
		struct Seen
		{
			const uint8_t *ps;
			int state, anim;
		};
		static Seen s_seen[4];
		Seen *s = nullptr;
		for (Seen &e : s_seen)
			if (e.ps == ps || (!s && !e.ps))
				s = &e;
		if (!s)
			s = &s_seen[reinterpret_cast<uintptr_t>(ps) >> 4 & 3];
		const int state = *reinterpret_cast<const int32_t *>(ps + kIntPsWeaponState);
		const int anim = *reinterpret_cast<const uint16_t *>(ps + kIntPsWeapAnim) & 0x1FFF;
		if (s->ps == ps && s->state == state && s->anim == anim)
			return;
		*s = {ps, state, anim};
		uint8_t *mps = const_cast<uint8_t *>(ps);
		Log("itrace: %s ps %04llx state %d anim %d time %d delay %d clip %d melee %d attack %d", when,
		    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ps) & 0xFFFF), state, anim,
		    *reinterpret_cast<const int32_t *>(ps + kIntPsWeaponTime), *reinterpret_cast<const int32_t *>(ps + kIntPsWeaponDelay),
		    ClipAmmo(mps, *reinterpret_cast<const uint64_t *>(ps + kIntPsWeapon)), IntBit(pm, kIntPmButtons, kIntBitMelee),
		    IntBit(pm, kIntPmButtons, kIntBitAttack));
	}

	void IntPmWeaponHook(void *pm, void *pml, int hand)
	{
		if (hand == 0)
		{
			if (g_intTrace)
				IntTrace(static_cast<uint8_t *>(pm), "pre ");
			IntBeforePmWeapon(static_cast<uint8_t *>(pm));
		}
		g_intPmWeapon(pm, pml, hand);
		if (hand == 0 && g_intTrace)
			IntTrace(static_cast<uint8_t *>(pm), "post");
	}

	// ---- interrupt_empty_melee=1: an empty gun's post-melee quick raise stays a quick raise -----------------------------
	// After a melee with a weapon that isn't the gun's own (the knife), melee end 0x1427B7EA0 puts the gun in state 32
	// (post-melee quick raise, quickRaiseTime, anim 54), which melee may interrupt (0x1427BB260 blocks only 1-5, 37-51,
	// 83-85, 106-121 and the other melee states): that is the loaded gun's rapid melee. With an EMPTY clip,
	// PM_Weapon_CheckForReload 0x1427B3990 (called from PM_Weapon before the melee check) turns state 32 into state 1
	// (RAISING, emptyRaiseTime WeaponDef+0xB18, anim 56 EMPTY_RAISE) on the next frame:
	//   1427B3A28  cmp dword [rdi+r13+5Ch], 20h / jnz / mov edx, esi / mov rcx, rdi
	//   1427B3A35  call BG_ClipEmpty 0x1427C81B0      <- patched to xor eax, eax (no clip-empty branch)
	//   1427B3A3C  jz ... ; call 0x1427A7CD0 ; jnz ... ; state = 1, weaponTime = def+0xB18, weapAnim = 56
	// and melee can't start in state 1 until the raise has run out (then READY -> the auto reload 12, which melee cancels).
	// Measured on zm_wz_hud (decho, V every 120 ms): loaded 31 -> 32 -> next melee 60 ms later (~565 ms per melee); empty
	// 31 -> 32 -> 1 (250 ms) -> 0 -> 12 -> melee (~840 ms). With the patch the empty gun stays in 32 like the loaded one;
	// the post-melee raise then plays the quick raise, the same as empty_melee_fix=1 does for the normal raise.
	// A code patch in BG, so the server's Pmove and the client's prediction run the same code. Not live.
	uintptr_t kIntPostMeleeEmptyCall = 0x27B3A35;
	uint8_t kIntPostMeleeCtx[] = {0x42, 0x83, 0x7C, 0x2F, 0x5C, 0x20, 0x75, 0x6E, 0x8B, 0xD6, 0x48, 0x8B, 0xCF};
	// Retail (0x2664935): cmp dword [rdi+rbp+5Ch],20h; jnz; mov edx,r14d; mov rcx,rdi | E8 B6 A9 00 00
	const uint8_t kIntPostMeleeCtxRetail[] = {0x83, 0x7C, 0x2F, 0x5C, 0x20, 0x75, 0x73, 0x41, 0x8B, 0xD6, 0x48, 0x8B, 0xCF};
	static_assert(sizeof(kIntPostMeleeCtxRetail) == 13, "13 bytes");

	void InstallEmptyMeleeRaise()
	{
		if (WtDebugSkip("intmelee"))
			return;
		static bool s_done;
		if (s_done || !g_intEmptyMelee)
			return;
		s_done = true;
		if (!WtExeSupported())
		{
			Log("interrupt: interrupt_empty_melee=1: unknown exe; not patched");
			return;
		}
		// E8 76 47 01 00 = call +0x14776 -> 0x1427C81B0 (BG_ClipEmpty): the rel32 ties the bytes to the callee.
		// Retail: E8 B6 A9 00 00 -> BG_ClipEmpty 0x14266F2F0.
		const bool r = IsRetailExe();
		if (PatchBytesCtx(kIntPostMeleeEmptyCall, r ? kIntPostMeleeCtxRetail : kIntPostMeleeCtx, sizeof(kIntPostMeleeCtx),
		                  r ? std::initializer_list<uint8_t>{0xE8, 0xB6, 0xA9, 0x00, 0x00} : std::initializer_list<uint8_t>{0xE8, 0x76, 0x47, 0x01, 0x00},
		                  {0x31, 0xC0, 0x90, 0x90, 0x90}, "interrupt_empty_melee: post-melee quick raise kept with an empty clip"))
			Log("interrupt: interrupt_empty_melee=1: the empty gun's post-melee raise (state 32) stays interruptible by melee");
	}

	// ---- install --------------------------------------------------------------------------------------------------------
	// Both PM_Weapon call sites in 0x1427C4860 (rel32 redirects through one stub; BG callees sit in Arxan tables, so no
	// prologue detours). All or nothing.
	void InstallInterrupt()
	{
		InstallEmptyMeleeRaise();  // independent of the interrupt= lines and the PM_Weapon hook
		if (WtDebugSkip("interrupt"))
			return;
		if (g_intHooked || (!g_intCount && !g_intTrace))
			return;
		if (!g_intEnable)
		{
			Log("interrupt: interrupt_enable=0; not installed (%d weapon line(s))", g_intCount);
			return;
		}
		if (!WtExeSupported())
		{
			Log("interrupt: unknown exe; not installed");
			return;
		}
		for (uintptr_t s : kIntPmWeaponSites)
		{
			uint8_t *site = At<uint8_t>(s);
			int32_t rel;
			memcpy(&rel, site + 1, 4);
			if (site[0] != 0xE8 || site + 5 + rel != At<uint8_t>(kIntPmWeapon))
			{
				Log("interrupt: +%zx isn't a call to PM_Weapon +%zx (another build, or already hooked); not installed", s,
				    kIntPmWeapon);
				return;
			}
		}
		// The node-time table is called through: check it is the engine's (IDA 2026-10-01: node 41 fn = 0x1427D1340,
		// node 43 = {0xAF8, -1, null}). The SL table pointer must be the one `mov rax, [rip+..]` at 0x14011FA2E reads.
		{
			const int64_t *t41 = At<int64_t>(kIntNodeTimes + 24 * 41), *t43 = At<int64_t>(kIntNodeTimes + 24 * 43);
			if (!FastReadable(t41, 24 * 3) || t41[2] != reinterpret_cast<int64_t>(At<uint8_t>(kIntNode41TimeFn)) || t43[0] != 0xAF8 ||
			    t43[2] != 0)
			{
				Log("interrupt: the node-time table at +%zx doesn't match; not installed", kIntNodeTimes);
				return;
			}
			if (!CodeRefers(kSLTableRef, 7, kSLTable))
			{
				Log("interrupt: the SL table constant +%zx doesn't match the code; not installed", kSLTable);
				return;
			}
			Log("interrupt: node-time table and SL table match the code");
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(At<uint8_t>(kIntPmWeapon)), 0x1000));
		if (!stub)
		{
			Log("interrupt: no memory near the exe; not installed");
			return;
		}
		g_intPmWeapon = reinterpret_cast<IntPmWeaponFn>(At<uint8_t>(kIntPmWeapon));
		void *hook = reinterpret_cast<void *>(&IntPmWeaponHook);
		memcpy(stub, "\xFF\x25\x00\x00\x00\x00", 6);  // jmp [rip] -> hook
		memcpy(stub + 6, &hook, 8);
		FlushInstructionCache(GetCurrentProcess(), stub, 14);
		for (uintptr_t s : kIntPmWeaponSites)
		{
			uint8_t *site = At<uint8_t>(s);
			int32_t newRel = static_cast<int32_t>(stub - (site + 5));
			DWORD old;
			VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
			memcpy(site + 1, &newRel, 4);
			VirtualProtect(site + 1, 4, old, &old);
			FlushInstructionCache(GetCurrentProcess(), site, 5);
		}
		g_intHooked = true;
		Log("interrupt: hooked both PM_Weapon calls; %d weapon line(s), %d alias(es) seen%s", g_intCount, g_intAliasCount,
		    g_intDebug ? ", interrupt_debug=1" : "");
		for (int i = 0; i < g_intCount; i++)
		{
			char txt[256] = {};
			int o = 0;
			for (int c = 0; c < kIcCount; c++)
			{
				const IntRule &r = g_intEntries[i].rule[c];
				if (!r.set || o > 200)
					continue;
				o += snprintf(txt + o, sizeof(txt) - o, " %s=", kIntCatNames[c]);
				for (int k = 0; k < r.n; k++)
					o += snprintf(txt + o, sizeof(txt) - o, "%s%s", k ? "|" : "", IntSrcName(r.chain[k].kind));
				o += snprintf(txt + o, sizeof(txt) - o, "/0x%x", r.acts);
			}
			Log("interrupt:   %s:%s", g_intEntries[i].weapon, txt);
		}
	}
}
