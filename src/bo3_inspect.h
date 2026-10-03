// MW-style weapon inspect, client side and visual only (replaces scripts\lilrobot\_inspectable_weapons.gsc).
// Both supported exes (addresses: bo3_build.h / bo3_retail.h). Recon: the author's research notes (e6af0.txt = CG_UpdateViewWeaponAnim, s3.txt = the hook site, t2.txt).
//
// Config (weapon_tech.cfg; not live):
//   inspect_enable=1                     master switch (default 0: nothing is installed)
//   inspect=<weapon>,<1|0>[,<seconds>]   per weapon; seconds overrides the length (default: the GDT's lowReadyLoopTime,
//                                        else the xanim's own length). A wop_alias=<pap>,<source> line gives the PaP its
//                                        source's line unless it has its own.
//   inspect_key=I                        a letter / digit, F1..F24, MOUSE3/MOUSE4/MOUSE5, or a VK code (0x49)
//   inspect_empty=in|out|off             empty inspect (MWII and newer): with an empty clip, play the GDT's
//                                        lowReadyInAnim (in, default) or lowReadyOutAnim (out) instead of the loop, timed by
//                                        its lowReadyIn/OutTime. A blank or idle slot falls back to the normal inspect.
//   inspect_hidehud=1                    also fade the whole HUD out while inspecting (default 0). The crosshair always
//                                        hides. Both are done by the map's LUI from the models bo3_inspect_hud.h publishes.
//   inspect_akimbo=1|0                   dual-wield (akimbo) weapons: 1 (default) inspects them as below, 0 = they don't
//                                        inspect at all. Single-wield weapons never take the akimbo path.
//
// How: the viewmodel plays whatever ps->weapAnim maps to (0x1427CC980: ps anim N -> node; 68 -> node 79 =
// lowReadyLoopAnim, timed by WeaponDef+0xB50 lowReadyLoopTime, so the engine scales the rate to it; an empty inspect
// uses 67 -> 78 / 69 -> 80). Right before CG_UpdateViewWeaponAnim (0x1404E6AF0) runs, the predicted ps's weapAnim is
// set to 68 (with a toggle bit that differs from what the viewmodel last saw), every frame, since prediction rebuilds
// the ps from the snapshot. The engine then starts and blends node 79 itself. Nothing reaches the server: the predicted ps is the client's own copy, and the
// usercmd carries no anim.
// Ending: the engine's own value comes back. A non-idle value (fire, reload, raise, ...) starts that anim with the
// engine's blend. For idle the engine waits for the current anim to finish (its idle path checks the current node's
// time), so a loop would play on: the node's time is set to 1.0 (finished) first, and the engine blends to idle.
//
// Akimbo (dual wield; IDA 2026-10-02 on the Enhanced exe): both hands live in ONE viewmodel DObj tree. The playerState
// keeps a per-hand block of 0x1C bytes at ps+0x54 (weaponTime +0x54/+0x70, weaponState +0x5C/+0x78, weapAnim
// +0x64/+0x80), and the viewmodel info keeps right node +0x368, left node +0x370, right last anim +0x36C, left last
// anim +0x374. CG_UpdateViewWeaponAnim runs a left block (only when BG_IsDualWield, 0x1427CF860) that maps ps+0x80
// with 0x1427CC980(a4 = 1) to the left nodes 161..172 (idle 168, empty idle 170); that map has NO low-ready case, so
// the left hand can't play an inspect slot of its own. When either hand plays a node, 0x1404DB690 fades every other
// node 1..172 to 0 EXCEPT the other hand's current node, so the left idle keeps weight 1 under a right-hand inspect.
// The akimbo inspect therefore plays the right channel's slot as usual (an MW akimbo inspect xanim animates both arms
// and both guns) and fades the left hand's current idle node to 0 with XAnimSetGoalWeight, so the inspect drives the
// left side too; it fades that node back to 1 when the inspect ends (unless the engine moved the left hand on itself).
// The left hand must be READY / idle to start; its state leaving READY or its weapAnim changing (left trigger, reload,
// switch, ...) stops the inspect, alongside the right-hand rules. Empty inspect: either gun's clip empty.
#pragma once
#pragma comment(lib, "user32.lib")  // GetAsyncKeyState, GetForegroundWindow (build.bat links kernel32 only)
#include "bo3_inspect_hud.h"

namespace
{
	// ---- Engine addresses (RVAs): Enhanced values; bo3_retail.h overwrites them on retail ---------------------------
	uintptr_t kInsSite = 0x4F384A;          // CG_UpdateViewModelAnims, the plain bytes before `call CG_UpdateViewWeaponAnim`
	uintptr_t kInsCallUpdate = 0x4F385B;    // E8 -> 0x4E6AF0
	uintptr_t &kInsUpdateViewWeaponAnim = kUpdateViewWeaponAnim;
	uintptr_t kInsKeyCatchers = 0x40E38F4;  // clientUIActive[lc].keyCatchers (u32, stride 4216): 0x10 = LUI, console, chat
	uintptr_t kInsKeyCatchersRef = 0x13DF34B;  // xor [rip+..], 1 on it
	constexpr size_t kInsKeyCatchersStride = 4216;
	uintptr_t &kInsVariants = kWeaponVariants;
	// At the site: rsi = predicted ps, r14 = viewmodel info, edi = local client, rsp 16-aligned, no volatile register live.
	//   44 8B 4D D0  mov r9d,[rbp-30h] | 4D 8B C6  mov r8,r14 | 48 8B D6  mov rdx,rsi | 0F 29 74 24 50  movaps [rsp+50h],xmm6 |
	//   8B CF  mov ecx,edi
	constexpr uint8_t kInsDisplacedEnh[17] = {0x44, 0x8B, 0x4D, 0xD0, 0x4D, 0x8B, 0xC6, 0x48, 0x8B, 0xD6,
	                                          0x0F, 0x29, 0x74, 0x24, 0x50, 0x8B, 0xCF};
	// Retail (0x44B446): r14 = ps, rdi = vm, r15d = local client.
	//   44 8B 4D A7  mov r9d,[rbp-59h] | 4C 8B C7  mov r8,rdi | 49 8B D6  mov rdx,r14 | 41 8B CF  mov ecx,r15d |
	//   0F 29 B4 24 C0 00 00 00  movaps [rsp+0C0h],xmm6
	constexpr uint8_t kInsDisplacedRetail[21] = {0x44, 0x8B, 0x4D, 0xA7, 0x4C, 0x8B, 0xC7, 0x49, 0x8B, 0xD6, 0x41,
	                                             0x8B, 0xCF, 0x0F, 0x29, 0xB4, 0x24, 0xC0, 0x00, 0x00, 0x00};

	// cg (= ps - 0x11A8B0), playerState_t, viewmodel info, WeaponDef
	constexpr size_t kInsCgPs = 0x11A8B0, kInsCgTime = 0x11A88C;
	constexpr size_t kInsPsPmType = 0x08, kInsPsPmFlags = 0x10, kInsPsWeaponState = 0x5C, kInsPsWeapAnim = 0x64,
	                 kInsPsWeapon = 0x2C0, kInsPsAds = 0x2FC, kInsPsCursorHint = 0x708;
	constexpr size_t kInsVmDObj = 0x0, kInsVmRightNode = 0x368, kInsVmLastAnim = 0x36C;
	constexpr size_t kInsVarDef = 0x18, kInsVarAnims = 0x48, kInsDefLowReadyLoopTime = 0xB50;  // read live 2026-10-01: 4800 for the Uzi
	// Empty inspect: 0x1427CC980 maps ps anim 67 -> node 78 (lowReadyInAnim) and 69 -> node 80 (lowReadyOutAnim);
	// their times sit either side of lowReadyLoopTime in the WeaponDef.
	constexpr size_t kInsDefLowReadyInTime = 0xB4C, kInsDefLowReadyOutTime = 0xB54;
	int g_insEmpty = 1;  // inspect_empty: 0 off, 1 in (node 78), 2 out (node 80)
	bool g_insDebugEmpty;  // inspect_debug_empty=1: treat the clip as empty (preview empty inspects)
	constexpr int kInsPsAnim = 68, kInsNode = 79;  // LOWREADY_LOOP -> lowReadyLoopAnim: 0x1427CC980 maps ps anim 67/68/69 -> node 78/79/80 (= szXAnims slot; idle if the slot is blank). 70-72 -> 81-83 are other, usually empty, slots
	constexpr uint64_t kInsPmfProne = 0x1, kInsPmfJuke = 0x40;

	// Weapon states (ps->weaponState, hand 0) and what each means for an inspect.
	//   S = inspect may START from it, I = entering it INTERRUPTS a running inspect.
	// Sources: PM_Weapon 0x1427C1EA0 and its helpers (the author's research notes section 4, segreload\).
	//   0        READY                                        S
	//   1, 2     RAISING, RAISING_ALT                         I   (weapon switch / first raise)
	//   3, 4, 5  DROPPING, DROPPING_QUICK, DROPPING_ALT       I
	//   6        FIRING                                       I
	//   7, 8, 9  RECHAMBER / fire follow-ups (9 cont. fire)   I
	//   10, 11   fire family (burst / delay)                  I
	//   12-14    RELOAD loop (13: fire held, 14: fast mag)    I
	//   15-17    RELOAD_START (17: fire held)                 I
	//   18       RELOAD_END                                   I
	//   19, 20   reload family                                I
	//   21-33    MELEE (21 charge, 28 ammo melee, 30 windup, 31 melee, 32 post-melee quick raise, 33 lunge)  I
	//   34-36    melee / drop follow-ups                      I
	//   37-46    OFFHAND (grenade / equipment prime, hold, throw, end)   I
	//   47-51    offhand / gadget follow-ups                  I
	//   52-59    sprint / crawl transitions (56-59)           I
	//   60-62    LOWREADY in / loop / out (the engine's SetLowReady; anims 70-72)   I
	//   63-66    lowready / deploy follow-ups                 I
	//   67, 90   READY variants (swim / no-gadget ready: 0x1427C1EA0 case 1/2)   I (no swimming in ZM; not a start)
	//   68-89, 91-127  vehicles, gadgets (106-121), swim, ladders, mantle, slide and the rest   I
	// Anything not 0 counts as busy. Besides the state, a running inspect stops when the engine's own weapAnim changes
	// (sprint in / out, slide, mantle, wallrun and everything else that plays an anim), on ADS, prone, juke, death /
	// last stand (pm_type), a weapon change, or when it ends.
	enum : uint8_t { kInsStart = 1, kInsInterrupt = 2 };
	constexpr uint8_t InspectStateRule(int state) { return state == 0 ? kInsStart : kInsInterrupt; }

	// ---- Akimbo (see the header) ---------------------------------------------------------------
	// Left hand: hand 1 of the ps's per-hand block (stride 0x1C), and the viewmodel info's left channel.
	constexpr size_t kInsPsLeftWeaponState = 0x78, kInsPsLeftWeapAnim = 0x80;
	constexpr size_t kInsVmLeftNode = 0x370, kInsVmLeftLastAnim = 0x374;
	constexpr int kInsLeftIdleNode = 168, kInsLeftEmptyIdleNode = 170;  // DD140(..., 1, 170, 168) at 0x1404EA2A0
	uintptr_t &kInsIsDualWield = kIsDualWield;    // BG_IsDualWield(weapon) -> bool (bo3_locomotion.h)
	uintptr_t kInsDualWieldWeapon = 0x27D18E0;     // BG_GetDualWieldWeapon(weapon) -> the left weapon
	uintptr_t &kInsClipWeapon = kClipSlotWeapon;   // the weapon whose held slot holds the clip
	// Where InspectCheckAkimbo reads the engine's own use of the constants (see there; retail: bo3_retail.h).
	uintptr_t kInsAkLastAnim = 0x4EA0EC, kInsAkCmpAnim = 0x4EA0F3, kInsAkSetNode = 0x4EA14D, kInsAkIdle = 0x4EA2A0,
	          kInsAkLeftState = 0x27B0C76, kInsAkIsDualCall = 0x4EA103, kInsAkDualCall = 0x4DD175, kInsAkClipCall = 0x4DD182;
	uintptr_t kInsAkClipAmmo = 0;  // retail: the hand-clip function's clip-ammo helper (it calls the clip weapon fn first)
	constexpr float kInsLeftFadeOut = 0.2f, kInsLeftFadeIn = 0.25f;  // s
	bool g_insAkimboCfg = true;  // inspect_akimbo
	bool g_insAkimbo;            // cfg on and the checks below passed at install
	using InsWeaponFn = uint64_t (*)(uint64_t);
	using InsIsDualFn = bool (*)(uint64_t);

	// The engine code the akimbo constants come from (IDA 2026-10-02). Any mismatch: akimbo is unsupported.
	bool InspectCheckAkimbo(const char *&what)
	{
		auto calls = [](uintptr_t rva, uintptr_t target) {
			const uint8_t *p = At<uint8_t>(rva);
			int32_t rel;
			if (!FastReadable(p, 5) || p[0] != 0xE8)
				return false;
			memcpy(&rel, p + 1, 4);
			return static_cast<uintptr_t>(p + 5 + rel - g_base) == target;
		};
		// CG_UpdateViewWeaponAnim's left block: movzx eax, word [rax+374h] | cmp [rcx+80h], ax | call BG_IsDualWield |
		// mov [rdi+370h], eax (the mapped left node) | the left idle's 0A8h / 0AAh (168 / 170)
		static const uint8_t lastAnim[] = {0x0F, 0xB7, 0x80, 0x74, 0x03, 0x00, 0x00};
		static const uint8_t cmpAnim[] = {0x66, 0x39, 0x81, 0x80, 0x00, 0x00, 0x00};
		static const uint8_t setNode[] = {0x89, 0x87, 0x70, 0x03, 0x00, 0x00};
		static const uint8_t idle[] = {0xC7, 0x44, 0x24, 0x48, 0xA8, 0x00, 0x00, 0x00, 0xC7, 0x44, 0x24, 0x40, 0xAA, 0x00, 0x00, 0x00};
		// Retail: the same reads with other registers (cmp [rdi+80h],ax; mov [rcx+370h],eax), and the left idle function
		// stores the node itself: mov dword [rsi+370h], 0AAh (empty) ... mov dword [rsi+370h], 0A8h, 18 bytes apart.
		static const uint8_t cmpAnimR[] = {0x66, 0x39, 0x87, 0x80, 0x00, 0x00, 0x00};
		static const uint8_t setNodeR[] = {0x89, 0x81, 0x70, 0x03, 0x00, 0x00};
		static const uint8_t idleR[] = {0xC7, 0x86, 0x70, 0x03, 0x00, 0x00, 0xAA, 0x00, 0x00, 0x00};
		static const uint8_t idleR2[] = {0xC7, 0x86, 0x70, 0x03, 0x00, 0x00, 0xA8, 0x00, 0x00, 0x00};
		// PM: cmp dword [rbx+78h], 0Ch (the left hand's weaponState) in 0x1427B0C20
		static const uint8_t leftState[] = {0x83, 0x7B, 0x78, 0x0C};
		const bool r = IsRetailExe();
		if (!CodeMatches(kInsAkLastAnim, lastAnim, sizeof(lastAnim)) ||
		    !CodeMatches(kInsAkCmpAnim, r ? cmpAnimR : cmpAnim, sizeof(cmpAnim)))
			return what = "left weapAnim / last-anim offsets", false;
		if (!CodeMatches(kInsAkSetNode, r ? setNodeR : setNode, sizeof(setNode)) ||
		    !(r ? CodeMatches(kInsAkIdle, idleR, sizeof(idleR)) && CodeMatches(kInsAkIdle + 0x12, idleR2, sizeof(idleR2))
		        : CodeMatches(kInsAkIdle, idle, sizeof(idle))))
			return what = "left node offset / idle nodes", false;
		if (!CodeMatches(kInsAkLeftState, leftState, sizeof(leftState)))
			return what = "left weaponState offset", false;
		// Retail's hand-clip function calls a clip-ammo helper, which calls the clip weapon function first.
		const bool clipCall = r ? calls(kInsAkClipCall, kInsAkClipAmmo) && calls(kInsAkClipAmmo + 0xC, kInsClipWeapon)
		                        : calls(kInsAkClipCall, kInsClipWeapon);
		if (!calls(kInsAkIsDualCall, kInsIsDualWield) || !calls(kInsAkDualCall, kInsDualWieldWeapon) || !clipCall)
			return what = "BG_IsDualWield / BG_GetDualWieldWeapon / clip weapon calls", false;
		return true;
	}

	// A hand's clip the way the engine finds it for the idle / empty idle choice (0x1404DD140): the clip weapon's held
	// slot, compared on the whole value. -1: not held.
	int InspectHandClip(const uint8_t *ps, uint64_t weapon, int hand)
	{
		if (hand == 1)
			weapon = reinterpret_cast<InsWeaponFn>(g_base + kInsDualWieldWeapon)(weapon);
		const uint64_t clipWeapon = reinterpret_cast<InsWeaponFn>(g_base + kInsClipWeapon)(weapon);
		for (int i = 0; i < 15; i++)
			if (*reinterpret_cast<const uint64_t *>(ps + kPsHeldWeapons + i * kPsHeldStride) == clipWeapon)
				return *reinterpret_cast<const int32_t *>(ps + kPsAmmoInClip + i * 4);
		return -1;
	}

	struct InspectEntry
	{
		char weapon[64];
		bool on;
		float seconds;  // 0: GDT / xanim
	};
	struct InspectAlias
	{
		char weapon[64], source[64];
	};
	InspectEntry g_insEntries[256];
	int g_insCount;
	InspectAlias g_insAliases[128];
	int g_insAliasCount;
	bool g_insEnable, g_insHideHud, g_insHooked;
	int g_insKey = 'I';

	int ParseVkName(const char *s)
	{
		if (!s[0])
			return 0;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			return static_cast<int>(strtol(s, nullptr, 16));
		if (!s[1] && isalnum(static_cast<unsigned char>(s[0])))
			return toupper(static_cast<unsigned char>(s[0]));
		if ((s[0] == 'F' || s[0] == 'f') && isdigit(static_cast<unsigned char>(s[1])))
		{
			int n = atoi(s + 1);
			return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
		}
		if (!_stricmp(s, "MOUSE3"))
			return VK_MBUTTON;
		if (!_stricmp(s, "MOUSE4"))
			return VK_XBUTTON1;
		if (!_stricmp(s, "MOUSE5"))
			return VK_XBUTTON2;
		return 0;
	}

	// Called with every cfg line before ParseWeaponLine: consumes inspect* (known = true); notes wop_alias= (known = false).
	bool ParseInspectLine(const char *line, bool &known)
	{
		known = false;
		if (strncmp(line, "wop_alias=", 10) == 0)
		{
			char weapon[64] = {}, source[64] = {};
			if (g_insAliasCount < 128 &&
			    sscanf_s(line + 10, "%63[^,],%63[^, \t#]", weapon, static_cast<unsigned>(sizeof(weapon)), source,
			             static_cast<unsigned>(sizeof(source))) == 2)
			{
				strcpy_s(g_insAliases[g_insAliasCount].weapon, weapon);
				strcpy_s(g_insAliases[g_insAliasCount++].source, source);
			}
			return true;
		}
		int v;
		if (strncmp(line, "inspect_enable=", 15) == 0)
			return known = true, sscanf_s(line + 15, "%d", &v) == 1 && ((g_insEnable = v != 0), true);
		if (strncmp(line, "inspect_debug_empty=", 20) == 0)
			return known = true, sscanf_s(line + 20, "%d", &v) == 1 && ((g_insDebugEmpty = v != 0), true);
		if (strncmp(line, "inspect_empty=", 14) == 0)
		{
			known = true;
			const char *v2 = line + 14;
			g_insEmpty = strncmp(v2, "off", 3) == 0 || strncmp(v2, "0", 1) == 0 ? 0 : strncmp(v2, "out", 3) == 0 ? 2 : 1;
			return true;
		}
		if (strncmp(line, "inspect_akimbo=", 15) == 0)
			return known = true, sscanf_s(line + 15, "%d", &v) == 1 && ((g_insAkimboCfg = v != 0), true);
		if (strncmp(line, "inspect_hidehud=", 16) == 0)
			return known = true, sscanf_s(line + 16, "%d", &v) == 1 && ((g_insHideHud = v != 0), true);
		if (strncmp(line, "inspect_key=", 12) == 0)
		{
			known = true;
			char name[16] = {};
			if (sscanf_s(line + 12, "%15[^ \t#]", name, static_cast<unsigned>(sizeof(name))) != 1)
				return false;
			int vk = ParseVkName(name);
			return vk > 0 && vk < 256 && ((g_insKey = vk), true);
		}
		if (strncmp(line, "inspect=", 8) != 0)
			return true;
		known = true;
		char weapon[64] = {};
		int on = 0;
		float seconds = 0;
		int n = sscanf_s(line + 8, "%63[^,],%d,%f", weapon, static_cast<unsigned>(sizeof(weapon)), &on, &seconds);
		if (n < 2 || seconds < 0)
			return false;
		InspectEntry *e = nullptr;
		for (int i = 0; i < g_insCount && !e; i++)
			if (strcmp(g_insEntries[i].weapon, weapon) == 0)
				e = &g_insEntries[i];
		if (!e)
		{
			if (g_insCount >= 256)
				return false;
			e = &g_insEntries[g_insCount++];
			strcpy_s(e->weapon, weapon);
		}
		e->on = on != 0;
		e->seconds = n >= 3 ? seconds : 0.0f;
		return true;
	}

	const InspectEntry *InspectEntryFor(const char *name)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			for (int i = 0; i < g_insCount; i++)
				if (strcmp(g_insEntries[i].weapon, name) == 0)
					return &g_insEntries[i];
			const char *source = nullptr;
			for (int i = 0; i < g_insAliasCount && !source; i++)
				if (strcmp(g_insAliases[i].weapon, name) == 0)
					source = g_insAliases[i].source;
			if (!source)
				return nullptr;
			name = source;
		}
		return nullptr;
	}

	// Per variant: does it inspect, and the slot name (checked once per variant pointer; main thread only).
	struct InspectVariant
	{
		const uint8_t *variant;
		const InspectEntry *entry;  // nullptr: not configured / off / no lowReadyLoopAnim
	};
	InspectVariant g_insVariants[512];

	const InspectEntry *InspectFor(uint64_t weapon)
	{
		if (!(weapon & 0x1FF))
			return nullptr;
		const uint8_t *variant = At<uint8_t *>(kInsVariants)[weapon & 0x1FF];
		InspectVariant &c = g_insVariants[weapon & 0x1FF];
		if (c.variant == variant)
			return c.entry;
		c.variant = variant;
		c.entry = nullptr;
		const char *name = variant ? *reinterpret_cast<const char *const *>(variant) : nullptr;
		const InspectEntry *e = name ? InspectEntryFor(name) : nullptr;
		if (!e || !e->on)
			return nullptr;
		const char *const *anims = *reinterpret_cast<const char *const *const *>(variant + kInsVarAnims);
		const char *slot = anims ? anims[kInsNode] : nullptr;
		const uint8_t *def = *reinterpret_cast<uint8_t *const *>(variant + kInsVarDef);
		const char *idle = anims ? anims[1] : nullptr;
		bool ok = slot && slot[0] && !(idle && strcmp(slot, idle) == 0);
		Log("inspect: %s (variant %d): lowReadyLoopAnim '%s', lowReadyLoopTime %d ms, cfg %.2f s%s", name,
		    static_cast<int>(weapon & 0x1FF), slot ? slot : "", def ? *reinterpret_cast<const int32_t *>(def + kInsDefLowReadyLoopTime) : -1,
		    e->seconds, ok ? "" : " -- NOT USED: the slot is blank or the idle anim");
		c.entry = ok ? e : nullptr;
		return c.entry;
	}

	struct InspectState
	{
		bool active;
		uint64_t weapon;
		uint16_t engineAnim;  // the engine's own ps value when it started
		uint16_t ourAnim;     // what we write
		int startTime, duration;  // cg ms
		int node = kInsNode;      // the viewmodel node playing: 79 loop, or 78 / 80 for an empty inspect
		bool keyDown;
		uint32_t lastCatchers = 0xFFFFFFFF;
		// Akimbo: the left hand's engine weapAnim at the start and the left node faded out (0: single wield).
		bool dual;
		uint16_t leftAnim;
		int leftNode;
		uint64_t trace;  // perf_hotlog: the last logged (R anim, L anim, L state, R node, L node), to log changes only
	} g_ins[4];

	bool InspectActive(int lc = 0) { return lc >= 0 && lc < 4 && g_ins[lc].active; }

	bool InspectKeyEdge(InspectState &s, int lc)
	{
		const bool down = (GetAsyncKeyState(g_insKey) & 0x8000) != 0;
		const bool edge = down && !s.keyDown;
		s.keyDown = down;
		if (!edge)
			return false;
		DWORD pid = 0;
		HWND fg = GetForegroundWindow();
		if (!fg || (GetWindowThreadProcessId(fg, &pid), pid != GetCurrentProcessId()))
			return false;
		// Console, chat and LUI menus set key catchers; gameplay has none.
		const uint32_t catchers = *At<uint32_t>(kInsKeyCatchers + kInsKeyCatchersStride * static_cast<uint32_t>(lc));
		if (catchers != s.lastCatchers)
		{
			s.lastCatchers = catchers;
			HotLog("inspect: key catchers now 0x%x", catchers);
		}
		return catchers == 0;
	}

	// Why an inspect can't start / must stop now ("" = fine). `running` relaxes nothing; it only picks the wording.
	const char *InspectBlocked(const uint8_t *ps, const uint8_t *vm, uint64_t weapon)
	{
		const int state = *reinterpret_cast<const int32_t *>(ps + kInsPsWeaponState);
		const uint64_t pmf = *reinterpret_cast<const uint64_t *>(ps + kInsPsPmFlags);
		if (*reinterpret_cast<const int32_t *>(ps + kInsPsPmType) != 0)
			return "dead / last stand / linked";
		if (!(InspectStateRule(state) & kInsStart))
			return "weapon busy";
		if (*reinterpret_cast<const float *>(ps + kInsPsAds) > 0.001f)
			return "ADS";
		if (pmf & kInsPmfProne)
			return "prone";
		if (pmf & kInsPmfJuke)
			return "juking";
		if (*reinterpret_cast<const uint64_t *>(ps + kInsPsWeapon) != weapon)
			return "weapon changed";
		(void)vm;
		return "";
	}

	// Akimbo: why the left hand can't start / must stop ("" = fine). Starting needs it READY on its idle / empty idle,
	// with the engine caught up on its anim; while running, anything the engine does to it stops the inspect.
	const char *InspectLeftBlocked(const uint8_t *ps, const uint8_t *vm, const InspectState *running)
	{
		const int state = *reinterpret_cast<const int32_t *>(ps + kInsPsLeftWeaponState);
		const uint16_t anim = *reinterpret_cast<const uint16_t *>(ps + kInsPsLeftWeapAnim);
		const int node = *reinterpret_cast<const int32_t *>(vm + kInsVmLeftNode);
		if (!(InspectStateRule(state) & kInsStart))
			return "left gun busy";
		if (running)
		{
			if (anim != running->leftAnim)
				return "the engine played a left-hand anim";
			if (node != running->leftNode)
				return "the left hand's node changed";
			return "";
		}
		if ((anim & 0x1FFF) > 1 || anim != *reinterpret_cast<const uint16_t *>(vm + kInsVmLeftLastAnim) ||
		    (node != kInsLeftIdleNode && node != kInsLeftEmptyIdleNode))
			return "left hand not idle";
		return "";
	}

	void InspectSetLeftWeight(uint8_t *vm, int node, float weight, float blend)
	{
		if (void *dobj = *reinterpret_cast<void **>(vm + kInsVmDObj))
			reinterpret_cast<SetGoalWeightFn>(g_base + kSetGoalWeight)(dobj, static_cast<uint32_t>(node), weight, blend, 1.0f, 0, 0, 0);
	}

	// perf_hotlog: both hands' ps / viewmodel values, when they change during an akimbo inspect.
	void InspectTraceAkimbo(InspectState &s, const uint8_t *ps, const uint8_t *vm, const char *when)
	{
		if (!g_hotLog)
			return;
		const uint16_t ra = *reinterpret_cast<const uint16_t *>(ps + kInsPsWeapAnim);
		const uint16_t la = *reinterpret_cast<const uint16_t *>(ps + kInsPsLeftWeapAnim);
		const int rs = *reinterpret_cast<const int32_t *>(ps + kInsPsWeaponState);
		const int ls = *reinterpret_cast<const int32_t *>(ps + kInsPsLeftWeaponState);
		const int rn = *reinterpret_cast<const int32_t *>(vm + kInsVmRightNode);
		const int ln = *reinterpret_cast<const int32_t *>(vm + kInsVmLeftNode);
		const uint64_t key = (uint64_t(ra) << 48) ^ (uint64_t(la) << 32) ^ (uint64_t(rs & 0xFF) << 24) ^
		                     (uint64_t(ls & 0xFF) << 16) ^ (uint64_t(rn & 0xFF) << 8) ^ uint64_t(ln & 0xFF);
		if (key == s.trace && strcmp(when, "frame") == 0)
			return;
		s.trace = key;
		Log("inspect: akimbo %s: R state %d anim 0x%x node %d last 0x%x | L state %d anim 0x%x node %d last 0x%x", when, rs,
		    ra, rn, *reinterpret_cast<const uint16_t *>(vm + kInsVmLastAnim), ls, la, ln,
		    *reinterpret_cast<const uint16_t *>(vm + kInsVmLeftLastAnim));
	}

	void InspectFinishNode(uint8_t *vm, int node)
	{
		// The engine's idle path waits while the current node hasn't finished: mark it finished so idle blends in now.
		if (void *dobj = *reinterpret_cast<void **>(vm + kInsVmDObj))
			if (*reinterpret_cast<int32_t *>(vm + kInsVmRightNode) == node)
				SetNodeTime(dobj, node, 1.0f);
	}

	void InspectStop(InspectState &s, uint8_t *ps, uint8_t *vm, const char *why)
	{
		const uint16_t now = *reinterpret_cast<uint16_t *>(ps + kInsPsWeapAnim);
		const uint16_t low = now & 0x1FFF;
		if (now == s.ourAnim || low <= 1)  // the engine is (still) on idle: finish the loop so its blend to idle starts
		{
			if (now == s.ourAnim)
				*reinterpret_cast<uint16_t *>(ps + kInsPsWeapAnim) = s.engineAnim;
			InspectFinishNode(vm, s.node);
		}
		if (s.dual)
		{
			// Fade the left idle back in, unless the engine already moved the left hand (its new node has weight) or the
			// weapon changed (the engine sets the new weapon's nodes up itself).
			const int leftNode = *reinterpret_cast<int32_t *>(vm + kInsVmLeftNode);
			const bool restore = leftNode == s.leftNode && *reinterpret_cast<uint64_t *>(ps + kInsPsWeapon) == s.weapon;
			if (restore)
				InspectSetLeftWeight(vm, s.leftNode, 1.0f, kInsLeftFadeIn);
			InspectTraceAkimbo(s, ps, vm, "stop");
			HotLog("inspect: akimbo: left node %d %s", s.leftNode,
			       restore ? "faded back in" : "left to the engine (it moved the left hand / changed weapon)");
			s.dual = false;
		}
		s.active = false;
		InspectHudSync(static_cast<int>(&s - g_ins), false, g_insHideHud);
		HotLog("inspect: stop (%s) after %d ms", why,
		       *reinterpret_cast<int32_t *>(ps - kInsCgPs + kInsCgTime) - s.startTime);
	}

	// Every frame, right before CG_UpdateViewWeaponAnim, on the main thread.
	void InspectBeforeAnim(uint8_t *ps, uint8_t *vm, int lc)
	{
		if (lc < 0 || lc >= 4 || !ps || !vm)
			return;
		InspectState &s = g_ins[lc];
		InspectHudSync(lc, s.active, g_insHideHud);  // writes only on a change (or a new root model)
		const int now = *reinterpret_cast<int32_t *>(ps - kInsCgPs + kInsCgTime);
		uint16_t &anim = *reinterpret_cast<uint16_t *>(ps + kInsPsWeapAnim);
		const bool pressed = InspectKeyEdge(s, lc);

		if (s.active)
		{
			const char *why = anim != s.engineAnim && anim != s.ourAnim ? "the engine played an anim" : InspectBlocked(ps, vm, s.weapon);
			if (s.dual)
			{
				InspectTraceAkimbo(s, ps, vm, "frame");
				if (!*why)
					why = InspectLeftBlocked(ps, vm, &s);
			}
			if (!*why && now - s.startTime >= s.duration)
				why = "done";
			if (*why)
			{
				InspectStop(s, ps, vm, why);
				return;
			}
			anim = s.ourAnim;  // prediction rebuilt the ps from the snapshot: again
			return;
		}
		if (!pressed)
			return;
		const uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kInsPsWeapon);
		const InspectEntry *e = InspectFor(weapon);
		const char *why = !e ? "not an inspect weapon" : InspectBlocked(ps, vm, weapon);
		const int node = *reinterpret_cast<int32_t *>(vm + kInsVmRightNode);
		if (!*why && ((anim & 0x1FFF) > 1 || (node != 1 && node != 2)))
			why = "not idle (sprint, slide, mantle, a transition)";
		if (!*why && *reinterpret_cast<const int32_t *>(ps + kInsPsCursorHint) != 0)
			why = "a use prompt is showing";
		// Akimbo: both hands must be free, and the akimbo path must be on and verified.
		const bool dual = !*why && reinterpret_cast<InsIsDualFn>(g_base + kInsIsDualWield)(weapon);
		if (dual)
			why = !g_insAkimbo ? "akimbo weapon, akimbo unsupported (inspect_akimbo=0 or its checks failed at install)"
			                   : InspectLeftBlocked(ps, vm, nullptr);
		if (*why)
		{
			HotLog("inspect: not started: %s", why);
			return;
		}
		const uint8_t *variant = At<uint8_t *>(kInsVariants)[weapon & 0x1FF];
		const uint8_t *def = *reinterpret_cast<uint8_t *const *>(variant + kInsVarDef);
		if (!def)
		{
			HotLog("inspect: not started: no WeaponDef");
			return;
		}
		// Empty inspect (MWII+): an empty clip plays the in / out slot instead, if the GDT gave it an anim of its own.
		int insNode = kInsNode, insAnim = kInsPsAnim;
		size_t timeOfs = kInsDefLowReadyLoopTime;
		// Akimbo: either gun's clip empty counts (the right channel's slot plays for both hands).
		const int clipR = dual ? InspectHandClip(ps, weapon, 0) : 0, clipL = dual ? InspectHandClip(ps, weapon, 1) : 0;
		if (dual)
			HotLog("inspect: akimbo: clips R %d L %d (left weapon %llx)", clipR, clipL,
			       reinterpret_cast<InsWeaponFn>(g_base + kInsDualWieldWeapon)(weapon));
		if (g_insEmpty && (g_insDebugEmpty || (dual ? clipR == 0 || clipL == 0 : ClipAmmo(ps, weapon) == 0)))
		{
			const int n = g_insEmpty == 2 ? 80 : 78;
			const char *const *anims = *reinterpret_cast<const char *const *const *>(variant + kInsVarAnims);
			const char *slot = anims ? anims[n] : nullptr, *idle = anims ? anims[1] : nullptr, *loop = anims ? anims[kInsNode] : nullptr;
			if (slot && slot[0] && !(idle && strcmp(slot, idle) == 0) && !(loop && strcmp(slot, loop) == 0))
			{
				insNode = n;
				insAnim = n - 11;  // 67 / 69
				timeOfs = g_insEmpty == 2 ? kInsDefLowReadyOutTime : kInsDefLowReadyInTime;
				HotLog("inspect: empty clip: %s '%s'", g_insEmpty == 2 ? "lowReadyOutAnim" : "lowReadyInAnim", slot);
			}
		}
		// Length: the cfg's seconds (normal inspect only), else the slot's GDT time (the engine fits the anim's rate to
		// it), else the anim's own.
		int duration = e->seconds > 0 && insNode == kInsNode ? static_cast<int>(e->seconds * 1000.0f)
		                                                     : *reinterpret_cast<const int32_t *>(def + timeOfs);
		if (duration <= 0)
		{
			float f = NodeFrequency(*reinterpret_cast<void **>(vm + kInsVmDObj), insNode);
			duration = f > 0 ? static_cast<int>(1000.0f / f) : 0;
		}
		if (duration <= 0)
		{
			HotLog("inspect: not started: no length (no lowReadyLoopTime, no cfg seconds, anim not loaded)");
			return;
		}
		s.active = true;
		s.weapon = weapon;
		s.engineAnim = anim;
		const uint16_t last = *reinterpret_cast<uint16_t *>(vm + kInsVmLastAnim);
		s.ourAnim = static_cast<uint16_t>((anim & 0xC000) | (~last & 0x2000) | insAnim);  // toggle differs: a fresh start
		s.node = insNode;
		s.startTime = now;
		s.duration = duration;
		anim = s.ourAnim;
		s.dual = dual;
		if (dual)
		{
			// The right channel's inspect animates both arms; the left idle would otherwise keep weight 1 beside it.
			s.leftAnim = *reinterpret_cast<uint16_t *>(ps + kInsPsLeftWeapAnim);
			s.leftNode = *reinterpret_cast<int32_t *>(vm + kInsVmLeftNode);
			InspectSetLeftWeight(vm, s.leftNode, 0.0f, kInsLeftFadeOut);
			s.trace = 0;
			InspectTraceAkimbo(s, ps, vm, "start");
		}
		InspectHudSync(lc, true, g_insHideHud);
		HotLog("inspect: start%s, %d ms (engine anim 0x%x, ours 0x%x, node %d%s)", dual ? " (akimbo)" : "", duration,
		       s.engineAnim, s.ourAnim, insNode, dual ? "; left idle faded out" : "");
	}

	// Patches the 17 bytes before the call to CG_UpdateViewWeaponAnim (not the call: that function spins when anything
	// sits between it and its caller). The stub calls InspectBeforeAnim(ps, vm, lc), runs the displaced code and jumps on.
	void InstallInspect()
	{
		if (WtDebugSkip("inspect"))
			return;
		InspectHudNoteLuaThread();  // init() is called from Lua: the thread the HUD's models are read on
		if (g_insHooked)
			return;
		if (!g_insEnable || !g_insCount)
		{
			if (g_insCount)
				Log("inspect: off (inspect_enable / [features] inspect); not installed (%d weapon line(s))", g_insCount);
			return;
		}
		if (!WtExeSupported())
		{
			Log("inspect: unknown exe; not installed");
			return;
		}
		// The key-catcher constant must be the one `xor [rip+..], 1` at 0x1413DF34B toggles (IDA 2026-10-01).
		if (!CodeRefers(kInsKeyCatchersRef, 7, kInsKeyCatchers))
		{
			Log("inspect: the key-catcher constant +%zx doesn't match the code; not installed", kInsKeyCatchers);
			return;
		}
		uint8_t *site = At<uint8_t>(kInsSite);
		uint8_t *call = At<uint8_t>(kInsCallUpdate);
		const uint8_t *kInsDisplaced = IsRetailExe() ? kInsDisplacedRetail : kInsDisplacedEnh;
		const size_t kInsDisplacedLen = IsRetailExe() ? sizeof(kInsDisplacedRetail) : sizeof(kInsDisplacedEnh);
		int32_t rel;
		memcpy(&rel, call + 1, 4);
		if (site + kInsDisplacedLen != call || memcmp(site, kInsDisplaced, kInsDisplacedLen) != 0 || call[0] != 0xE8 ||
		    call + 5 + rel != At<uint8_t>(kInsUpdateViewWeaponAnim))
		{
			Log("inspect: the code before the CG_UpdateViewWeaponAnim call doesn't match; not installed");
			return;
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!stub)
		{
			Log("inspect: no memory near the exe; not installed");
			return;
		}
		uint8_t *p = stub;
		auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
		emit({0x48, 0x83, 0xEC, 0x20});  // sub rsp, 20h (rsp stays 16-aligned at the call)
		if (IsRetailExe())
		{
			emit({0x4C, 0x89, 0xF1});    // mov rcx, r14   ps
			emit({0x48, 0x89, 0xFA});    // mov rdx, rdi   vm
			emit({0x45, 0x89, 0xF8});    // mov r8d, r15d  lc
		}
		else
		{
			emit({0x48, 0x89, 0xF1});    // mov rcx, rsi   ps
			emit({0x4C, 0x89, 0xF2});    // mov rdx, r14   vm
			emit({0x41, 0x89, 0xF8});    // mov r8d, edi   lc
		}
		emit({0x48, 0xB8});              // mov rax, InspectBeforeAnim
		void *fn = reinterpret_cast<void *>(&InspectBeforeAnim);
		memcpy(p, &fn, 8);
		p += 8;
		emit({0xFF, 0xD0});              // call rax
		emit({0x48, 0x83, 0xC4, 0x20});  // add rsp, 20h
		memcpy(p, kInsDisplaced, kInsDisplacedLen);
		p += kInsDisplacedLen;
		emit({0xFF, 0x25, 0, 0, 0, 0});  // jmp [rip] -> the call
		memcpy(p, &call, 8);

		uint8_t jump[sizeof(kInsDisplacedRetail)];
		memset(jump, 0x90, sizeof(jump));
		jump[0] = 0xFF;
		jump[1] = 0x25;
		memset(jump + 2, 0, 4);
		memcpy(jump + 6, &stub, 8);
		DWORD old;
		VirtualProtect(site, kInsDisplacedLen, PAGE_EXECUTE_READWRITE, &old);
		memcpy(site, jump, kInsDisplacedLen);
		VirtualProtect(site, kInsDisplacedLen, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, kInsDisplacedLen);
		g_insHooked = true;
		const char *akimboWhy = "inspect_akimbo=0";
		g_insAkimbo = g_insAkimboCfg && InspectCheckAkimbo(akimboWhy);
		if (!g_insAkimbo)
			Log("inspect: akimbo unsupported (%s); dual-wield weapons won't inspect", akimboWhy);
		int on = 0;
		for (int i = 0; i < g_insCount; i++)
			on += g_insEntries[i].on;
		Log("inspect: hooked before CG_UpdateViewWeaponAnim; %d of %d weapon line(s) on, key 0x%02X, %d alias(es)%s%s", on,
		    g_insCount, g_insKey, g_insAliasCount, g_insHideHud ? "; inspect_hidehud=1 (HUD fades out)" : "",
		    g_insAkimbo ? "; akimbo on" : "");
	}
}
