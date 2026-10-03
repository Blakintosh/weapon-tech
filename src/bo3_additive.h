// Weapon additive layers driven at runtime (proof of concept, BO3 both supported exes for now).
// Recon: the author's research notes (addresses there are absolute; RVAs here).
//
// Every viewmodel XAnimTree has two additive blend groups that no game code touches:
// 193 -> 194 and 195 -> 196 (made for swimming, dead in ZM). Weapon anim slots are stored as NAMES on
// the weapon variant (variant+0x48, 197 entries) and the tree is built from them on weapon change, so
// writing our xanim's name into szXAnims[194]/[196] makes the engine load it into that node.
// CG_UpdateViewWeaponAnim is then detoured: the original picks the weapon anim and resets nodes
// 1..172, then we set the additive group weights (the tree advances right after it returns).
//
// Config (weapon_tech.cfg, read by weapon_tech.dll: weapon_tech.h; stub_boot.dll no longer includes this file):
//   additive=<weapon>,<empty|recoil|bullet>,<root node 193|195>,<xanim>[,<weight>[,<mag size, bullet only>]]
//   additive=<weapon>,<kind>,slot:<purpose>,<xanim>[,<weight>[,<mag|rate>]][,side:right]   a purpose slot (bo3_slots.h:
//       bullets / empty / recoil_ads on extra additive roots; slot:idle / slot:recoil = the old 193 / 195)
//   empty_melee_fix=1   gun keeps its own melee and normal/quick raise+drop when empty
//   locomotion=, locomotion_jog=, locomotion_alias=, idle_active=, idle_active_fade=, locomotion_debug=
//       MW19 walk / jog / idle active (bo3_locomotion.h, included at the end of this file)
//   sway_*=  MW19 advanced hip sway, advanced idle, stance pivots, ADS gun bob (bo3_sway.h, included at the end)
//   perf_eventhooks=1   find weapon variants when the game registers them and patch slot names right before the viewmodel
//                       tree is built (bo3_perf.h); 0 = the table poll every 120 frames only (both are cheap now)
//   perf_pollms=2000    with the event hooks in, the table is also polled this often as a fallback (live)
//   perf_cfgwatch=1     the live-tuning check of this file runs on a background thread (4 times a second); 0 = a stat
//                       on the game thread with each table poll, as before
//   perf_hotlog=0       per-shot / per-state debug lines in the log (additive layers, kick, ammohide); 1 = on (live;
//                       a removed line keeps the last value until a restart)
//   perf_timing=0       1 = the game thread's time per hook / feature and the frame time, a report every 10 s (live)
//   additive_enable=1, locomotion_enable=1, idle_active_enable=1 (and sway_enable=1, bo3_sway.h)
//                       A/B switches (live; a removed line counts as 1): 0 fades that feature out and leaves the tree alone,
//                       so the engine-side cost of the extra anim layers shows in perf_timing's frame time
#pragma once
#include "bo3_zone.h"
#include "bo3_build.h"
#include "bo3_wop.h"
#include "bo3_features.h"
#include <atomic>

namespace
{
	// ---- Engine addresses (RVAs): Enhanced values; bo3_retail.h overwrites them on the retail exe ------------------
	uintptr_t kUpdateViewWeaponAnim = 0x4E6AF0;  // CG_UpdateViewWeaponAnim(lc, ps, vm, force)
	uintptr_t kAfterUpdateCall = 0x4F3860;       // right after its only call, in CG_UpdateViewModelAnims
	uintptr_t kSetGoalWeight = 0x2404120;        // XAnimSetGoalWeight
	uintptr_t kGetInfoIndex = 0x2400190;         // XAnimGetInfoIndex(XAnimTree*, node)
	uintptr_t kXAnimInfo = 0x19B81920;           // XAnimInfo[], 0x58 each
	uintptr_t kSyncTime = 0x2402280;             // XAnimSyncTime(infoIndex, time)
	uintptr_t kWeaponVariants = 0x9AD2360;       // WeaponVariantDef* [512]
	uintptr_t kVariantCount = 0x9AD2350;         // int: variants registered this session (Enhanced: table-0x10; retail: table+0x1004)
	uintptr_t kSLTable = 0x3B1F308;              // u8*: scr string table, 28-byte entries, text at +4
	uintptr_t kClipSlotWeapon = 0x27C6590;       // weapon -> the value heldWeapons[] stores
	uintptr_t kClipEmpty = 0x27C81B0;            // BG_ClipEmpty
	uintptr_t kMeleeOwnGun = 0x27CE2BE;          // jz -> jmp: always the gun's own melee
	uint8_t kMeleeCtx[] = {0x83, 0xB9, 0xCC, 0x15, 0x00, 0x00, 0x00};  // cmp dword [rcx+15CCh], 0 (the jz's flag); retail [rax+..]
	uint8_t kMeleeJzRel = 0x1C;                  // the jz's rel8 (retail 0xC5)
	uintptr_t kEmptyRaiseCall = 0x27B2359;       // call BG_ClipEmpty -> xor eax,eax
	uintptr_t kEmptyDropCall = 0x27B18AF;
	uintptr_t kDObjGetBoneIndex = 0x23FD180;     // (DObj*, u32 name, s16 *inoutIndex, u32 modelMask)
	uintptr_t kSLGetString = 0x137DEF0;          // SL_GetString(const char*, u32 user); retail (.., type): SlString()
	// Code that must refer to the data above (InstallAdditives): lea XAnimInfo; mov rax,[SL table]; mov edx,[variant count]
	uintptr_t kXAnimInfoRef = 0x12480C, kSLTableRef = 0x11FA2E, kVariantCountRef = 0x27CB70A;

	// XAnimGetInfoIndex(tree, node): 0 when the tree has no info for the node (retail's is a 6-instruction wrapper over
	// the (node, root info index) worker, the root being the u32 at tree+8).
	uint32_t XGetInfo(void *tree, uint32_t node)
	{
		return reinterpret_cast<uint32_t (*)(void *, uint32_t)>(g_base + kGetInfoIndex)(tree, node);
	}
	// SL_GetString(text, user). Retail's takes a third argument (the string type) and uses it when it allocates; 3 is
	// what most of its engine callers pass. Enhanced's ignores the extra register.
	uint32_t SlString(const char *text, uint32_t user)
	{
		return reinterpret_cast<uint32_t (*)(const char *, uint32_t, uint32_t)>(g_base + kSLGetString)(text, user, 3);
	}

	// playerState_t
	constexpr size_t kPsWeaponState = 0x5C, kPsShotCount = 0x60, kPsWeapon = 0x2C0, kPsAdsFraction = 0x2FC;
	constexpr size_t kPsHeldWeapons = 0x378, kPsHeldStride = 0x30, kPsAmmoInClip = 0x684;
	// viewmodel info
	constexpr size_t kVmDObj = 0x0, kVmRightAnim = 0x368;
	constexpr size_t kVmHideBits = 0x338;       // u32[12], the engine's viewmodel hide bits (hideTags, tag_clip)
	// DObj
	constexpr size_t kDObjHideBits = 0x20;      // u32[12], MSB-first per bone; copied from vm+0x338 by the engine
	constexpr size_t kDObjModels = 0x18, kDObjNumBones = 0x120;
	constexpr size_t kVariantAnims = 0x48;  // -> const char *szXAnims[197]
	constexpr int kNumWeapAnims = 197;

	using SetGoalWeightFn = void (*)(void *dobj, uint32_t node, float weight, float blendTime, float rate,
	                                 uint32_t notifyName, uint32_t notifyType, int restart);
	using WeaponFn = uint64_t (*)(uint64_t weapon);

	enum class AdditiveKind { Empty, Recoil, Bullet };
	struct AdditiveConfig
	{
		char weapon[64];
		AdditiveKind kind;
		uint32_t root;       // 193 or 195; the anim goes in root + 1
		char xanim[96];
		float weight;
		int magSize = 0;       // bullet: rounds in a full magazine (the clip has magSize + 1 frames)
		float rate = 1.0f;     // recoil: playback speed (1 = the anim's own 30 fps)
		int lastClip = -2;     // for logging changes
		int lastDumpClip = -2; // belt_dump
		float lastTarget = -1;
		int variant = -1;    // resolved variant index
		// recoil state
		int lastShotCount = -1;
		double lastShot = -1e9, burstStart = 0;
		uint32_t leaf = 0;   // the slot the xanim name goes in: root + 1, or a purpose slot's leaf (bo3_slots.h)
		int purpose = -1;    // bo3_slots.h kSlotDefs index; -1 = an old 193 / 195 line (driven by ApplyAdditives)
		int side = 0;        // bo3_slots.h SlotSide (only right is driven)
		double elLastShot = -1e9;  // empty_lastshot: when this line's gun last played a hold-mode last-shot anim
		double elReload = -1;      // EmptyReloadHandoff: when an empty gun's reload began with the layer on (-1 = not in one)
		float elReloadW = 0;       // the layer's weight at that moment
	};
	constexpr int kMaxAdditives = 192;  // recoil + bullet + empty layers for every gun and PaP (128 + the slot lines, 2026-10-02)
	AdditiveConfig g_additives[kMaxAdditives];
	int g_additiveCount;

	// ammohide=<weapon>,<joint 1>,<joint 2>,... : joint k is hidden while the clip holds fewer than k rounds
	// (a belt's spent links, a magazine's follower rounds).
	struct AmmoHideConfig
	{
		char weapon[64];
		char joints[24][32];
		int count;
		int variant = -1;
		uint32_t names[24];  // script strings, made on first use
		int16_t bones[24];   // DObjGetBoneIndex cache: -2 = look up, -1 = not in this DObj
		void *dobj;          // the DObj (and its first model) the cache is for
		uint64_t models;
		int lastHidden = -1;
	};
	constexpr int kMaxAmmoHides = 16;
	AmmoHideConfig g_ammoHides[kMaxAmmoHides];
	int g_ammoHideCount;

	bool ParseAmmoHide(const char *value)
	{
		if (g_ammoHideCount >= kMaxAmmoHides)
			return false;
		AmmoHideConfig &h = g_ammoHides[g_ammoHideCount];
		const char *comma = strchr(value, ',');
		if (!comma || comma == value || comma - value >= static_cast<ptrdiff_t>(sizeof(h.weapon)))
			return false;
		memcpy(h.weapon, value, comma - value);
		h.weapon[comma - value] = 0;
		h.count = 0;
		for (const char *p = comma + 1; *p && h.count < 24;)
		{
			size_t n = strcspn(p, ",");
			if (n && n < sizeof(h.joints[0]))
			{
				memcpy(h.joints[h.count], p, n);
				h.joints[h.count++][n] = 0;
			}
			p += n + (p[n] == ',');
		}
		if (!h.count)
			return false;
		g_ammoHideCount++;
		return true;
	}
	bool g_emptyMeleeFix;
	float g_wopForceYaw;  // wop_debug_yaw=<deg>: every shot's view kick is this yaw, no pitch (sign test)
	// wop_debug=<channel>,<x>,<y>,<z>: a constant added to one output while a WOP weapon is held (sign tests).
	// Channels: 0 view origin (fwd/left/up), 1 view angles, 2 gun origin (fwd/left/up), 3 gun angles, 4 aim (kickAngles).
	float g_wopDebug[5][3];
	bool g_wopKickReturn;  // wop_kickreturn=1: IW8 view-kick return (KickReturn), off by default

	// Camera tuning, all live (re-read with the cfg):
	//   cam_shake=<pitch/yaw>,<roll>,<origin>            scales for the VIEW_ANGLES / VIEW_ORIGIN patterns (default 1,1,1)
	//   cam_shake=<weapon>,<pitch/yaw>,<roll>,<origin>[,<pitchUp 0|1>]   per weapon (a wop_alias falls back to its source's)
	//       pitchUp=1 (also as a 4th value on the global line): the shake's pitch only ever goes up. The signed noise
	//       patterns snap in on the shot, so an unlucky sample dipped the view below the kick.
	//   camera_free=<0..1>  /  camera_free=<weapon>,<0..1>
	//       IW8-style camera animation: 1 = tag_camera moves only the camera and the viewmodel stays put (IW8
	//       CG_View_CalcViewAnimation), 0 = BO3 (the gun turns with the camera). Fractions blend. Default 0.
	struct CamTune
	{
		char weapon[64];
		float shake[3];  // < 0: not set here
		int pitchUp;     // < 0: not set here
		float free;      // < 0: not set here
	};
	CamTune g_camTunes[128];
	int g_camTuneCount;
	float g_camShake[3] = {1, 1, 1}, g_camFree = 0.0f;
	int g_camPitchUp;
	unsigned g_camGen;  // bumped on every (re)parse, so cached lookups refresh

	void ResetCameraTuning()
	{
		g_camTuneCount = 0;
		g_camShake[0] = g_camShake[1] = g_camShake[2] = 1.0f;
		g_camFree = 0.0f;
		g_camPitchUp = 0;
		g_camGen++;
	}

	// wop_kick_consts=iw8|legacy: the PDB build's kick integrator constants, or the pre-PDB guesses (live).
	bool ParseKickConstsLine(const char *line, bool &known)
	{
		known = strncmp(line, "wop_kick_consts=", 16) == 0;
		if (!known)
			return true;
		const char *v = line + 16;
		if (strncmp(v, "iw8", 3) == 0)
			g_iw8kc = &kIw8KickReal;
		else if (strncmp(v, "legacy", 6) == 0)
			g_iw8kc = &kIw8KickLegacy;
		else
			return false;
		return true;
	}

	bool ParseCameraLine(const char *line, bool &known)
	{
		known = true;
		const bool shake = strncmp(line, "cam_shake=", 10) == 0;
		if (!shake && strncmp(line, "camera_free=", 12) != 0)
		{
			known = false;
			return true;
		}
		const char *v = line + (shake ? 10 : 12);
		g_camGen++;
		if (!isalpha(static_cast<unsigned char>(*v)))  // global
			return shake ? sscanf_s(v, "%f,%f,%f,%d", &g_camShake[0], &g_camShake[1], &g_camShake[2], &g_camPitchUp) >= 3
			             : sscanf_s(v, "%f", &g_camFree) == 1;
		char weapon[64] = {};
		float f[3];
		int up = -1;
		int n = sscanf_s(v, "%63[^,],%f,%f,%f,%d", weapon, static_cast<unsigned>(sizeof(weapon)), &f[0], &f[1], &f[2], &up);
		if (shake ? n < 4 : n != 2)
			return false;
		CamTune *t = nullptr;
		for (int i = 0; i < g_camTuneCount && !t; i++)
			if (strcmp(g_camTunes[i].weapon, weapon) == 0)
				t = &g_camTunes[i];
		if (!t)
		{
			if (g_camTuneCount >= 128)
				return false;
			t = &g_camTunes[g_camTuneCount++];
			*t = CamTune{};
			strcpy_s(t->weapon, weapon);
			t->shake[0] = t->shake[1] = t->shake[2] = t->free = -1.0f;
			t->pitchUp = -1;
		}
		if (shake)
		{
			memcpy(t->shake, f, sizeof(f));
			t->pitchUp = up;
		}
		else
			t->free = f[0];
		return true;
	}
	bool g_additiveNamesOnly;  // additive_debug=names: patch slot names, set no weights
	bool g_additiveHooked;
	bool g_swayHooked;  // the gun / view / fire hooks went in with sway_* lines at start (sway_* lines are live from then on)

	// bo3_locomotion.h (defined at the end of this file)
	bool ParseLocomotionLine(const char *line, bool &known);
	bool ParseLocomotionGlobal(const char *line, bool &known);
	void RecordLocoAlias(const char *weapon, const char *source);
	void FinishLocomotionConfig();
	void LiveIdleActiveLine(const char *line);
	void PatchLocomotionSlots();
	bool LocomotionConfigured();
	void ApplyLocomotion(uint8_t *ps, uint8_t *vm, double now);
	void (*g_slideVmFrame)(uint8_t *ps, uint8_t *vm, double now);  // bo3_slide.h sets it when the MW slide is installed
	// bo3_slots.h (included by weapon_tech.h): purpose slots
	int ParseSlotAdditive(const char *value, AdditiveConfig &c);
	bool SlotLiveTuning(const char *value);
	bool ParseSlotsLine(const char *line);
	void LiveSlotsLine(const char *line);
	void FinishSlotConfig();
	void (*g_slotsVmFrame)(uint8_t *ps, uint8_t *vm, double now);  // set by FinishSlotConfig when slot lines remain
	// bo3_sway.h (defined at the end of this file)
	bool ParseSwayLine(const char *line, bool &known);
	void RecordSwayAlias(const char *weapon, const char *source);
	void FinishSwayConfig();
	bool SwayConfigured();
	void LogSwayConfig(const char *when);
	void ResolveSwayVariants();
	void SwayApply(const uint8_t *ps, const uint8_t *cg, float *placement, float *ang);
	void SwayViewAngles(float *angles);
	void SwayOnFire(const void *ps);
	bool SwayEnabled();
	void BeginSwayReload();
	void EndSwayReload();
	// bo3_ik.h (defined at the end of this file)
	bool ParseIkLine(const char *line, bool &known);
	void IkReloadText(const char *text);
	bool IkConfigured();
	void IkFrame(uint8_t *ps, uint8_t *vm);
	void IkOff();  // no IK this frame (the viewmodel shows another weapon)
	void InstallIk();
	// bo3_roundguard.h (FLOATROUND): spent-round guard, shares the IK skeleton hook
	int g_rgDebug;                          // ammohide_debug=1
	std::atomic<void *> g_rgDObj{nullptr};  // the held viewmodel DObj (set by ApplyAmmoHides every frame)
	bool g_rgEnable = true;                 // ammohide_guard=
	float g_rgPark = 12.0f;                 // ammohide_park=
	uint32_t g_rgAnimBits[12];              // the DObj hide bits ApplyAmmoHides wrote this frame (engine's | ours)
	const uint32_t *g_rgVmBits;             // the engine's viewmodel hide bits (vm+0x338)
	volatile int g_rgAnimSet;               // g_rgAnimBits / g_rgVmBits are for g_rgDObj
	void RgAfterSkel(uint8_t *obj, const float *origin);
	bool RgWantsSkelHook();
	// bo3_vmfov.h (included by weapon_tech.h after this file)
	void VmFovReloadText(const char *text);
	char g_cfgPath[MAX_PATH];  // set by the host DLL: re-read when it changes (live tuning)
	FILETIME g_cfgTime;

	// perf_* (bo3_perf.h): what the cfg asks for, and what is actually running
	bool g_perfEventHooks = true, g_perfCfgWatch = true, g_hotLog = false;
	int g_perfPollMs = 2000;
	bool g_eventHooksLive, g_treeHookLive, g_cfgWatchLive;
	unsigned g_perfGen;     // bumped whenever the variant table or a viewmodel tree may have changed (invalidates memos)
	unsigned g_treeBuilds;  // viewmodel anim trees built since the hook went in
	bool g_namesDirty;      // the weapon-name index needs a rebuild (the sway table was re-read)
	enum SubKind { kSubAdditive, kSubAmmoHide, kSubWop, kSubLoco, kSubSway, kSubKinds };
	void PerfFrame(int frame);
	void ResolveKinds(unsigned kinds);  // one pass over the variant table for these kinds (1 << SubKind)
	void ResolveAllVariants();
	void InstallPerfHooks();
	void StartCfgWatch();
	int FindConfigByVariantName(int variant, int kind, ReadCache &rc);

	// Per-shot / per-state debug lines: only with perf_hotlog=1.
#define HotLog(...) (g_hotLog ? Log(__VA_ARGS__) : (void)0)

	// A/B switches (live; a line that is removed counts as 1 again):
	//   additive_enable=0      the additive= layers (empty / recoil / bullet) fade out and are left alone
	//   locomotion_enable=0    walk / jog / idle active: the walk root goes back to the engine, idle active fades out
	//   idle_active_enable=0   only idle active
	//   (sway_enable=0 is the sway's own, bo3_sway.h)
	bool g_additiveEnable = true, g_locoEnable = true, g_idleActiveEnable = true;

	// Melee and the viewmodel's weapon (2026-10-02, the author's research notes):
	// BG_GetViewmodelWeapon 0x1427A79C0(ps) is what CG_UpdateViewWeaponAnim builds the viewmodel from: the held weapon,
	// except in the melee states (21, 23-31, 33: BG_GetMeleeWeapon 0x1427CE210, the knife ps+0x2E8 unless the gun has its
	// own melee) and the offhand states (37-46, 106-112 with ps+0x18 & 2: ps+0x298). The additive / slot layers are keyed on
	// the HELD weapon, so during a knife melee they used to write their weights and times into the knife's tree (the
	// empty layer at weight 1 whenever the clip was empty). Now they leave a tree that isn't the held weapon's alone.
	//   additive_melee_fade=0|1   (default 0; not live) also fade the held gun's own empty / bullet / recoil layers to 0
	//                             during its melee states (an own-melee gun bash), back in after (IW8 keeps them on)
	bool g_additiveMeleeFade;

	// empty_lastshot= (EmptyLastShotTarget, before Reloading() below)
	enum : int8_t { kElAuto = 0, kElIw = 1, kElHold = 2 };
	struct ElOverride
	{
		char weapon[64];
		int8_t mode;
	};
	struct ElConfig
	{
		int8_t def = kElAuto;
		int count = 0;
		ElOverride o[64];
	};
	ElConfig g_el, g_elLoad;  // g_elLoad: built by a (re)parse, copied over g_el once complete

	int8_t ElModeName(const char *v)
	{
		while (*v == ' ' || *v == '\t')
			v++;
		char m[8] = {};
		for (int i = 0; i < 7 && v[i] && !strchr(" \t#\r\n", v[i]); i++)
			m[i] = static_cast<char>(tolower(static_cast<unsigned char>(v[i])));
		return !strcmp(m, "auto") ? kElAuto : !strcmp(m, "iw") ? kElIw : !strcmp(m, "hold") ? kElHold : -1;
	}

	bool ParseEmptyLastShot(const char *v, ElConfig &cfg)
	{
		const char *comma = strchr(v, ',');
		const int8_t mode = ElModeName(comma ? comma + 1 : v);
		if (mode < 0)
			return false;
		if (!comma)
			return cfg.def = mode, true;
		if (comma == v || comma - v >= 64 || cfg.count >= 64)
			return false;
		ElOverride &o = cfg.o[cfg.count++];
		memset(o.weapon, 0, sizeof(o.weapon));
		memcpy(o.weapon, v, comma - v);
		o.mode = mode;
		return true;
	}

	int8_t ElMode(const char *weapon)
	{
		for (int i = 0; i < g_el.count; i++)
			if (_stricmp(g_el.o[i].weapon, weapon) == 0)
				return g_el.o[i].mode;
		return g_el.def;
	}

	uintptr_t kGetViewmodelWeapon = 0x27A79C0;      // BG_GetViewmodelWeapon(ps) -> Weapon (u64)
	uintptr_t kGetViewmodelWeaponCall = 0x4E84CE;   // CG_UpdateViewWeaponAnim: call BG_GetViewmodelWeapon
	int g_vmWeaponCheck = -1;                                  // -1 not checked yet, 0 code doesn't match (guard off), 1 on

	// Melee states whose viewmodel is the melee weapon (0x3EFA00000, the set 0x1427A79C0 / 0x1427BB260 use), minus 32
	// (the post-melee quick raise, which shows the gun).
	bool MeleeViewState(int state) { return state >= 0 && state <= 33 && state != 32 && ((0x3EFA00000ull >> state) & 1); }

	// perf_timing=1 (live): the game thread's time in each hook and feature, from QueryPerformanceCounter, and the frame
	// time (anim hook to anim hook); every 10 s one log line per section: calls, mean / p99 / max in us, and the share
	// of the frame time. Off: one branch per section.
	bool g_perfTiming;
#define BO3_PERF_TIMING 1
	enum PerfSection
	{
		kPtAnimHook, kPtPoll, kPtAdditive, kPtAmmoHide, kPtWop, kPtLoco, kPtGunHook, kPtSway, kPtViewHook, kPtFireHook, kPtTreeBuild,
		kPtIk, kPtIkSkel, kPtFrame, kPtSections
	};
	struct PerfTimer
	{
		const char *name;
		uint64_t n;
		double sum, max;
		uint32_t hist[128];  // 4 buckets per octave from 0.1 us
	};
	PerfTimer g_perfTimers[kPtSections] = {{"anim hook, all"},          {"  variant events + poll"}, {"  additive= layers"},
	                                       {"  ammohide"},              {"  weapon offsets + kick"}, {"  locomotion"},
	                                       {"gun hook (ours)"},         {"  sway"},                  {"view hook (ours)"},
	                                       {"fire hook (ours)"},        {"tree build hook"},         {"  hand IK"},
	                                       {"IK skeleton hook (ours)"}, {"frame time"}};
	double g_perfUsPerTick = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1e6 / static_cast<double>(f.QuadPart); }();
	int64_t PerfTicks()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}
	void PerfRecord(int s, double us)
	{
		PerfTimer &t = g_perfTimers[s];
		t.n++;
		t.sum += us;
		t.max = us > t.max ? us : t.max;
		int b = us > 0.1 ? static_cast<int>(log2(us * 10.0) * 4.0) : 0;
		t.hist[b < 0 ? 0 : b > 127 ? 127 : b]++;
	}
	struct PerfScope
	{
		int s;
		int64_t t0;
		explicit PerfScope(int section) : s(g_perfTiming ? section : -1), t0(s >= 0 ? PerfTicks() : 0) {}
		~PerfScope()
		{
			if (s >= 0)
				PerfRecord(s, static_cast<double>(PerfTicks() - t0) * g_perfUsPerTick);
		}
	};
	double PerfP99(const PerfTimer &t)
	{
		uint64_t want = t.n - t.n / 100, seen = 0;
		for (int b = 0; b < 128; b++)
			if ((seen += t.hist[b]) >= want)
				return fmin(0.1 * exp2((b + 1) / 4.0), t.max);  // the bucket's upper edge, at most the max
		return t.max;
	}
	// Once per frame (the anim hook): the frame time, and the report every 10 s.
	void PerfTimingFrame(double now)
	{
		static double s_start = -1, s_prev = -1;
		if (!g_perfTiming)
		{
			s_start = s_prev = -1;
			return;
		}
		if (s_start < 0)
		{
			for (PerfTimer &t : g_perfTimers)
				t.n = 0, t.sum = t.max = 0, memset(t.hist, 0, sizeof(t.hist));
			s_start = s_prev = now;
			return;
		}
		if (now - s_prev < 0.25)
			PerfRecord(kPtFrame, (now - s_prev) * 1e6);
		s_prev = now;
		if (now - s_start < 10.0)
			return;
		const PerfTimer &fr = g_perfTimers[kPtFrame];
		const double frameSum = fr.sum > 0 ? fr.sum : 1;
		Log("perf timing: %.1f s, %llu frames, %.1f fps; frame time mean %.2f ms, p99 %.2f, max %.2f | switches: additive %d, "
		    "locomotion %d, idle_active %d, sway %d",
		    now - s_start, static_cast<unsigned long long>(fr.n), fr.n / (now - s_start), fr.n ? fr.sum / fr.n / 1000 : 0,
		    PerfP99(fr) / 1000, fr.max / 1000, g_additiveEnable, g_locoEnable, g_idleActiveEnable, SwayEnabled());
		for (int s = 0; s < kPtFrame; s++)
		{
			const PerfTimer &t = g_perfTimers[s];
			if (t.n)
				Log("perf timing:   %-26s %7llu calls  mean %7.2f us  p99 %7.2f  max %8.1f  (%.3f%% of frame time)", t.name,
				    static_cast<unsigned long long>(t.n), t.sum / t.n, PerfP99(t), t.max, 100.0 * t.sum / frameSum);
		}
		for (PerfTimer &t : g_perfTimers)
			t.n = 0, t.sum = t.max = 0, memset(t.hist, 0, sizeof(t.hist));
		s_start = now;
	}

	bool ParsePerfLine(const char *line)
	{
		int v;
		if (sscanf_s(line, "perf_timing=%d", &v) == 1)
			return g_perfTiming = v != 0, true;
		if (sscanf_s(line, "perf_eventhooks=%d", &v) == 1)
			return g_perfEventHooks = v != 0, true;
		if (sscanf_s(line, "perf_cfgwatch=%d", &v) == 1)
			return g_perfCfgWatch = v != 0, true;
		if (sscanf_s(line, "perf_hotlog=%d", &v) == 1)
			return g_hotLog = v != 0, true;
		if (sscanf_s(line, "perf_pollms=%d", &v) == 1 && v >= 100 && v <= 60000)
			return g_perfPollMs = v, true;
		return false;
	}

	// The whole file as text (text mode, as fgets reads it), malloc'd; nullptr if it can't be opened.
	char *ReadCfgFileRaw(const char *path)
	{
		FILE *f = fopen(path, "r");
		if (!f)
			return nullptr;
		size_t cap = 1 << 17, n = 0;
		char *text = static_cast<char *>(malloc(cap));
		for (size_t got; text && (got = fread(text + n, 1, cap - n - 1, f)) > 0;)
		{
			n += got;
			if (n + 1 == cap)
			{
				char *bigger = static_cast<char *>(realloc(text, cap *= 2));
				if (!bigger)
					free(text);
				text = bigger;
			}
		}
		fclose(f);
		if (text)
			text[n] = 0;
		return text;
	}

	// The cfg as every parser reads it (malloc'd; nullptr if it can't be opened): a sectioned (format v2) file comes back
	// as flat key=value lines (bo3_features.h NormaliseCfgText, wt_cfgv2.h), a flat one as it is. Every reader goes
	// through here: the start, the live reloads and the slide watcher.
	char *ReadCfgText(const char *path) { return NormaliseCfgText(ReadCfgFileRaw(path), "weapon_tech.cfg"); }

	// fgets over a buffer: the next line (through its '\n', at most cap - 1 chars) into `line`; nullptr at the end.
	const char *NextCfgLine(const char *p, char *line, size_t cap)
	{
		if (!*p)
			return nullptr;
		size_t n = 0;
		while (*p && n + 1 < cap)
		{
			char c = *p++;
			line[n++] = c;
			if (c == '\n')
				break;
		}
		line[n] = 0;
		return p;
	}

	// Live tuning, applied from the cfg's text: take the weight and speed / mag size of every additive= line that
	// matches a loaded layer (weapon, kind and node). New or removed lines need a restart.
	void ApplyTuningText(const char *text)
	{
		IkReloadText(text);  // every ik line is live (bo3_ik.h)
		VmFovReloadText(text);  // every vmfov line is live (bo3_vmfov.h)
		char line[256];
		memset(g_wopDebug, 0, sizeof(g_wopDebug));  // wop_debug= lines are live too
		ResetCameraTuning();  // cam_shake= / camera_free= are live too
		g_iw8kc = &kIw8KickReal;  // wop_kick_consts= is live too (a removed line is iw8 again)
		const bool timing = g_perfTiming;
		g_additiveEnable = g_locoEnable = g_idleActiveEnable = true;  // the A/B switches: a removed line is 1 again
		g_perfTiming = false;
		// sway_* lines are live whenever the sway hooks went in at start, not only when the last reload found some (one
		// reload without sway_ lines used to turn sway reloading off for the rest of the process)
		const bool sway = g_swayHooked || SwayConfigured();
		if (sway)
			BeginSwayReload();
		g_elLoad = ElConfig();  // empty_lastshot= lines are live (a removed line is auto again)
		for (const char *p = text; (p = NextCfgLine(p, line, sizeof(line))) != nullptr;)
		{
			line[strcspn(line, "\r\n")] = 0;
			if (strncmp(line, "empty_lastshot=", 15) == 0)
			{
				if (!ParseEmptyLastShot(line + 15, g_elLoad))
					Log("additive: live reload: bad line '%s'", line);
				continue;
			}
			if (strncmp(line, "perf_", 5) == 0)  // perf_hotlog= and perf_pollms= are live
			{
				if (ParsePerfLine(line))
					Log("additive: live %s", line);
				continue;
			}
			if (sway && strncmp(line, "wop_alias=", 10) == 0)
			{
				char weapon[64] = {}, source[64] = {};
				if (sscanf_s(line + 10, "%63[^,],%63[^, \t#]", weapon, static_cast<unsigned>(sizeof(weapon)), source,
				             static_cast<unsigned>(sizeof(source))) == 2)
					RecordSwayAlias(weapon, source);
				continue;
			}
			if (sway && strncmp(line, "sway_", 5) == 0)
			{
				bool known;
				if (!ParseSwayLine(line, known))
					Log("sway: live reload: bad line '%s'", line);
				continue;
			}
			bool cam;
			if (FeatOff(kFtCamera) && (strncmp(line, "cam_shake=", 10) == 0 || strncmp(line, "camera_free=", 12) == 0))
				continue;  // [features] camera = off
			if (!ParseCameraLine(line, cam))
				Log("additive: live reload: bad line '%s'", line);
			if (cam)
			{
				Log("additive: live %s", line);
				continue;
			}
			bool kc;
			if (!ParseKickConstsLine(line, kc))
				Log("additive: live reload: bad line '%s'", line);
			if (kc)
			{
				Log("additive: live %s", line);
				continue;
			}
			if (strncmp(line, "wop_debug=", 10) == 0)
			{
				int ch;
				float v[3];
				if (sscanf_s(line + 10, "%d,%f,%f,%f", &ch, &v[0], &v[1], &v[2]) == 4 && ch >= 0 && ch <= 4)
				{
					memcpy(g_wopDebug[ch], v, sizeof(v));
					Log("additive: live wop_debug channel %d = %.2f %.2f %.2f", ch, v[0], v[1], v[2]);
				}
				continue;
			}
			bool loco;  // locomotion_jog= / idle_active_fade= / locomotion_debug= are live too
			if (ParseLocomotionGlobal(line, loco) && loco)
			{
				Log("additive: live %s", line);
				continue;
			}
			if (strncmp(line, "additive_enable=", 16) == 0)
			{
				int on;
				if (sscanf_s(line + 16, "%d", &on) == 1)
					g_additiveEnable = on != 0;
				continue;
			}
			if (strncmp(line, "idle_active=", 12) == 0)  // weight / rate are live
			{
				LiveIdleActiveLine(line);
				continue;
			}
			if (strncmp(line, "slots_", 6) == 0)
			{
				LiveSlotsLine(line);
				continue;
			}
			if (strncmp(line, "additive=", 9) != 0 || SlotLiveTuning(line + 9))
				continue;
			char weapon[64] = {}, kind[16] = {}, xanim[96] = {};
			unsigned root = 0;
			float weight = 1, extra = 0;
			int n = sscanf_s(line + 9, "%63[^,],%15[^,],%u,%95[^,],%f,%f", weapon, (unsigned)sizeof(weapon), kind,
			                 (unsigned)sizeof(kind), &root, xanim, (unsigned)sizeof(xanim), &weight, &extra);
			for (int i = 0; i < g_additiveCount; i++)
			{
				AdditiveConfig &c = g_additives[i];
				bool sameKind = strcmp(kind, c.kind == AdditiveKind::Recoil   ? "recoil"
				                             : c.kind == AdditiveKind::Bullet ? "bullet"
				                                                              : "empty") == 0;
				if (strcmp(weapon, c.weapon) != 0 || !sameKind || root != c.root)
					continue;
				c.weight = n >= 5 ? weight : 1.0f;
				if (c.kind == AdditiveKind::Bullet && extra > 0)
					c.magSize = static_cast<int>(extra);
				if (c.kind == AdditiveKind::Recoil)
					c.rate = n >= 6 && extra >= 0 ? extra : 1.0f;
				Log("additive: live tuning: %s %s weight %.2f rate %.2f mag %d", c.weapon, kind, c.weight, c.rate, c.magSize);
			}
		}
		if (sway)
			EndSwayReload();
		if (FeatOff(kFtAdditives))  // [features]: an off feature stays off through live reloads
			g_additiveEnable = false;
		if (FeatOff(kFtLocomotion))
			g_locoEnable = g_idleActiveEnable = false;
		g_el = g_elLoad;
		Log("additive: empty_lastshot: default %s, %d weapon line(s)", g_el.def == kElIw ? "iw" : g_el.def == kElHold ? "hold" : "auto",
		    g_el.count);
		if (g_perfTiming != timing)
			Log("perf: timing %s (perf_timing)", g_perfTiming ? "ON: a report every 10 s" : "off");
		Log("additive: switches: additive_enable %d, locomotion_enable %d, idle_active_enable %d, sway_enable %d, perf_timing %d", g_additiveEnable,
		    g_locoEnable, g_idleActiveEnable, SwayEnabled(), g_perfTiming);
	}

	// Live tuning, synchronous: stat the cfg and, when its time changed, read and apply it. The first call only records
	// the time. With perf_cfgwatch=1 only the host's first call comes here; a background thread watches after that.
	void ReloadAdditiveTuning()
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!g_cfgPath[0] || !GetFileAttributesExA(g_cfgPath, GetFileExInfoStandard, &a) ||
		    CompareFileTime(&a.ftLastWriteTime, &g_cfgTime) == 0)
			return;
		bool first = g_cfgTime.dwLowDateTime == 0 && g_cfgTime.dwHighDateTime == 0;
		g_cfgTime = a.ftLastWriteTime;
		if (first)
			return;
		if (char *text = ReadCfgText(g_cfgPath))
		{
			ApplyTuningText(text);
			free(text);
		}
	}

	double NowSeconds()
	{
		static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return static_cast<double>(t.QuadPart) / freq.QuadPart;
	}

	// ---- Game clock (2026-10-02) -------------------------------------------------------------------------------------
	// The time every time-driven layer runs on (idle_active, locomotion, sway, slide gesture, slot / additive recoil
	// scrubs, the WOP patterns = cam shake and gun kick, IK): the wall clock, stopped while the game's own clock is.
	// The pause menu in a solo game stops the server, so cg time (cg+0x11A88C, int ms, the clock sway / inspect already
	// read; cg = predicted ps - 0x11A8B0) stops moving while the viewmodel hooks keep running every rendered frame, and
	// layers that ran on QueryPerformanceCounter kept animating (idle_active played on in the pause menu). Now: while cg
	// time hasn't moved for kGamePauseAfter of wall time, the clock holds; when it moves again the clock carries on from
	// where it stopped, so the layers hold their pose and resume without a jump. The engine's own pause flag sits behind
	// the obfuscated dvar getter (cl_paused is a hashed dvar; IDA 2026-10-02 found no plain flag reader), so the game
	// clock itself is the signal, as the engine's anim advance (cg frametime 0) uses it.
	constexpr size_t kGameCgFromPs = 0x11A8B0, kGameCgTime = 0x11A88C;
	constexpr double kGamePauseAfter = 0.05;  // s of wall time without a cg time change (game frames are <= 1000 Hz)
	struct GameClockState
	{
		const uint8_t *cg = nullptr;  // set by the anim hook (FastReadable-checked when it changes)
		double clock = 0, lastWall = -1, lastMove = -1;
		int lastCgTime = 0;
		bool paused = false;
		int pauses = 0;
	} g_gameClock;

	// From AfterViewWeaponAnim each frame: the cg the clock watches.
	void GameClockCg(const uint8_t *ps)
	{
		const uint8_t *cg = ps ? ps - kGameCgFromPs : nullptr;
		if (cg == g_gameClock.cg)
			return;
		if (cg && !FastReadable(cg + kGameCgTime, 4))
		{
			static bool s_logged;
			if (!s_logged)
				Log("gameclock: cg time at %p isn't readable; the layers run on the wall clock (no pause hold)", cg + kGameCgTime);
			s_logged = true;
			cg = nullptr;
		}
		g_gameClock.cg = cg;
		g_gameClock.lastMove = -1;
	}

	double GameNow()
	{
		GameClockState &g = g_gameClock;
		const double wall = NowSeconds();
		if (g.lastWall < 0)
			g.lastWall = wall;
		double dt = wall - g.lastWall;
		g.lastWall = wall;
		dt = dt < 0 ? 0 : dt > 0.25 ? 0.25 : dt;
		bool moving = true;
		if (g.cg)
		{
			const int t = *reinterpret_cast<const int32_t *>(g.cg + kGameCgTime);
			if (g.lastMove < 0 || t != g.lastCgTime)
				g.lastMove = wall, g.lastCgTime = t;
			moving = wall - g.lastMove < kGamePauseAfter;
		}
		if (moving != !g.paused)
		{
			g.paused = !moving;
			if (g.paused && g.pauses < 200)
				g.pauses++, Log("gameclock: paused (cg time %d held %.0f ms); layers hold at game time %.3f", g.lastCgTime,
				                (wall - g.lastMove) * 1000.0, g.clock);
			else if (!g.paused && g.pauses <= 200)
				Log("gameclock: running again at game time %.3f (cg time %d)", g.clock, g.lastCgTime);
		}
		if (moving)
			g.clock += dt;
		return g.clock;
	}
	bool GamePaused() { return g_gameClock.paused; }

	bool ParseAdditive(const char *value)
	{
		if (g_additiveCount >= kMaxAdditives)
			return false;
		AdditiveConfig &c = g_additives[g_additiveCount];
		switch (ParseSlotAdditive(value, c))  // slot:<purpose> as the third field (bo3_slots.h)
		{
		case 1: g_additiveCount++; return true;
		case 0: c = AdditiveConfig{}; return false;
		default: break;
		}
		char kind[16] = {};
		c.weight = 1.0f;
		float extra = 0;
		int n = sscanf_s(value, "%63[^,],%15[^,],%u,%95[^,],%f,%f", c.weapon, (unsigned)sizeof(c.weapon), kind,
		                 (unsigned)sizeof(kind), &c.root, c.xanim, (unsigned)sizeof(c.xanim), &c.weight, &extra);
		c.magSize = static_cast<int>(extra);  // bullet: mag size
		if (n >= 6 && extra >= 0)
			c.rate = extra;                   // recoil: playback speed (0 holds the reference frame)
		if (n < 4 || (c.root != 193 && c.root != 195))
			return false;
		c.leaf = c.root + 1;
		c.kind = strcmp(kind, "recoil") == 0   ? AdditiveKind::Recoil
		         : strcmp(kind, "bullet") == 0 ? AdditiveKind::Bullet
		                                       : AdditiveKind::Empty;
		if (c.kind == AdditiveKind::Bullet && c.magSize <= 0)
			return false;
		g_additiveCount++;
		return true;
	}

	bool g_beltDump;
	void LogNode(void *dobj, uint32_t node);
	// belt_dump=1: once a bullet layer's clip has been steady for 0.4 s, log the model-space skeleton mats (DObj +0x58:
	// quat xyzw, trans xyz) of j_mag1 and j_bullet* on the viewmodel DObj, plus the layer's root/leaf state.
	void BeltDump(void *dobj, const AdditiveConfig &c, int clip)
	{
		uint8_t *o = static_cast<uint8_t *>(dobj);
		const float *mats = *reinterpret_cast<float *const *>(o + 0x58);
		int16_t numBones = *reinterpret_cast<int16_t *>(o + kDObjNumBones);
		if (!mats || numBones <= 0 || !FastReadable(mats, 32 * static_cast<size_t>(numBones)))
			return;
		auto getBone = reinterpret_cast<int (*)(void *, uint32_t, int16_t *, uint32_t)>(g_base + kDObjGetBoneIndex);
		auto slStr = &SlString;
		static const char *const kNames[] = {"tag_mag_attach", "j_mag1", "j_bullet011", "j_bullet010", "j_bullet09", "j_bullet08", "j_bullet07",
		                                     "j_bullet06", "j_bullet05", "j_bullet04", "j_bullet03", "j_bullet02", "j_bullet01"};
		Log("beltdump: clip %d bones %d", clip, numBones);
		LogNode(dobj, c.root);
		LogNode(dobj, c.root + 1);
		for (const char *n : kNames)
		{
			int16_t b = -2;
			if (!getBone(dobj, slStr(n, 0), &b, 0xFFFF) || b < 0 || b >= numBones)
			{
				Log("beltdump: %s none", n);
				continue;
			}
			const float *m = mats + 8 * b;
			Log("beltdump: %s %d q %.6f %.6f %.6f %.6f t %.5f %.5f %.5f", n, b, m[0], m[1], m[2], m[3], m[4], m[5], m[6]);
		}
	}

	// One cfg line: additive=, ammohide=, wop_weapon=, wop_curve=, wop=, wop_kickreturn=, empty_melee_fix=1,
	// additive_debug=names. Returns false for a line it knows but can't parse; `known` is false for keys
	// that aren't ours (left to the caller).
	bool ParseAmmoHideAuto(const char *v);  // BELTHIDE (defined with ApplyAutoAmmoHide)
	bool ParseHideOrder(const char *v, int mode);  // HIDEORDER
	constexpr int kShownFull = 1 << 20;            // RELOADVIEW: ShownClip() "a full mag"
	int ShownClip(uint8_t *ps, uint8_t *vm, int live);
	bool g_ammoHideReload = true;                  // ammohide_reload=0: the old way (layers off, every round shown)
	bool ParseWeaponLine(const char *line, bool &known)
	{
		bool ikOk = ParseIkLine(line, known);  // ik*= lines (wop_alias= is read there too, and still handled below)
		if (known)
			return ikOk;
		bool locoOk = ParseLocomotionLine(line, known);
		if (known)
			return locoOk;
		bool swayOk = ParseSwayLine(line, known);
		if (known)
			return swayOk;
		known = true;
		if (strncmp(line, "wop_alias=", 10) == 0)  // PaP and other variants take their source's locomotion and sway lines too
		{
			char weapon[64] = {}, source[64] = {};
			if (sscanf_s(line + 10, "%63[^,],%63[^, \t#]", weapon, static_cast<unsigned>(sizeof(weapon)), source,
			             static_cast<unsigned>(sizeof(source))) == 2)
			{
				RecordLocoAlias(weapon, source);
				RecordSwayAlias(weapon, source);
			}
		}
		if (strncmp(line, "belt_dump=", 10) == 0)  // debug: log the bullet bones' model-space mats per clip (bullet layers)
		{
			int on;
			return sscanf_s(line + 10, "%d", &on) == 1 && ((g_beltDump = on != 0), true);
		}
		if (strncmp(line, "additive_enable=", 16) == 0)
		{
			int on;
			return sscanf_s(line + 16, "%d", &on) == 1 && ((g_additiveEnable = on != 0), true);
		}
		if (strncmp(line, "additive=", 9) == 0)
			return ParseAdditive(line + 9);
		if (strncmp(line, "empty_lastshot=", 15) == 0)  // re-read live too (ApplyTuningText)
		{
			const bool ok = ParseEmptyLastShot(line + 15, g_elLoad);
			g_el = g_elLoad;
			return ok;
		}
		if (strncmp(line, "ammohide_debug=", 15) == 0)  // FLOATROUND
			return sscanf_s(line + 15, "%d", &g_rgDebug) == 1;
		if (strncmp(line, "ammohide_guard=", 15) == 0)
		{
			int on;
			return sscanf_s(line + 15, "%d", &on) == 1 && ((g_rgEnable = on != 0), true);
		}
		if (strncmp(line, "ammohide_park=", 14) == 0)
			return sscanf_s(line + 14, "%f", &g_rgPark) == 1 && g_rgPark > 0;
		if (strncmp(line, "ammohide_auto=", 14) == 0)  // BELTHIDE
			return ParseAmmoHideAuto(line + 14);
		if (strncmp(line, "ammohide_reload=", 16) == 0)  // RELOADVIEW
		{
			int on;
			return sscanf_s(line + 16, "%d", &on) == 1 && ((g_ammoHideReload = on != 0), true);
		}
		if (strncmp(line, "ammohide_order=", 15) == 0)  // HIDEORDER
			return ParseHideOrder(line + 15, 0);
		if (strncmp(line, "ammohide_spend=", 15) == 0)
			return ParseHideOrder(line + 15, 1);
		if (strncmp(line, "ammohide_reverse=", 17) == 0)
			return ParseHideOrder(line + 17, 2);
		if (strncmp(line, "ammohide=", 9) == 0)
			return ParseAmmoHide(line + 9);
		if (strncmp(line, "wop_kickreturn=", 15) == 0 && !strchr(line, ','))  // the global switch; per weapon below
		{
			int on;
			if (sscanf_s(line + 15, "%d", &on) != 1)
				return false;
			g_wopKickReturn = on != 0;
			return true;
		}
		for (const char *key : {"wop_weapon", "wop_curve", "wop_kickpct", "wop_kickreturn", "wop_kick", "wop_spring", "wop_tilt", "wop_alias", "wop"})
		{
			size_t n = strlen(key);
			if (strncmp(line, key, n) == 0 && line[n] == '=')
				return ParseWop(key, line + n + 1);
		}
		bool cam;
		bool camOk = ParseCameraLine(line, cam);
		if (cam)
			return camOk;
		bool kc;
		bool kcOk = ParseKickConstsLine(line, kc);
		if (kc)
			return kcOk;
		if (strcmp(line, "empty_melee_fix=1") == 0)
			return g_emptyMeleeFix = true;
		if (strcmp(line, "empty_melee_fix=0") == 0)  // the default, spelled out (known, so not flagged as a typo)
			return true;
		if (strncmp(line, "additive_melee_fade=", 20) == 0)
		{
			int v;
			return sscanf_s(line + 20, "%d", &v) == 1 && ((g_additiveMeleeFade = v != 0), true);
		}
		if (strncmp(line, "wop_debug=", 10) == 0)
		{
			int ch;
			float v[3];
			if (sscanf_s(line + 10, "%d,%f,%f,%f", &ch, &v[0], &v[1], &v[2]) != 4 || ch < 0 || ch > 4)
				return false;
			memcpy(g_wopDebug[ch], v, sizeof(v));
			return true;
		}
		if (strncmp(line, "wop_debug_yaw=", 14) == 0)
			return sscanf_s(line + 14, "%f", &g_wopForceYaw) == 1;
		if (strcmp(line, "additive_debug=names") == 0)
			return g_additiveNamesOnly = true;
		if (strncmp(line, "perf_", 5) == 0)
			return ParsePerfLine(line);
		if (strncmp(line, "slots_", 6) == 0)
			return ParseSlotsLine(line);
		known = false;
		return true;
	}

	// Writes each configured xanim name into its weapon variant's slot (root + 1). Weapon variants are found by name in
	// one pass over the table (bo3_perf.h); after that, the registration and tree-build hooks keep them current.
	void PatchVariantSlots() { ResolveKinds(1u << kSubAdditive); }

	// Clip ammo of the held weapon. heldWeapons[i] stores the weapon value; matching on the variant
	// bits avoids calling the game's normaliser, whose signature isn't confirmed.
	int ClipAmmo(uint8_t *ps, uint64_t weapon)
	{
		static bool s_logged;
		for (int i = 0; i < 15; i++)
		{
			uint64_t held = *reinterpret_cast<uint64_t *>(ps + kPsHeldWeapons + i * kPsHeldStride);
			if (!s_logged && held)
				Log("additive: held[%d] = %llx clip %d (current %llx)", i, held,
				    *reinterpret_cast<int32_t *>(ps + kPsAmmoInClip + i * 4), weapon);
			if (held && (held & 0x1FF) == (weapon & 0x1FF))
			{
				s_logged = true;
				return *reinterpret_cast<int32_t *>(ps + kPsAmmoInClip + i * 4);
			}
		}
		s_logged = true;
		return -1;
	}

	// Sets a node's normalised time (0..1) the way the engine does for the walk/juke additives:
	// time and old time, zero the cycle state, reset the notify index, then XAnimSyncTime.
	void SetNodeTime(void *dobj, uint32_t node, float t)
	{
		void *tree = *reinterpret_cast<void **>(dobj);
		if (!tree)
			return;
		auto getInfo = &XGetInfo;
		uint32_t i = getInfo(tree, node);
		if (!i)
			return;
		uint8_t *info = At<uint8_t>(kXAnimInfo + 0x58 * i);
		*reinterpret_cast<float *>(info + 0x10) = t;
		*reinterpret_cast<float *>(info + 0x14) = t;
		*reinterpret_cast<int32_t *>(info + 0x28) = 0;
		*reinterpret_cast<uint16_t *>(info + 0x3A) = 0xFFFF;
		reinterpret_cast<void (*)(uint32_t, float)>(g_base + kSyncTime)(i, t);
	}

	// The frequency (1 / length in seconds) of the XAnimParts bound to a leaf node, or 0.
	float NodeFrequency(void *dobj, uint32_t node)
	{
		void *tree = *reinterpret_cast<void **>(dobj);
		if (!tree)
			return 0;
		uint32_t i = XGetInfo(tree, node);
		if (!i)
			return 0;
		const uint8_t *parts = *reinterpret_cast<uint8_t *const *>(At<uint8_t>(kXAnimInfo + 0x58 * i) + 0x08);
		// Asked every frame for the held weapon's recoil leaf: the answer for the same parts is kept until the variant
		// table or a tree changes (a VirtualQuery per frame otherwise).
		static const uint8_t *s_parts;
		static float s_frequency;
		static unsigned s_gen;
		if (parts && parts == s_parts && s_gen == g_perfGen)
			return s_frequency;
		float frequency = parts && FastReadable(parts, 0x58) ? *reinterpret_cast<const float *>(parts + 0x54) : 0.0f;
		s_parts = parts;
		s_frequency = frequency;
		s_gen = g_perfGen;
		return frequency;
	}

	// Debug: the tree's own state for a node (info +0x10 time, +0x1C goal weight, +0x20 weight,
	// +0x24 rate, +0x50 anim index).
	void LogNode(void *dobj, uint32_t node)
	{
		void *tree = *reinterpret_cast<void **>(dobj);
		if (!tree)
			return;
		auto getInfo = &XGetInfo;
		uint32_t i = getInfo(tree, node);
		if (!i)
		{
			Log("additive:   node %u: no info", node);
			return;
		}
		uint8_t *info = At<uint8_t>(kXAnimInfo + 0x58 * i);
		auto f = [&](size_t o) { return *reinterpret_cast<float *>(info + o); };
		const uint8_t *parts = *reinterpret_cast<uint8_t *const *>(info + 0x08);
		bool leaf = *reinterpret_cast<uint32_t *>(info + 0x30) != 0;
		Log("additive:   node %u: info %u time %.3f goal %.2f weight %.2f rate %.2f%s", node, i, f(0x10), f(0x1C), f(0x20),
		    f(0x24), leaf ? "" : " (blend)");
		if (leaf && parts && FastReadable(parts, 0x58))
			Log("additive:   node %u parts: %u frames, %.1f fps, frequency %.3f (0 = missing anim)", node,
			    *reinterpret_cast<const uint16_t *>(parts + 0x20), *reinterpret_cast<const float *>(parts + 0x50),
			    *reinterpret_cast<const float *>(parts + 0x54));
	}

	// empty_lastshot (2026-10-02, Maya\fire_last_feasibility\REPORT.md): when the empty layer comes on after the last round.
	//   hold  off while the last-shot anim plays (a real fire_last ends in the locked pose itself), then cut over to the
	//         empty layer as that anim blends out: the layer's weight is (1 - the last-shot node's weight) every frame, so
	//         the clip's locked pose and the layer always add up to one lock (no pop, no double lock).
	//   iw    MW2019's PlayAdditiveEmptyAnim: on as soon as the clip is 0 (0.05 s blend), whatever anim plays. For a gun
	//         whose last-shot anim is only its fire anim (bolt forward at its end).
	//   auto  (default) per weapon and per hip / ADS / left gun: hold when the variant's last-shot anim (szXAnims 10 / 106 /
	//         167) is a real clip, i.e. set and not the same name as its fire anim (4 / 104 / 161); iw otherwise.
	// empty_lastshot=<auto|iw|hold> sets the default, empty_lastshot=<weapon>,<auto|iw|hold> one gun (live).
	// Off at reloads and melee exactly as before. Names that can't be read count as hold (the old behaviour).
	// 1 when szXAnims[lastNode] of the variant is a real clip (set, and a different name from szXAnims[fireNode]), 0 when
	// it isn't, -1 when the names can't be read (callers treat that as hold). Memoised per variant until the variant
	// table or a tree changes. Data gate: the name array must be readable and its idle (1) and fire (4) slots non-empty
	// strings, so a wrong +0x48 fails closed.
	int RealLastShot(int variant, int lastNode, int fireNode)
	{
		const int which = lastNode == 10 ? 0 : lastNode == 106 ? 1 : 2;
		static int8_t s_memo[512][3];  // result + 2, 0 = not asked yet
		static unsigned s_gen = ~0u;
		if (s_gen != g_perfGen)
			memset(s_memo, 0, sizeof(s_memo)), s_gen = g_perfGen;
		variant &= 0x1FF;
		if (s_memo[variant][which])
			return s_memo[variant][which] - 2;
		int r = -1;
		ReadCache rc;
		const uint8_t *vdef = variant ? At<uint8_t *>(kWeaponVariants)[variant] : nullptr;
		auto str = [&](const char *a) { return a && rc.Readable(a, 1) && (!a[0] || (a[0] >= 0x20 && a[0] < 0x7F)) && rc.Span(a); };
		const char *last = nullptr, *fire = nullptr;
		if (vdef && rc.Readable(vdef, kVariantAnims + 8))
		{
			const char *const *anims = *reinterpret_cast<const char *const *const *>(vdef + kVariantAnims);
			if (anims && rc.Readable(anims, 8 * kNumWeapAnims) && str(anims[1]) && anims[1][0] && str(anims[4]) && anims[4][0])
			{
				last = anims[lastNode], fire = anims[fireNode];
				if (!last || (rc.Readable(last, 1) && !last[0]))
					r = 0;  // blank: no clip of its own
				else if (str(last) && (!fire || str(fire)))
				{
					const size_t ln = strnlen(last, rc.Span(last));
					r = !(last == fire || (fire && strnlen(fire, rc.Span(fire)) == ln && strncmp(last, fire, ln) == 0));
				}
			}
		}
		Log("empty_lastshot: variant %d node %d '%s' vs fire node %d '%s': %s", variant, lastNode, r >= 0 && last ? last : "",
		    fireNode, r >= 0 && fire ? fire : "",
		    r == 1 ? "a real last-shot clip (auto = hold)" : r == 0 ? "no last-shot clip of its own (auto = iw)" : "names unreadable (hold)");
		s_memo[variant][which] = static_cast<int8_t>(r + 2);
		return r;
	}

	// A node's current weight (XAnimInfo +0x20), 0 when the tree has no info for it.
	float NodeWeight(void *dobj, uint32_t node)
	{
		void *tree = dobj ? *reinterpret_cast<void **>(dobj) : nullptr;
		if (!tree)
			return 0;
		const uint32_t i = XGetInfo(tree, node);
		if (!i)
			return 0;
		const float w = *reinterpret_cast<const float *>(At<uint8_t>(kXAnimInfo + 0x58 * i) + 0x20);
		return w > 1.0f ? 1.0f : w > 0 ? w : 0.0f;
	}

	// The empty layer's goal weight for one gun (left = the dual-wield left gun) whose clip is empty and that isn't
	// reloading or in melee; `playing` is that gun's viewmodel node. Sets the blend time to use with it.
	float EmptyLastShotTarget(AdditiveConfig &c, void *dobj, int variant, bool left, int playing, double now, float &blend)
	{
		static const int kPairs[3][2] = {{10, 4}, {106, 104}, {167, 161}};  // last shot, its fire anim
		const int8_t mode = ElMode(c.weapon);
		float fade = 0;
		for (int k = left ? 2 : 0; k < (left ? 3 : 2); k++)
		{
			const int lastNode = kPairs[k][0];
			if (!(mode == kElHold || (mode == kElAuto && RealLastShot(variant, lastNode, kPairs[k][1]) != 0)))
				continue;  // iw for this node
			if (playing == lastNode)
			{
				c.elLastShot = now;
				blend = 0.1f;
				return 0.0f;
			}
			if (now - c.elLastShot < 0.5)  // its blend-out once it has ended (bounded, should the node keep a weight)
			{
				const float w = NodeWeight(dobj, static_cast<uint32_t>(lastNode));
				fade = w > fade ? w : fade;
			}
		}
		if (now - c.elLastShot < 0.5)
		{
			blend = 0.0f;  // follow the last-shot node's own blend-out frame by frame
			return c.weight * (1.0f - fade);
		}
		blend = mode == kElHold ? 0.1f : 0.05f;  // iw: MW2019's 0.05 s
		return c.weight;
	}

	// Empty -> reload handoff (2026-10-02, "the empty state cycles" on mike1911 and the other pistols). The layer used to
	// drop to 0 over 0.1 s the frame the reload state began, while the engine blends the reload anim in from the fire /
	// idle pose (slide forward) over its own, longer blend: the slide went forward and came back as the reload took
	// over. Now the layer holds its weight until the reload anim has a weight and then follows it out frame by frame,
	// weight = w0 * (1 - reload node weight), the same cut-over as hold-mode empty_lastshot, so the locked pose from the
	// layer plus the reload anim's own locked first frame always add up to one lock. reloadNode is the gun's reload anim
	// node (40..45 right, 171 / 172 left), or -1 while the reload state runs before its anim starts (the layer holds).
	// Only when the layer was on as the reload began; bounded at 0.6 s. Call with the clip 0 and the gun reloading;
	// reset c.elReload = -1 otherwise.
	float EmptyReloadHandoff(AdditiveConfig &c, void *dobj, int reloadNode, double now, float &blend)
	{
		blend = 0.1f;
		if (c.elReload < 0)
		{
			if (!(c.lastTarget > 0.001f))
				return 0.0f;  // the layer wasn't on: nothing to hand over
			c.elReload = now;
			c.elReloadW = c.lastTarget;
		}
		if (now - c.elReload > 0.6)
			return 0.0f;
		blend = 0.0f;
		const float w = reloadNode > 0 ? NodeWeight(dobj, static_cast<uint32_t>(reloadNode)) : 0.0f;
		const float target = c.elReloadW * (1.0f - w);
		if (now == c.elReload)  // first frame of the hand-over (the engine snaps the reload anim in: node weight 1 at once)
			HotLog("additive: empty -> reload handoff %s: node %d weight %.3f -> layer %.3f (%.0f ms)", c.weapon, reloadNode, w, target,
			       (now - c.elReload) * 1000.0);
		return target;
	}

	// Reload states (12-20) and the viewmodel's reload anims (40-45). A reload animates the belt / slide itself, so the
	// ammo-driven layers step aside for it.
	bool Reloading(uint8_t *ps, uint8_t *vm)
	{
		int state = *reinterpret_cast<int32_t *>(ps + kPsWeaponState);
		int playing = *reinterpret_cast<int32_t *>(vm + kVmRightAnim);
		return (state >= 12 && state <= 20) || (playing >= 40 && playing <= 45);
	}

	void ApplyAdditives(uint8_t *ps, uint8_t *vm)
	{
		void *dobj = *reinterpret_cast<void **>(vm + kVmDObj);
		if (!dobj)
			return;
		uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		int variant = static_cast<int>(weapon & 0x1FF);
		auto setGoal = reinterpret_cast<SetGoalWeightFn>(g_base + kSetGoalWeight);
		int state = *reinterpret_cast<int32_t *>(ps + kPsWeaponState);
		int playing = *reinterpret_cast<int32_t *>(vm + kVmRightAnim);
		double now = GameNow();
		static int s_lastVariant = -1;
		if (variant != s_lastVariant)
		{
			Log("additive: holding variant %d (weapon %llx, state %d, anim %d)", variant, weapon, state, playing);
			s_lastVariant = variant;
		}

		// Debug (perf_hotlog=1): every weapon-state / viewmodel-anim change while holding a configured weapon.
		static int s_lastState = -1, s_lastPlaying = -1;
		bool configured = false;
		for (int a = 0; a < g_additiveCount && g_hotLog; a++)
			configured |= g_additives[a].variant == variant;
		if (configured && (state != s_lastState || playing != s_lastPlaying))
		{
			Log("additive: anim: state %d playing %d clip %d", state, playing, ClipAmmo(ps, weapon));
			s_lastState = state;
			s_lastPlaying = playing;
		}

		// additive_melee_fade=1: the gun's own melee (its tree; a knife melee never reaches here, AfterViewWeaponAnim)
		const bool meleeOff = g_additiveMeleeFade && MeleeViewState(state);
		for (int a = 0; a < g_additiveCount; a++)
		{
			AdditiveConfig &c = g_additives[a];
			if (c.variant != variant || c.purpose >= 0)  // slot lines: bo3_slots.h
				continue;
			if (!g_additiveEnable)  // additive_enable=0: fade the layer out, leave its leaf alone
			{
				setGoal(dobj, c.root, 0.0f, 0.15f, 1.0f, 0, 0, 0);
				c.lastTarget = -1;
				c.lastShotCount = -1;
				continue;
			}
			if (c.kind == AdditiveKind::Empty)
			{
				// On while the clip is empty, through raises, drops and melee (the empty raise/drop anims
				// are patched out, so those play the normal ones), but off during reloads, which animate the
				// slide themselves, and until the last-shot anim (10 hip, 106 ADS) has finished, since it
				// ends in the empty pose itself.
				// When it comes on after the last round: empty_lastshot (EmptyLastShotTarget).
				bool reloading = Reloading(ps, vm);
				int clip = ClipAmmo(ps, weapon);
				float blend = 0.1f;
				float target = 0.0f;
				if (clip == 0 && reloading && !meleeOff)  // hand over to the reload anim (EmptyReloadHandoff)
					target = EmptyReloadHandoff(c, dobj, playing >= 40 && playing <= 45 ? playing : -1, now, blend);
				else
				{
					c.elReload = -1;
					target = clip == 0 && !reloading && !meleeOff ? EmptyLastShotTarget(c, dobj, variant, false, playing, now, blend) : 0.0f;
				}
				if (c.elReload < 0 && (clip != c.lastClip || target != c.lastTarget))
				{
					HotLog("additive: empty layer: clip %d state %d playing %d -> weight %.3f (blend %.2f) t %.3f", clip, state, playing,
					       target, blend, now);
					c.lastClip = clip;
					c.lastTarget = target;
				}
				// The pose is the additive's last frame (frame 0 is its reference, a zero delta); hold it there
				// at rate 0.
				setGoal(dobj, c.root + 1, 1.0f, 0.0f, 0.0f, 0, 0, 0);
				SetNodeTime(dobj, c.root + 1, 0.999f);  // not 1.0: that can count as finished
				setGoal(dobj, c.root, target, blend, 1.0f, 0, 0, 0);
			}
			else if (c.kind == AdditiveKind::Bullet)
			{
				// Rounds left in the magazine. Source frame 0 is a full mag and frame <mag> empty, one frame per
				// round; the export puts its reference frame in front, so it has mag + 2 frames and source frame
				// k sits at time (k + 1) / (mag + 1). Off during reloads: the clip stays empty until the reload
				// adds the rounds, and the parked-belt offsets on top of the reload anim's own belt twist the links.
				int clip = ClipAmmo(ps, weapon);
				if (clip < 0)
					continue;
				clip = ShownClip(ps, vm, clip);  // RELOADVIEW: frozen / full / live through a reload (gramien notes)
				int k = c.magSize - (clip > c.magSize ? c.magSize : clip);
				k = k < 0 ? 0 : k > c.magSize ? c.magSize : k;
				float t = static_cast<float>(k + 1) / static_cast<float>(c.magSize + 1);
				if (clip != c.lastClip)
				{
					HotLog("additive: bullet layer: clip %d -> frame %d (t %.3f)", clip, k, t);
					c.lastClip = clip;
				}
				setGoal(dobj, c.root + 1, 1.0f, 0.0f, 0.0f, 0, 0, 0);
				SetNodeTime(dobj, c.root + 1, t > 0.999f ? 0.999f : t);
				setGoal(dobj, c.root, (Reloading(ps, vm) && !g_ammoHideReload) || meleeOff ? 0.0f : c.weight, 0.15f, 1.0f, 0, 0, 0);
				if (g_beltDump)
				{
					static int s_dumpClip = -999;
					static double s_since;
					if (clip != c.lastDumpClip)
						c.lastDumpClip = clip, s_since = now, s_dumpClip = -999;
					else if (now - s_since > 0.4 && s_dumpClip != clip)
						s_dumpClip = clip, BeltDump(dobj, c, clip);
				}
			}
			else  // Recoil
			{
				// IW8's recoil anim (e.g. vm_lm_sierrax_recoil) keys only tag_ads, the viewmodel root. IW8
				// (cg_weapons.cpp) runs it at the HIP only, scrubbed rather than played: time =
				// (now - burst start) / (fireDelay + fireTime * clipSize), i.e. the clip spans one mag dump
				// (cfg rate = anim length / mag-dump time), weight to 1 over 0.15 s while firing and back to 0
				// over weight * 0.15 s after. The export keys only the top branches, so it moves the rig rigidly.
				// A shot is the clip going down (ps weaponShotCount doesn't change in automatic fire).
				int shots = ClipAmmo(ps, weapon);
				bool newShot = c.lastShotCount >= 0 && shots >= 0 && shots < c.lastShotCount;
				c.lastShotCount = shots;
				if (newShot)
				{
					if (now - c.lastShot > 0.2)
					{
						c.burstStart = now;
						HotLog("additive: recoil burst start (shots %d)", shots);
					}
					c.lastShot = now;
				}
				bool firing = now - c.lastShot < 0.2;
				// Drive the additive's time from the burst (the node doesn't advance by itself): it
				// ramps through the clip while the trigger is held and holds where it got to after.
				// The anim's own frequency keeps it at its authored rate (30 fps) whatever its length.
				double held = (firing ? now : c.lastShot) - c.burstStart;
				setGoal(dobj, c.root + 1, 1.0f, 0.0f, 0.0f, 0, 0, 0);
				float t = static_cast<float>(held * NodeFrequency(dobj, c.root + 1) * c.rate);
				SetNodeTime(dobj, c.root + 1, t < 0 ? 0.0f : t > 0.999f ? 0.999f : t);
				float ads = *reinterpret_cast<float *>(ps + kPsAdsFraction);
				float target = firing && !meleeOff ? c.weight * (1.0f - (ads < 0 ? 0 : ads > 1 ? 1 : ads)) : 0.0f;  // hip only
				setGoal(dobj, c.root, target, 0.15f, 1.0f, 0, 0, 0);
				static double s_nextRecoilDump;
				if (g_hotLog && firing && now > s_nextRecoilDump)
				{
					s_nextRecoilDump = now + 0.5;
					Log("additive: recoil: held %.2fs t %.3f target %.2f ads %.2f", held, t, target, ads);
					LogNode(dobj, c.root);
					LogNode(dobj, c.root + 1);
				}
			}
		}
	}

	// Ammohide and weapon-offset (wop) weapons: their variants by name (bo3_perf.h keeps them current after that).
	void ResolveAmmoHideVariants() { ResolveKinds(1u << kSubAmmoHide | 1u << kSubWop); }

	// Evaluates the held weapon's IW8 weapon offset patterns (bo3_wop.h). g_wopHeld is what the view /
	// viewmodel hooks apply this frame (null when the held weapon has none).
	constexpr size_t kCgPredictedPsOffset = 0x11A8B0, kCgKickAngles = 0x2E74D8;  // cg = ps - 0x11A8B0 (NOTES_view.txt)
	WopWeapon *g_wopHeld;
	double g_wopHeldTime = -1;  // when g_wopHeld was last updated (outputs are stale after a pause)

	// ---- IW8 view-kick return ------------------------------------------------------------------------
	// BO3 aims like IW8: CL_FinishMove sends usercmd angles = clientActive viewangles (+0xB8C8) + clientActive
	// kickAngles (+0xB7D8), and CG_DrawActiveFrame copies cg kickAngles (ours) into the latter every frame. So
	// our spring moves the aim, but only a write to clientActive viewangles can make part of the kick permanent,
	// which is what IW8's CgViewSystem::UpdateViewKickState does with whatever the player pulled against it.
	// An offset left in kickAngles would aim the same, but it could never be baked in, and it would have to
	// drain (sinking the aim anyway) or stay in kickAngles forever.
	// clientActive's pointer is encrypted behind an obfuscated getter, so it is caught instead: rax still holds
	// it at the call right after the kickAngles copy (and rcx = cg), where a near stub records both and jumps on.
	// Retail (bo3_retail.h): the getter call and the three stores sit in a small setter (kKickCopy calls it; it returns
	// rax = clientActive), and the ADS-scale test is a helper (kKickAdsScaleCheck) over BG_GetWeaponDef (kGetWeaponDef).
	uintptr_t kKickCopy = 0x11D67DD;              // call ClientActive(lc), then the three kickAngles stores
	uintptr_t kClientActiveGetter = 0xBF110;
	uintptr_t kKickSetter = 0;                    // retail: setter(lc, kick*): getter + the three stores; 0 = Enhanced form
	uintptr_t kCallAfterKickCopy = 0x11D6808;     // call 0x141288680(cg, ..): rax = clientActive
	uintptr_t kAfterKickCopyTarget = 0x1288680;
	uintptr_t kKickAdsScaleCheck = 0x11D672A;     // weaponDef+0xEBF && ads == 1: aim kick x0.25
	uintptr_t kGetWeaponDef = 0;                  // retail: BG_GetWeaponDef(weapon) (table lookup + variant+0x18)
	uintptr_t kFinishMoveAngles = 0x13E902D;      // CL_FinishMove: cmd angles[0] = cl+0xB8C8 + cl+0xB7D8
	// The kickAngles copy exists twice: the block above (CG_DrawActiveFrame 0x1411D4950) and a twin in 0x1406E82D0
	// (retail 0x1406114A0), which is the one that runs in ZM: with only the first hooked, clientActive was never caught
	// ("not caught for this cg", 2026-10-01 Karelia, 2026-10-03 retail). The twin is hooked too.
	//   Enhanced: call getter (kTwinGetterCall); lea rdx,[rax+B7D8h]; lea rcx,[rbp+30h]; call VectorCopy (kTwinCopyCall):
	//             a stub on that call catches cl = rdx - 0xB7D8 and cg = r14.
	//   Retail:   call the kick setter (kTwinGetterCall); the 26 bytes; call 0x1411D7520 (kTwinCopyCall): the same
	//             rax / rcx capture as the first site (rcx = rdi = cg there).
	uintptr_t kTwinGetterCall = 0x6E9DBD, kTwinCopyCall = 0x6E9DCD, kTwinCopyTarget = 0xBEF60;
	constexpr size_t kClKickAngles = 0xB7D8, kClViewAngles = 0xB8C8, kWeaponDefKickAdsScale = 0xEBF;
	struct ClCapture
	{
		uint8_t *cl, *cg;
	} g_clCapture;  // written by the stub every frame
	bool g_kickReturnHooked;

	// Retail's shape (retail view-model mapping): kKickCopy calls a plain setter that calls the getter and does the
	// three stores; 26 bytes later comes the capture call. The ADS-scale test is a helper over BG_GetWeaponDef.
	bool KickReturnShapeOkRetail(const uint8_t *copy, int32_t rel, const uint8_t *aim, size_t aimLen)
	{
		static const uint8_t kSetter[] = {
		    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0xE8,  // push rbx; sub rsp,20h; mov rbx,rdx; call getter
		    0, 0, 0, 0,                                                  //   (its rel32, checked below)
		    0x8B, 0x0B, 0x89, 0x88, 0xD8, 0xB7, 0x00, 0x00,              // mov ecx,[rbx]; mov [rax+B7D8h],ecx
		    0x8B, 0x4B, 0x04, 0x89, 0x88, 0xDC, 0xB7, 0x00, 0x00,        // mov ecx,[rbx+4]; mov [rax+B7DCh],ecx
		    0x8B, 0x4B, 0x08, 0x89, 0x88, 0xE0, 0xB7, 0x00, 0x00,        // mov ecx,[rbx+8]; mov [rax+B7E0h],ecx
		    0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};                         // add rsp,20h; pop rbx; ret (rax = clientActive)
		static const uint8_t kCopy[] = {
		    0x48, 0x8B, 0x8E, 0xE8, 0x72, 0x2E, 0x00,  // mov rcx, [rsi+2E72E8h]
		    0xE8, 0x44, 0x0A, 0x59, 0x01,              // call a nullsub (C2 00 00): rax survives it
		    0x4C, 0x8B, 0x86, 0x20, 0x72, 0x2E, 0x00,  // mov r8, [rsi+2E7220h]
		    0x48, 0x8B, 0xD7,                          // mov rdx, rdi (ps)
		    0x48, 0x8B, 0xCE,                          // mov rcx, rsi (cg)
		    0xE8};                                     // call (kCallAfterKickCopy)
		static const uint8_t kScale[] = {0x48, 0x8B, 0x89, 0xC0, 0x02, 0x00, 0x00,   // mov rcx, [rcx+2C0h]
		                                 0xE8, 0, 0, 0, 0,                           // call BG_GetWeaponDef
		                                 0x80, 0xB8, 0xBF, 0x0E, 0x00, 0x00, 0x00};  // cmp byte [rax+0EBFh], 0
		// BG_GetWeaponDef after its lea rax,[variant table]: and ecx,1FFh; mov rax,[rax+rcx*8]; mov rax,[rax+18h]; ret
		static const uint8_t kDefTail[] = {0x81, 0xE1, 0xFF, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x04, 0xC8, 0x48, 0x8B, 0x40, 0x18, 0xC3};
		if (!kKickSetter || !kGetWeaponDef || copy[0] != 0xE8 || copy + 5 + rel != At<uint8_t>(kKickSetter))
			return false;
		const uint8_t *setter = At<uint8_t>(kKickSetter);
		int32_t r;
		memcpy(&r, setter + 10, 4);
		if (memcmp(setter, kSetter, 10) != 0 || memcmp(setter + 14, kSetter + 14, sizeof(kSetter) - 14) != 0 ||
		    setter + 14 + r != At<uint8_t>(kClientActiveGetter))
			return false;
		memcpy(&r, copy + 5 + 8, 4);
		const uint8_t *nullsub = copy + 5 + 12 + r;
		if (memcmp(copy + 5, kCopy, sizeof(kCopy)) != 0 || kKickCopy + 5 + sizeof(kCopy) - 1 != kCallAfterKickCopy ||
		    nullsub[0] != 0xC2 || nullsub[1] != 0 || nullsub[2] != 0)
			return false;
		const uint8_t *scale = At<uint8_t>(kKickAdsScaleCheck);
		memcpy(&r, scale + 8, 4);
		if (memcmp(scale, kScale, 8) != 0 || memcmp(scale + 12, kScale + 12, sizeof(kScale) - 12) != 0 ||
		    scale + 12 + r != At<uint8_t>(kGetWeaponDef))
			return false;
		const uint8_t *def = At<uint8_t>(kGetWeaponDef);
		memcpy(&r, def + 3, 4);
		return def[0] == 0x48 && def[1] == 0x8D && def[2] == 0x05 && def + 7 + r == At<uint8_t>(kWeaponVariants) &&
		       memcmp(def + 7, kDefTail, sizeof(kDefTail)) == 0 && memcmp(At<uint8_t>(kFinishMoveAngles), aim, aimLen) == 0;
	}

	// The code around both ends has to be exactly what was read in the exe, or nothing is patched.
	bool KickReturnShapeOk()
	{
		static const uint8_t kCopy[] = {
		    0xF3, 0x0F, 0x11, 0xB0, 0xD8, 0xB7, 0x00, 0x00,        // movss [rax+B7D8h], xmm6
		    0xF3, 0x0F, 0x11, 0xB8, 0xDC, 0xB7, 0x00, 0x00,        // movss [rax+B7DCh], xmm7
		    0xF3, 0x44, 0x0F, 0x11, 0x80, 0xE0, 0xB7, 0x00, 0x00,  // movss [rax+B7E0h], xmm8
		    0x4D, 0x8B, 0x87, 0x20, 0x72, 0x2E, 0x00,              // mov r8, [r15+2E7220h]
		    0x48, 0x8B, 0xD6,                                      // mov rdx, rsi
		    0x49, 0x8B, 0xCF,                                      // mov rcx, r15 (cg)
		    0xE8};                                                 // call (kCallAfterKickCopy)
		static const uint8_t kAim[] = {0xF3, 0x0F, 0x10, 0x80, 0xC8, 0xB8, 0x00, 0x00,   // movss xmm0, [rax+B8C8h]
		                               0xF3, 0x0F, 0x58, 0x80, 0xD8, 0xB7, 0x00, 0x00};  // addss xmm0, [rax+B7D8h]
		static const uint8_t kScale[] = {0x48, 0x8B, 0x84, 0xC7, 0x60, 0x23, 0xAD, 0x09,  // mov rax, [rdi+rax*8+9AD2360h]
		                                 0x48, 0x8B, 0x48, 0x18,                          // mov rcx, [rax+18h]
		                                 0x80, 0xB9, 0xBF, 0x0E, 0x00, 0x00, 0x00};       // cmp byte [rcx+0EBFh], 0
		const uint8_t *copy = At<uint8_t>(kKickCopy);
		int32_t rel;
		memcpy(&rel, copy + 1, 4);
		if (IsRetailExe())
			return KickReturnShapeOkRetail(copy, rel, kAim, sizeof(kAim));
		return copy[0] == 0xE8 && copy + 5 + rel == At<uint8_t>(kClientActiveGetter) &&
		       memcmp(copy + 5, kCopy, sizeof(kCopy)) == 0 && kKickCopy + 5 + sizeof(kCopy) - 1 == kCallAfterKickCopy &&
		       memcmp(At<uint8_t>(kFinishMoveAngles), kAim, sizeof(kAim)) == 0 &&
		       memcmp(At<uint8_t>(kKickAdsScaleCheck), kScale, sizeof(kScale)) == 0;
	}

	// mov r11, &g_clCapture; mov [r11], rax; mov [r11+8], rcx; jmp [rip] target. r11 is volatile and not an
	// argument, and the callee sees the original return address and registers.
	size_t BuildCaptureStub(uint8_t *stub, void *capture, void *target)
	{
		uint8_t *p = stub;
		auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
		emit({0x49, 0xBB});
		memcpy(p, &capture, 8);
		p += 8;
		emit({0x49, 0x89, 0x03, 0x49, 0x89, 0x4B, 0x08, 0xFF, 0x25, 0, 0, 0, 0});
		memcpy(p, &target, 8);
		return p + 8 - stub;
	}

	// The twin's capture (see kTwinGetterCall). Returns whether it was hooked.
	bool InstallKickReturnTwin()
	{
		uint8_t *g = At<uint8_t>(kTwinGetterCall), *site = At<uint8_t>(kTwinCopyCall);
		if (!FastReadable(g, 64))
			return false;
		int32_t rel;
		memcpy(&rel, g + 1, 4);
		bool shape;
		if (IsRetailExe())
		{
			static const uint8_t kMid[] = {0x48, 0x8B, 0x8F, 0xE8, 0x72, 0x2E, 0x00,                    // mov rcx, [rdi+2E72E8h]
			                               0xE8, 0, 0, 0, 0,                                            // call the nullsub
			                               0x4C, 0x8B, 0x87, 0x20, 0x72, 0x2E, 0x00,                    // mov r8, [rdi+2E7220h]
			                               0x48, 0x8D, 0x97, 0xB0, 0xA8, 0x11, 0x00,                    // lea rdx, [rdi+11A8B0h]
			                               0x48, 0x8B, 0xCF};                                           // mov rcx, rdi (cg)
			int32_t n;
			memcpy(&n, g + 5 + 8, 4);
			const uint8_t *nullsub = g + 5 + 12 + n;
			shape = g[0] == 0xE8 && g + 5 + rel == At<uint8_t>(kKickSetter) && memcmp(g + 5, kMid, 8) == 0 &&
			        memcmp(g + 5 + 12, kMid + 12, sizeof(kMid) - 12) == 0 && nullsub[0] == 0xC2 && nullsub[1] == 0 &&
			        g + 5 + sizeof(kMid) == site;
		}
		else
		{
			static const uint8_t kMid[] = {0x48, 0x8D, 0x90, 0xD8, 0xB7, 0x00, 0x00,   // lea rdx, [rax+B7D8h]
			                               0x48, 0x8D, 0x4D, 0x30};                     // lea rcx, [rbp+30h]
			shape = g[0] == 0xE8 && g + 5 + rel == At<uint8_t>(kClientActiveGetter) && memcmp(g + 5, kMid, sizeof(kMid)) == 0 &&
			        g + 5 + sizeof(kMid) == site;
		}
		int32_t srel;
		memcpy(&srel, site + 1, 4);
		if (!shape || site[0] != 0xE8 || site + 5 + srel != At<uint8_t>(kTwinCopyTarget))
		{
			Log("additive: kick return: the twin kickAngles copy at +%zx doesn't match; only the first site hooked", kTwinCopyCall);
			return false;
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!stub)
			return false;
		void *capture = &g_clCapture, *target = site + 5 + srel;
		if (IsRetailExe())
			BuildCaptureStub(stub, capture, target);
		else
		{
			// mov r11, &g_clCapture; lea rax, [rdx-0B7D8h]; mov [r11], rax; mov [r11+8], r14; jmp [rip] VectorCopy
			uint8_t *p = stub;
			auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
			emit({0x49, 0xBB});
			memcpy(p, &capture, 8);
			p += 8;
			emit({0x48, 0x8D, 0x82, 0x28, 0x48, 0xFF, 0xFF, 0x49, 0x89, 0x03, 0x4D, 0x89, 0x73, 0x08, 0xFF, 0x25, 0, 0, 0, 0});
			memcpy(p, &target, 8);
		}
		int32_t newRel = static_cast<int32_t>(stub - (site + 5));
		DWORD old;
		VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
		memcpy(site + 1, &newRel, 4);
		VirtualProtect(site + 1, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, 5);
		return true;
	}

	void InstallKickReturn()
	{
		if (g_kickReturnHooked)
			return;
		if (!KickReturnShapeOk())
		{
			Log("additive: kick return: the kickAngles copy / CL_FinishMove code doesn't match; kick return OFF");
			return;
		}
		uint8_t *site = At<uint8_t>(kCallAfterKickCopy);
		int32_t rel;
		memcpy(&rel, site + 1, 4);
		if (site + 5 + rel != At<uint8_t>(kAfterKickCopyTarget))
		{
			Log("additive: kick return: +%zx doesn't call +%zx; kick return OFF", kCallAfterKickCopy, kAfterKickCopyTarget);
			return;
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!stub)
		{
			Log("additive: kick return: no memory near the exe; kick return OFF");
			return;
		}
		BuildCaptureStub(stub, &g_clCapture, site + 5 + rel);
		int32_t newRel = static_cast<int32_t>(stub - (site + 5));
		DWORD old;
		VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
		memcpy(site + 1, &newRel, 4);
		VirtualProtect(site + 1, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, 5);
		g_kickReturnHooked = true;
		const bool twin = InstallKickReturnTwin();
		Log("additive: kick return: hooked (clientActive caught at +%zx%s)", kCallAfterKickCopy, twin ? " and at the twin copy" : "");
	}

	// IW8 CgViewSystem::UpdateViewKickState for the held weapon (Iw8ViewKickReturnUpdate): while firing it adds
	// up how far the player moved their own aim; once the kick recentres, that much of it (at most the kick, plus
	// viewKickMaintainFraction of it) goes into clientActive viewangles as the spring returns. No input and no
	// maintain fraction: nothing is written and the kick recentres fully, as before.
	void KickReturn(WopWeapon &w, uint8_t *ps, float ads, double now, float frame)
	{
		static const WopWeapon *s_weapon;
		static uint8_t *s_cl;
		static float s_prev[2];
		static double s_last = -1;
		static int s_missing, s_logs;
		uint8_t *cl = g_clCapture.cg == ps - kCgPredictedPsOffset ? g_clCapture.cl : nullptr;
		if (!cl)
		{
			if (++s_missing == 120)  // the stub runs later in the frame than this, so not on the first frames
				Log("additive: kick return: clientActive not caught for this cg (%p, %p); skipped", g_clCapture.cl, g_clCapture.cg);
			s_weapon = nullptr;
			return;
		}
		s_missing = 0;
		float *view = reinterpret_cast<float *>(cl + kClViewAngles);
		// Another weapon, clientActive or a gap (menu, death, a weapon without a spring): check it again and start
		// over, so no abandoned return carries over.
		if (&w != s_weapon || cl != s_cl || now - s_last > 0.25)
		{
			s_weapon = nullptr;
			if (!FastReadable(cl + kClKickAngles, kClViewAngles + 12 - kClKickAngles))
			{
				Log("additive: kick return: clientActive %p isn't readable; skipped", cl);
				return;
			}
			// BO3 quarters the aim kick at full ADS on weapons with weaponDef+0xEBF set; the return would be sized
			// for a kick 4x the real one there, so those weapons are left alone.
			const uint8_t *variant = At<uint8_t *>(kWeaponVariants)[*reinterpret_cast<uint64_t *>(ps + kPsWeapon) & 0x1FF];
			const uint8_t *def = variant && FastReadable(variant + 0x18, 8) ? *reinterpret_cast<uint8_t *const *>(variant + 0x18) : nullptr;
			if (!def || !FastReadable(def + kWeaponDefKickAdsScale, 1) || def[kWeaponDefKickAdsScale])
			{
				Log("additive: kick return: %s %s; kick return off for it", w.weapon,
				    def ? "quarters its ADS kick (weaponDef+0xEBF)" : "has no readable weaponDef");
				w.kickReturn = false;
				return;
			}
			static uint8_t *s_loggedCl;
			if (cl != s_loggedCl)
				Log("additive: kick return: clientActive %p, viewangles %.2f %.2f, kickAngles %.2f %.2f", cl, view[0], view[1],
				    reinterpret_cast<float *>(cl + kClKickAngles)[0], reinterpret_cast<float *>(cl + kClKickAngles)[1]);
			s_loggedCl = cl;
			w.viewReturn = {};
			s_prev[0] = view[0];
			s_prev[1] = view[1];
			s_weapon = &w;
			s_cl = cl;
		}
		s_last = now;
		if (!std::isfinite(view[0]) || !std::isfinite(view[1]))
			return;
		// The player's own aim movement this frame: s_prev is taken after our last write.
		const float look[2] = {Iw8AngleNormalize180(view[0] - s_prev[0]), Iw8AngleNormalize180(view[1] - s_prev[1])};
		const bool input = fabsf(look[0]) + fabsf(look[1]) > 1e-4f;
		// IW8 counts while weaponState == FIRING, which lasts a fire time after each shot (w.sustained runs 1.5x to
		// bridge frame jitter, and would hold the transfer back while the kick is already returning). One frame of
		// slack bridges automatic fire.
		const bool firing = now - w.lastShot < w.fireTime + frame + 0.005;
		const int before = w.viewReturn.state;
		const float pulled = w.viewReturn.counterMag;
		float out[2];
		Iw8ViewKickReturnUpdate(w.viewReturn, w.viewKick.angles, firing, input, look, w.kickMaintain, w.kickNoDampening,
		                        static_cast<int>(fmod(now * 1000.0, 1e9)), static_cast<int>(frame * 1000.0f + 0.5f), out);
		if (std::isfinite(out[0]) && std::isfinite(out[1]))
		{
			view[0] += out[0];
			view[1] += out[1];
		}
		s_prev[0] = view[0];
		s_prev[1] = view[1];
		if (before != 2 && w.viewReturn.state == 2 && s_logs < 200)
		{
			s_logs++;
			Log("additive: kick return %s: kick %.2f %.2f at release, player pulled %.2f deg, keeping %.2f %.2f (ads %.2f)",
			    w.weapon, w.viewKick.angles[0], w.viewKick.angles[1], pulled, w.viewReturn.correction[0],
			    w.viewReturn.correction[1], ads);
		}
	}

	void ApplyWeaponOffsets(uint8_t *ps)
	{
		g_wopHeld = nullptr;
		uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		int variant = static_cast<int>(weapon & 0x1FF);
		for (int i = 0; i < g_wopCount; i++)
		{
			WopWeapon &w = g_wops[i];
			if (w.variant != variant)
				continue;
			float ads = *reinterpret_cast<float *>(ps + kPsAdsFraction);
			double now = GameNow();
			UpdateWop(w, ClipAmmo(ps, weapon), ads < 0 ? 0 : ads > 1 ? 1 : ads, now);
			g_wopHeld = &w;
			g_wopHeldTime = now;

			// IW8 kick springs. View: BG_KickAngles (hip or ADS params at 0.5), written over BO3's kickAngles, which
			// the engine adds to the aim after this hook; BO3's own integrator only ever sees zero velocity from
			// our fire hook. Gun: BG_CalculateWeaponMovement_Recoil (params lerped by ADS), added to the gun in
			// CalcWeaponPosHook.
			float frame = w.lastSpringTime < 0 ? 0.0f : static_cast<float>(now - w.lastSpringTime);
			w.lastSpringTime = now;
			if (frame > 0.2f)
				frame = 0.2f;
			float a = ads < 0 ? 0 : ads > 1 ? 1 : ads;
			if (w.hasSpring[0])
			{
				Iw8KickAdvance(w.viewKick, w.springs[0][a > 0.5f ? 1 : 0], frame * 1000.0f);
				// Only while our spring is live (plus one frame to land on 0), so BO3's own uses of kickAngles
				// (damage flinch) still work between bursts.
				static bool s_wasLive;
				const Iw8KickState &k = w.viewKick;
				bool live = k.angles[0] != 0 || k.angles[1] != 0 || k.vel[0] != 0 || k.vel[1] != 0 ||
				            g_wopDebug[4][0] != 0 || g_wopDebug[4][1] != 0;
				if (live || s_wasLive)
				{
					float *kick = reinterpret_cast<float *>(ps - kCgPredictedPsOffset + kCgKickAngles);
					kick[0] = k.angles[0] + g_wopDebug[4][0];
					kick[1] = k.angles[1] + g_wopDebug[4][1];
					kick[2] = 0.0f;
				}
				s_wasLive = live;
				if (g_kickReturnHooked && w.kickReturn)
					KickReturn(w, ps, a, now, frame);
			}
			if (w.hasSpring[1])
			{
				const Iw8KickParams &h = w.springs[1][0], &d = w.springs[1][1];
				Iw8KickParams g = h;
				auto lerp = [a](float x, float y) { return x + (y - x) * a; };
				g.accel = lerp(h.accel, d.accel);
				g.returnAccelScale = lerp(h.returnAccelScale, d.returnAccelScale);
				g.returnSpeedCurveScale = lerp(h.returnSpeedCurveScale, d.returnSpeedCurveScale);
				Iw8KickAdvanceGun(w.gunKick, g, frame);
			}
			static double s_next;
			if (g_hotLog && w.sustained && now > s_next)
			{
				s_next = now + 0.1;
				const float *kick = reinterpret_cast<const float *>(ps - 0x11A8B0 + 0x2E74D8);  // cg kickAngles (aim)
				Log("additive: kickAngles pitch %.2f yaw %.2f roll %.2f  +2E74E4 %.2f %.2f %.2f  gunSpeed %.1f %.1f", kick[0], kick[1],
				    kick[2], kick[3], kick[4], kick[5], kick[-0x2E74D8 / 4 + 0x2D9110 / 4], kick[-0x2E74D8 / 4 + 0x2D9110 / 4 + 1]);
				Log("additive: wop ads %.2f gunAng %.2f %.2f %.2f gunOrg %.2f %.2f %.2f viewAng %.2f %.2f %.2f viewOrg %.2f %.2f %.2f",
				    ads, w.weaponAngles[0], w.weaponAngles[1], w.weaponAngles[2], w.weaponOrigin[0], w.weaponOrigin[1],
				    w.weaponOrigin[2], w.viewAngles[0], w.viewAngles[1], w.viewAngles[2], w.viewOrigin[0], w.viewOrigin[1],
				    w.viewOrigin[2]);
			}
			return;
		}
	}

	// ---- RELOADVIEW (2026-10-02): the round count the mag shows through a reload --------------------------------------
	// Gramien's notetracks on the reload anims (gramien.csc watch_weapon_notetracks): gramien_hide_full_magazine = keep
	// showing the clip the old mag came out with, gramien_show_full_magazine = the new mag is full, gramien_watch_ammo =
	// the live clip again. Read from the playing right-hand anim's XAnimParts notify list (+0xC0 16-byte entries: +0 scr
	// name, +4 normalised time; +0xC8 count) against the leaf's time (info +0x10). A reload anim without those notes
	// keeps the old count until the engine adds the ammo (the clip goes up: reloadAddTime, the mag-in point), then live.
	// Never full at the start of a reload. Bullet layers and both hide paths use it.
	struct ReloadView
	{
		uint64_t weapon = 0;
		bool reloading = false;
		int frozen = 0;
		int mode = 0;      // 0 live, 1 frozen, 2 full
		int lastLogged = -1;
	} g_reloadView;

	int ShownClip(uint8_t *ps, uint8_t *vm, int live)
	{
		if (!g_ammoHideReload)
			return live;
		ReloadView &r = g_reloadView;
		const uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		const bool reloading = Reloading(ps, vm);
		if (weapon != r.weapon)
			r = ReloadView{}, r.weapon = weapon;
		if (!reloading)
		{
			r.reloading = false;
			r.mode = 0;
			return live;
		}
		if (!r.reloading)  // the reload starts: freeze what the mag shows now
		{
			r.reloading = true;
			r.frozen = live;
			r.mode = 1;
		}
		// the notes of the playing reload anim that its time has passed; the latest wins
		static uint32_t s_hide, s_show, s_watch;
		if (!s_hide)
		{
			auto slStr = &SlString;
			s_hide = slStr("gramien_hide_full_magazine", 0);
			s_show = slStr("gramien_show_full_magazine", 0);
			s_watch = slStr("gramien_watch_ammo", 0);
		}
		bool noted = false, lifts = false;
		int notedMode = 1;
		uint8_t *dobj = *reinterpret_cast<uint8_t **>(vm + kVmDObj);
		void *tree = dobj ? *reinterpret_cast<void **>(dobj) : nullptr;
		const int playing = *reinterpret_cast<int32_t *>(vm + kVmRightAnim);
		if (tree && playing > 0 && playing < 197)
		{
			const uint32_t info = XGetInfo(tree, static_cast<uint32_t>(playing));
			const uint8_t *in = info ? At<uint8_t>(kXAnimInfo + 0x58 * info) : nullptr;
			const uint8_t *parts = in ? *reinterpret_cast<const uint8_t *const *>(in + 0x08) : nullptr;
			if (parts && *reinterpret_cast<const uint32_t *>(in + 0x30) != 0 && FastReadable(parts, 0xD0))
			{
				const float time = *reinterpret_cast<const float *>(in + 0x10);
				const uint8_t *notes = *reinterpret_cast<const uint8_t *const *>(parts + 0xC0);
				const int count = parts[0xC8];
				static const uint8_t *s_dumped[64];  // GRAMNOTES debug (perf_hotlog=1): one dump per reload XAnimParts
				static int s_dumpCount;
				bool dumped = false;
				for (int i = 0; i < s_dumpCount; i++)
					dumped |= s_dumped[i] == parts;
				if (g_hotLog && !dumped && s_dumpCount < 64)
				{
					s_dumped[s_dumpCount++] = parts;
					auto txt = [](uint32_t id) -> const char * {
						const uint8_t *sl = *At<uint8_t *>(kSLTable);
						if (!sl || !id || id > 0x100000)
							return "-";
						const char *t = reinterpret_cast<const char *>(sl + 28 * static_cast<size_t>(id) + 4);
						return FastReadable(t, 64) && strnlen(t, 64) < 64 ? t : "?";
					};
					Log("gramnotes: anim %d parts %p '%s' notify %p count %d (+0xC8) frames %u; raw +0xB0..+0xD8:", playing, parts,
					    FastReadable(*reinterpret_cast<const char *const *>(parts), 8) ? *reinterpret_cast<const char *const *>(parts) : "?",
					    notes, count, *reinterpret_cast<const uint16_t *>(parts + 0x20));
					for (int o = 0xB0; o < 0xE0; o += 8)
						Log("gramnotes:   +0x%02X %016llx", o, *reinterpret_cast<const unsigned long long *>(parts + o));
					for (int i = 0; notes && i < (count > 0 ? count : 0) && i < 48 && FastReadable(notes + 16 * i, 16); i++)
					{
						const uint32_t *e = reinterpret_cast<const uint32_t *>(notes + 16 * i);
						Log("gramnotes:   [%d] name %u '%s' time %.4f p1 %u '%s' p2 %u '%s'", i, e[0], txt(e[0]),
						    *reinterpret_cast<const float *>(e + 1), e[2], txt(e[2]), e[3], txt(e[3]));
					}
				}
				if (notes && count > 0 && FastReadable(notes, 16u * count))
				{
					float best = -1.0f;
					for (int i = 0; i < count; i++)
					{
						// XAnimParts notify entry (verified 2026-10-02 on vm_ar_mcharlie_*_reload): +0 u32 scr action
						// ("self notify", "sound", "rumble"), +4 float time 0..1, +8 u32 scr param (the GDT customnote's
						// actionparam1: the notify name for "self notify"), +0xC u32. A plain notetrack keeps its name at +0.
						const uint32_t action = *reinterpret_cast<const uint32_t *>(notes + 16 * i);
						const uint32_t param = *reinterpret_cast<const uint32_t *>(notes + 16 * i + 8);
						const float at = *reinterpret_cast<const float *>(notes + 16 * i + 4);
						auto kind = [&](uint32_t n) { return n == s_hide ? 1 : n == s_show ? 2 : n == s_watch ? 0 : -1; };
						const int m = kind(param) >= 0 ? kind(param) : kind(action);
						if (m < 0)
							continue;
						noted = true;
						lifts |= m != 1;  // the anim has its own show / watch note
						if (at <= time + 1e-4f && at >= best)
							best = at, notedMode = m;
					}
				}
			}
		}
		if (noted)
			r.mode = notedMode;
		// no gramien notes, or only a hide (anov94 reload_empty): live once the engine adds the ammo (mag in)
		if ((!noted || !lifts) && r.mode == 1 && live > r.frozen)
			r.mode = 0;
		const int shown = r.mode == 1 ? r.frozen : r.mode == 2 ? kShownFull : live;
		const int logKey = r.mode * 100000 + (shown == kShownFull ? 99999 : shown);
		if (logKey != r.lastLogged)
		{
			r.lastLogged = logKey;
			HotLog("additive: reload view: %s (clip %d, frozen %d, anim %d, %s)", r.mode == 1 ? "frozen" : r.mode == 2 ? "full" : "live",
			       live, r.frozen, playing, noted ? (lifts ? "gramien notes" : "gramien hide only: ammo-add for the rest") : "no notes: ammo-add fallback");
		}
		return shown;
	}
	// ---- end RELOADVIEW ---------------------------------------------------------------------------------------------------

	// ---- BELTHIDE (2026-10-02): automatic spent-round hide for every bullet layer --------------------------------------
	// A gun with a bullet line (additive=<w>,bullet,... or slot:bullets) and no ammohide= line of its own hides its spent
	// rounds through the same DObj hide bits HidePart uses (DObj+0x20, see ApplyAmmoHides). The rounds are the bullet
	// anim's own parts whose names look like rounds (bullet / j_b_<n> / round / shell, not follower / link-empty / mag),
	// read from the bound leaf's XAnimParts (+0x42 count, +0x68 scr names). Spend order: nearest a follower part first
	// (stack mags: the bottom round goes, the stack rises), else nearest j_bolt / tag_flash first (belts and tubes: the
	// round at the feed goes). The clip leaves the last <clip> of that order shown; a reload shows them all.
	// The first non-round child joint of a round (its link / case joint) is hidden with it.
	// ammohide_auto=0 turns it off; ammohide_auto=<weapon>,0 opts one gun out (an ammohide= line also replaces it).
	constexpr int kAutoHideOrderMax = 128;
	bool g_ammoHideAuto = true;
	char g_ammoHideAutoOff[32][64];
	int g_ammoHideAutoOffCount;
	// HIDEORDER (2026-10-02): per-gun overrides of the spend order, normally written by
	// tools/ammohide_order.py from the bullets anim itself:
	//   ammohide_order=<weapon>,<joint>,...        first spent first; the last <clip> of the list stay shown
	//   ammohide_spend=<weapon>,<joint>:<clip>,... each joint hidden while the clip holds <clip> rounds or fewer
	//   ammohide_reverse=<weapon>                  reverse the built-in (follower / bolt distance) order
	struct HideOrderLine
	{
		char weapon[64];
		int mode;  // 0 order, 1 spend, 2 reverse
		int count;
		char joints[kAutoHideOrderMax][32];
		int spend[kAutoHideOrderMax];
	};
	HideOrderLine g_hideOrders[48];
	int g_hideOrderCount;

	bool ParseHideOrder(const char *v, int mode)
	{
		if (g_hideOrderCount >= 48)
			return false;
		HideOrderLine &h = g_hideOrders[g_hideOrderCount];
		h = HideOrderLine{};
		h.mode = mode;
		char buf[4096];
		strncpy_s(buf, v, _TRUNCATE);
		if (char *hash = strchr(buf, '#'))
			*hash = 0;
		char *ctx = nullptr;
		char *tok = strtok_s(buf, ", \t\r\n", &ctx);
		if (!tok)
			return false;
		strncpy_s(h.weapon, tok, _TRUNCATE);
		// cfg lines are read 255 chars at a time: "<weapon>,+,..." continues the gun's earlier line of the same key
		HideOrderLine *cont = nullptr;
		char *first = strtok_s(nullptr, ", \t\r\n", &ctx);
		if (first && strcmp(first, "+") == 0)
		{
			for (int i = 0; i < g_hideOrderCount && !cont; i++)
				if (_stricmp(g_hideOrders[i].weapon, h.weapon) == 0 && g_hideOrders[i].mode == mode)
					cont = &g_hideOrders[i];
			if (!cont)
				return false;
			first = strtok_s(nullptr, ", \t\r\n", &ctx);
		}
		HideOrderLine &dst = cont ? *cont : h;
		for (tok = first; tok && dst.count < kAutoHideOrderMax; tok = strtok_s(nullptr, ", \t\r\n", &ctx))
		{
			int clip = -1;
			if (char *colon = strchr(tok, ':'))
			{
				*colon = 0;
				clip = atoi(colon + 1);
			}
			if (mode == 1 && clip < 0)
				return false;
			strncpy_s(dst.joints[dst.count], tok, _TRUNCATE);
			dst.spend[dst.count++] = clip;
		}
		if (cont)
			return true;
		if (mode != 2 && !h.count)
			return false;
		for (int i = 0; i < g_hideOrderCount; i++)  // a re-read cfg replaces the gun's earlier line
			if (_stricmp(g_hideOrders[i].weapon, h.weapon) == 0)
			{
				g_hideOrders[i] = h;
				return true;
			}
		g_hideOrderCount++;
		return true;
	}
	constexpr int kAutoHideMax = 128;
	struct AutoHide
	{
		void *dobj = nullptr;
		uint64_t models = 0;
		const void *parts = nullptr;
		int variant = -1;
		int count = 0;                // rounds, in spend order
		int16_t bone[kAutoHideMax];   // DObj bone of each round
		int16_t extra[kAutoHideMax];  // its first non-round child joint (-1 none)
		int spend[kAutoHideMax];      // hidden while clip <= spend (-1: the count rule, the last <clip> stay shown)
		int lastHidden = -1;
	};
	AutoHide g_autoHide;

	bool ParseAmmoHideAuto(const char *v)
	{
		char w[64] = {};
		int on;
		if (!strchr(v, ','))
			return sscanf_s(v, "%d", &on) == 1 && ((g_ammoHideAuto = on != 0), true);
		if (sscanf_s(v, "%63[^,],%d", w, static_cast<unsigned>(sizeof(w)), &on) != 2)
			return false;
		if (!on && g_ammoHideAutoOffCount < 32)
			strcpy_s(g_ammoHideAutoOff[g_ammoHideAutoOffCount++], w);
		return true;
	}

	bool AutoHideRoundName(const char *low)
	{
		if (strstr(low, "follower") || strstr(low, "linkempty") || strstr(low, "mag"))
			return false;
		if (strstr(low, "bullet") || strstr(low, "round") || strstr(low, "shell"))
			return true;
		// BO7 / IW9 rounds: j_ammo_01.. (left gun: j_ammo_01_le, j_ammo_011 ...; tag_ammo_* attach tags are not rounds)
		if (strncmp(low, "j_ammo", 6) == 0)
			return true;
		return strncmp(low, "j_b_", 4) == 0 && isdigit(static_cast<unsigned char>(low[4]));
	}

	void ApplyAutoAmmoHide(uint8_t *ps, uint8_t *vm, uint8_t *dobj, int variant, uint64_t weapon)
	{
		if (!g_ammoHideAuto)
			return;
		const AdditiveConfig *bc = nullptr;
		for (int a = 0; a < g_additiveCount && !bc; a++)
			if (g_additives[a].variant == variant && g_additives[a].kind == AdditiveKind::Bullet)
				bc = &g_additives[a];
		if (!bc)
			return;
		for (int k = 0; k < g_ammoHideAutoOffCount; k++)
			if (_stricmp(g_ammoHideAutoOff[k], bc->weapon) == 0)
				return;
		void *tree = *reinterpret_cast<void **>(dobj);
		if (!tree)
			return;
		// the bound leaf: legacy lines root+1, slot lines the bullets purpose's leaf (bo3_slots.h kSlotDefs: 117)
		const uint32_t leaf = bc->purpose >= 0 ? 117u : static_cast<uint32_t>(bc->root + 1);
		static int s_whyVariant = -1;  // one "why not" line per held variant
		auto why = [&](const char *msg, int v1) {
			if (s_whyVariant != variant)
				s_whyVariant = variant, Log("additive: ammohide auto %s: %s (%d)", bc->weapon, msg, v1);
		};
		const uint32_t info = XGetInfo(tree, leaf);
		if (!info)
			return why("no anim info on the bullet leaf", static_cast<int>(leaf));
		const uint8_t *in = At<uint8_t>(kXAnimInfo + 0x58 * info);
		const uint8_t *parts = *reinterpret_cast<const uint8_t *const *>(in + 0x08);
		if (!parts || *reinterpret_cast<const uint32_t *>(in + 0x30) == 0 || !FastReadable(parts, 0x70))
			return why("the bullet leaf has no parts", static_cast<int>(leaf));
		const uint64_t models = *reinterpret_cast<uint64_t *>(dobj + kDObjModels);
		const int16_t numBones = *reinterpret_cast<int16_t *>(dobj + kDObjNumBones);
		if (numBones <= 0 || numBones > 384)
			return;
		AutoHide &h = g_autoHide;
		if (h.dobj != dobj || h.models != models || h.parts != parts || h.variant != variant)
		{
			h = AutoHide{};
			h.dobj = dobj, h.models = models, h.parts = parts, h.variant = variant;
			const float *mats = *reinterpret_cast<float *const *>(dobj + 0x58);
			const uint16_t nNames = *reinterpret_cast<const uint16_t *>(parts + 0x42);
			const uint32_t *names = *reinterpret_cast<const uint32_t *const *>(parts + 0x68);
			const uint8_t *sl = *At<uint8_t *>(kSLTable);  // scr string table: 28-byte entries, text at +4
			if (!mats || !names || !sl || !nNames || !FastReadable(names, 4u * nNames) || !FastReadable(mats, 32u * numBones))
			{
				h.dobj = nullptr;  // the skeleton isn't built yet right after a weapon change: try again next frame
				return why("parts names or skeleton not readable yet (retrying); parts", nNames);
			}
			auto getBone = reinterpret_cast<int (*)(void *, uint32_t, int16_t *, uint32_t)>(g_base + kDObjGetBoneIndex);
			auto slStr = &SlString;
			int16_t ref = -1;
			int16_t round[kAutoHideMax];
			int n = 0;
			for (int i = 0; i < nNames; i++)
			{
				if (!names[i] || names[i] > 0x100000)
					continue;
				const char *txt = reinterpret_cast<const char *>(sl + 28 * static_cast<size_t>(names[i]) + 4);
				if (!FastReadable(txt, 64) || strnlen(txt, 64) >= 64)
					continue;
				char low[64];
				size_t j = 0;
				for (; txt[j] && j < 63; j++)
					low[j] = static_cast<char>(tolower(static_cast<unsigned char>(txt[j])));
				low[j] = 0;
				const bool follower = strstr(low, "follower") != nullptr || strstr(low, "pusher") != nullptr;
				if (!follower && !AutoHideRoundName(low))
					continue;
				int16_t b = -2;
				if (!getBone(dobj, names[i], &b, 0xFFFF) || b < 0 || b >= numBones)
					continue;
				if (follower)
					ref = b;
				else if (n < kAutoHideMax)
					round[n++] = b;
			}
			bool overridden = false;  // an ammohide_order / _spend line lists the joints itself (the linker can drop
			for (int o = 0; o < g_hideOrderCount; o++)  // parts its skeleton lacks, e.g. the anov94's j_bullet05+)
				overridden |= g_hideOrders[o].mode != 2 && _stricmp(g_hideOrders[o].weapon, bc->weapon) == 0;
			if (!n && !overridden)
				return why("no round joints in the anim's parts found on the DObj; parts", nNames);
			if (ref < 0)
				for (const char *r : {"j_bolt", "tag_flash", "tag_weapon"})
				{
					int16_t b = -2;
					if (getBone(dobj, slStr(r, 0), &b, 0xFFFF) && b >= 0 && b < numBones)
					{
						ref = b;
						break;
					}
				}
			if (ref < 0 && !overridden)
				return;
			if (ref < 0)
				ref = 0;
			auto dist = [&](int16_t b) {
				const float *a = mats + 8 * b, *r = mats + 8 * ref;
				const float dx = a[4] - r[4], dy = a[5] - r[5], dz = a[6] - r[6];
				return dx * dx + dy * dy + dz * dz;
			};
			std::sort(round, round + n, [&](int16_t a, int16_t b) { return dist(a) < dist(b); });  // first spent first
			// parents from each model's list (attached roots under their tag), to find a round's non-round child joint
			int16_t parent[384];
			for (int b = 0; b < numBones; b++)
				parent[b] = -1;
			const int numModels = dobj[0x122];
			auto *const *ms = reinterpret_cast<uint8_t *const *>(models);
			if (ms && FastReadable(ms, numModels * 10))
			{
				const uint16_t *attach = reinterpret_cast<const uint16_t *>(ms + numModels);
				for (int m = 0, start = 0; m < numModels; m++)
				{
					const uint8_t *xm = ms[m];
					if (!xm || !FastReadable(xm, 0x20))
						break;
					const int nb = xm[0x08], nr = xm[0x09];
					const uint8_t *pl = *reinterpret_cast<const uint8_t *const *>(xm + 0x18);
					if (start + nb > numBones || (nb > nr && (!pl || !FastReadable(pl, nb - nr))))
						break;
					for (int i = 0; i < nb; i++)
						parent[start + i] = i >= nr ? static_cast<int16_t>(start + i - pl[i - nr])
						                            : attach[m] != 0xFFFF ? static_cast<int16_t>(attach[m]) : int16_t(-1);
					start += nb;
				}
			}
			for (int k = 0; k < n; k++)
			{
				h.bone[k] = round[k];
				h.extra[k] = -1;
				for (int b = 0; b < numBones && h.extra[k] < 0; b++)
					if (parent[b] == round[k] && std::find(round, round + n, static_cast<int16_t>(b)) == round + n)
						h.extra[k] = static_cast<int16_t>(b);
			}
			h.count = n;
			for (int k = 0; k < n; k++)
				h.spend[k] = -1;
			const char *how = "follower / bolt distance";
			for (int o = 0; o < g_hideOrderCount; o++)
			{
				const HideOrderLine &ol = g_hideOrders[o];
				if (_stricmp(ol.weapon, bc->weapon) != 0)
					continue;
				if (ol.mode == 2)
				{
					std::reverse(h.bone, h.bone + n);
					std::reverse(h.extra, h.extra + n);
					how = "ammohide_reverse";
					break;
				}
				// the listed joints, in the listed order (each keeps the child joint found above)
				int16_t nb[kAutoHideMax], ne[kAutoHideMax];
				int ns[kAutoHideMax], m = 0;
				for (int k = 0; k < ol.count && m < kAutoHideMax; k++)
				{
					int16_t b = -2;
					if (!getBone(dobj, slStr(ol.joints[k], 0), &b, 0xFFFF) || b < 0 || b >= numBones)
						continue;
					int16_t e = -1;  // its first child joint that isn't a round itself (a link / case joint)
					for (int c = 0; c < numBones && e < 0; c++)
					{
						if (parent[c] != b)
							continue;
						bool isRound = false;
						for (int k2 = 0; k2 < ol.count && !isRound; k2++)
						{
							int16_t b2 = -2;
							isRound = getBone(dobj, slStr(ol.joints[k2], 0), &b2, 0xFFFF) && b2 == c;
						}
						if (!isRound)
							e = static_cast<int16_t>(c);
					}
					nb[m] = b, ne[m] = e, ns[m] = ol.mode == 1 ? ol.spend[k] : -1, m++;
				}
				if (!m)
					break;
				memcpy(h.bone, nb, sizeof(int16_t) * m);
				memcpy(h.extra, ne, sizeof(int16_t) * m);
				memcpy(h.spend, ns, sizeof(int) * m);
				h.count = n = m;
				how = ol.mode == 1 ? "ammohide_spend" : "ammohide_order";
				break;
			}
			Log("additive: ammohide auto %s: %d round joint(s) (%s), order from %s: first %d, last %d", bc->weapon, n,
			    bc->purpose >= 0 ? "slot:bullets" : "bullet line", how, h.bone[0], h.bone[n - 1]);
		}
		if (!h.count)
			return;
		int clip = ClipAmmo(ps, weapon);
		if (clip < 0)
			return;
		clip = ShownClip(ps, vm, clip);  // RELOADVIEW
		if (clip > h.count && clip != kShownFull)
			clip = h.count;
		if (clip == kShownFull)
			clip = h.count + 100000;  // every round shown, spend lines too
		const int byCount = h.count - (clip > h.count ? h.count : clip);
		int hidden = 0;
		uint32_t bits[12];
		memcpy(bits, vm + kVmHideBits, sizeof(bits));
		for (int k = 0; k < h.count; k++)
		{
			if (h.spend[k] >= 0 ? clip > h.spend[k] : k >= byCount)
				continue;
			hidden++;
			for (int16_t b : {h.bone[k], h.extra[k]})
				if (b >= 0 && b < numBones)
					bits[b >> 5] |= 0x80000000u >> (b & 31);
		}
		memcpy(dobj + kDObjHideBits, bits, sizeof(bits));
		memcpy(g_rgAnimBits, bits, sizeof(bits));  // FLOATROUND
		if (hidden != h.lastHidden)
		{
			HotLog("additive: ammohide auto %s: clip %d -> %d round(s) hidden", bc->weapon, clip, hidden);
			h.lastHidden = hidden;
		}
	}
	// ---- end BELTHIDE ----------------------------------------------------------------------------------------------------

	// Hides joint k (1-based) of an ammohide list while the clip holds fewer than k rounds. The engine keeps
	// its own viewmodel hide bits at vm+0x338 (hideTags, the empty tag_clip) and copies them into the DObj
	// only when they change, before our hook; the DObj's bits are what the renderer reads (a hidden bone's
	// skinning matrix is zero, so its rigid verts collapse). So every frame: DObj bits = engine bits | ours,
	// never touching vm+0x338, and refilled rounds come back by themselves.
	void ApplyAmmoHides(uint8_t *ps, uint8_t *vm)
	{
		uint8_t *dobj = *reinterpret_cast<uint8_t **>(vm + kVmDObj);
		// FLOATROUND (bo3_roundguard.h): the skeleton hook's viewmodel and the hide bits it rebuilds from
		g_rgAnimSet = 0;
		g_rgDObj.store(dobj, std::memory_order_release);
		if (!dobj)
			return;
		g_rgVmBits = reinterpret_cast<const uint32_t *>(vm + kVmHideBits);
		memcpy(g_rgAnimBits, vm + kVmHideBits, sizeof(g_rgAnimBits));
		g_rgAnimSet = 1;
		uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		int variant = static_cast<int>(weapon & 0x1FF);
		auto getBone = reinterpret_cast<int (*)(void *, uint32_t, int16_t *, uint32_t)>(g_base + kDObjGetBoneIndex);
		for (int h = 0; h < g_ammoHideCount; h++)
		{
			AmmoHideConfig &c = g_ammoHides[h];
			if (c.variant != variant)
				continue;
			int clip = ClipAmmo(ps, weapon);
			if (clip < 0)
				return;
			// Through a reload: the gramien notes / ammo-add view (RELOADVIEW); a full mag shows every round.
			clip = ShownClip(ps, vm, clip);
			if (clip > c.count)
				clip = c.count;
			if (!c.names[0])
				for (int k = 0; k < c.count; k++)
					c.names[k] = SlString(c.joints[k], 0);
			// A rebuilt DObj can reuse the address, so the first model pointer is checked too.
			uint64_t models = *reinterpret_cast<uint64_t *>(dobj + kDObjModels);
			if (dobj != c.dobj || models != c.models)
			{
				c.dobj = dobj;
				c.models = models;
				for (int k = 0; k < c.count; k++)
					c.bones[k] = -2;
			}
			uint32_t bits[12];
			memcpy(bits, vm + kVmHideBits, sizeof(bits));
			int16_t numBones = *reinterpret_cast<int16_t *>(dobj + kDObjNumBones);
			int hidden = 0;
			for (int k = clip < 0 ? 0 : clip; k < c.count; k++)  // joint k + 1 needs k + 1 rounds
			{
				if (c.bones[k] == -1 || !getBone(dobj, c.names[k], &c.bones[k], 0xFFFF))
					continue;
				int16_t b = c.bones[k];
				if (b < 0 || b >= numBones || b >= 384)
					continue;
				bits[b >> 5] |= 0x80000000u >> (b & 31);
				hidden++;
			}
			memcpy(dobj + kDObjHideBits, bits, sizeof(bits));
			memcpy(g_rgAnimBits, bits, sizeof(bits));  // FLOATROUND
			if (hidden != c.lastHidden)
			{
				HotLog("additive: ammohide %s: clip %d -> %d joint(s) hidden", c.weapon, clip, hidden);
				c.lastHidden = hidden;
				static bool s_boneLog;
				if (!s_boneLog)
				{
					s_boneLog = true;
					for (int k = 0; k < c.count; k++)
						Log("additive:   %s -> bone %d", c.joints[k], c.bones[k]);
				}
			}
			return;
		}
		ApplyAutoAmmoHide(ps, vm, dobj, variant, weapon);  // BELTHIDE: the held variant has no ammohide= line
	}

	// True while the viewmodel shows another weapon than the held one (a knife melee, an offhand): the held weapon's
	// additive / slot layers must not be written into that tree. The engine's own weights there are untouched; ours never
	// were on it (the knife's 193 / 195 swim roots are 0 unless written, its juke leaves are zeroed by the juke driver).
	bool ViewmodelIsOtherWeapon(uint8_t *ps, uint8_t *vm)
	{
		if (g_vmWeaponCheck < 0)
		{
			// The function must be the one CG_UpdateViewWeaponAnim calls for its viewmodel weapon (rel32 at the call site).
			const uint8_t *site = At<uint8_t>(kGetViewmodelWeaponCall);
			int32_t rel = 0;
			if (FastReadable(site, 5))
				memcpy(&rel, site + 1, 4);
			g_vmWeaponCheck = FastReadable(site, 5) && site[0] == 0xE8 && site + 5 + rel == At<uint8_t>(kGetViewmodelWeapon);
			Log(g_vmWeaponCheck ? "additive: viewmodel-weapon guard on (BG_GetViewmodelWeapon +%zx, called at +%zx)"
			                    : "additive: +%zx isn't called at +%zx (another build?); viewmodel-weapon guard off",
			    kGetViewmodelWeapon, kGetViewmodelWeaponCall);
		}
		if (!g_vmWeaponCheck)
			return false;
		const uint64_t held = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		const uint64_t shown = reinterpret_cast<uint64_t (*)(void *)>(g_base + kGetViewmodelWeapon)(ps);
		const bool other = (held & 0x1FF) && (shown & 0x1FF) != (held & 0x1FF);
		static bool s_was;
		if (other != s_was)
		{
			s_was = other;
			HotLog("additive: viewmodel %s (held variant %d, shown %d, state %d, dobj %p): layers %s", other ? "shows another weapon" : "back to the held weapon",
			       static_cast<int>(held & 0x1FF), static_cast<int>(shown & 0x1FF), *reinterpret_cast<int32_t *>(ps + kPsWeaponState),
			       *reinterpret_cast<void **>(vm + kVmDObj), other ? "held off" : "driven");
		}
		return other;
	}

	// Runs after CG_UpdateViewWeaponAnim(lc, ps, vm, ..) returns and before the tree advances.
	void AfterViewWeaponAnim(uint8_t *ps, uint8_t *vm)
	{
		double start = NowSeconds();
		PerfTimingFrame(start);
		PerfScope all(kPtAnimHook);
		static int s_frames;
		// Variants registered since the last frame, the table poll when it is due, a cfg edit read by the watcher
		// (bo3_perf.h). Used to be a full rescan and a file stat every 120 frames here, which hitched.
		{
			PerfScope t(kPtPoll);
			PerfFrame(s_frames++);
		}
		// A knife melee / offhand shows another weapon's tree: the held gun's per-tree layers (additive, slots, ammohide hide
		// bits by bone index, locomotion, slide gesture, IK) stay off it. The WOPs move the view / gun placement, not a tree.
		const bool otherTree = ps && vm && ViewmodelIsOtherWeapon(ps, vm);
		// The layers' clock: holds while the game's clock does (the pause menu), see GameNow. `start` stays the wall clock
		// (perf timing, the slow-frame check).
		GameClockCg(ps);
		const double game = GameNow();
		if (ps && vm)
		{
			if (!g_additiveNamesOnly && !otherTree)  // additive_debug=names: slot names only, no weights
			{
				PerfScope t(kPtAdditive);
				ApplyAdditives(ps, vm);
				if (g_slotsVmFrame)
					g_slotsVmFrame(ps, vm, game);  // the purpose-slot layers (bo3_slots.h)
			}
			if (!otherTree)
			{
				PerfScope t(kPtAmmoHide);
				ApplyAmmoHides(ps, vm);
			}
			{
				PerfScope t(kPtWop);
				ApplyWeaponOffsets(ps);
			}
			if (!otherTree)
			{
				PerfScope t(kPtLoco);
				ApplyLocomotion(ps, vm, game);
				if (g_slideVmFrame)
					g_slideVmFrame(ps, vm, game);  // the MW slide gesture layer (bo3_slide.h)
			}
			{
				PerfScope t(kPtIk);
				if (otherTree)
					IkOff();
				else
					IkFrame(ps, vm);
			}
		}
		double ms = (NowSeconds() - start) * 1000.0;
		static int s_slowLogged;
		if (ms > 1.0 && s_slowLogged < 20)
		{
			s_slowLogged++;
			Log("additive: slow frame: %.2f ms in the hook (frame %d)", ms, s_frames);
		}
	}

	bool PatchBytes(uintptr_t rva, std::initializer_list<uint8_t> expect, std::initializer_list<uint8_t> with, const char *what)
	{
		uint8_t *p = At<uint8_t>(rva);
		if (memcmp(p, expect.begin(), expect.size()) != 0)
		{
			if (memcmp(p, with.begin(), with.size()) == 0)
				return true;  // already applied
			Log("additive: %s: unexpected bytes at +%zx, not patched", what, rva);
			return false;
		}
		DWORD old;
		VirtualProtect(p, with.size(), PAGE_EXECUTE_READWRITE, &old);
		memcpy(p, with.begin(), with.size());
		VirtualProtect(p, with.size(), old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, with.size());
		Log("additive: %s patched", what);
		return true;
	}

	// PatchBytes, but only when the ctxLen bytes right before rva are ctx too (that ties a short pattern like a lone jz to
	// the instruction that sets its flag). Only the bytes at rva are written.
	bool PatchBytesCtx(uintptr_t rva, const uint8_t *ctx, size_t ctxLen, std::initializer_list<uint8_t> expect,
	                   std::initializer_list<uint8_t> with, const char *what)
	{
		if (memcmp(At<uint8_t>(rva) - ctxLen, ctx, ctxLen) != 0)
		{
			Log("additive: %s: unexpected code before +%zx, not patched", what, rva);
			return false;
		}
		return PatchBytes(rva, expect, with, what);
	}

	// A call site's rel32 must land on BG_ClipEmpty before it is replaced.
	void PatchClipEmptyCall(uintptr_t rva, const char *what)
	{
		uint8_t *p = At<uint8_t>(rva);
		int32_t rel;
		memcpy(&rel, p + 1, 4);
		if (p[0] == 0xE8 && p + 5 + rel == At<uint8_t>(kClipEmpty))
			PatchBytes(rva, {p[0], p[1], p[2], p[3], p[4]}, {0x31, 0xC0, 0x90, 0x90, 0x90}, what);
		else if (!(p[0] == 0x31 && p[1] == 0xC0))
			Log("additive: %s: not a call to BG_ClipEmpty at +%zx, not patched", what, rva);
	}

	// ---- IW8 recoil hooks (research: the author's research notes) -------------------------
	// All three are call-site rel32 patches (the callees sit in Arxan tables, so no prologue detours).
	uintptr_t kCallCalcWeaponPos = 0x126F94A;  // CG_AddViewWeapon: call CG_CalculateWeaponPosition
	uintptr_t kCalcWeaponPos = 0x126D750;      // (cg*, placement*, float ang[3])
	uintptr_t kCallViewAxis[2] = {0x1174AD3, 0x1174AC2};  // CG_CalcViewValues: AnglesToAxis(refdefViewAngles, viewaxis)
	int kCallViewAxisCount = 2;                // retail: 1 (both view paths reach one call there)
	uintptr_t kCallFireRecoil = 0x12AB480;     // CG_FireWeapon: call BG_WeaponFireRecoil
	uintptr_t kFireRecoil = 0x27C4AF0;
	constexpr size_t kCgRefdefViewAngles = 0x2D88E0, kCgViewOrg = 0x131D78, kCgPredictedPs = 0x11A8B0;

	bool WopLive() { return g_wopHeld && GameNow() - g_wopHeldTime < 0.25; }

	// Points the call at `rva` (E8 rel32) through a near stub at `fn`; `*orig` gets the old target.
	// `expect` (an RVA, or 0) must be the current target.
	bool RedirectCall(uintptr_t rva, uintptr_t expect, void *fn, void **orig, const char *what)
	{
		uint8_t *site = At<uint8_t>(rva);
		int32_t rel;
		memcpy(&rel, site + 1, 4);
		uint8_t *target = site + 5 + rel;
		if (site[0] != 0xE8 || (expect && target != At<uint8_t>(expect)))
		{
			Log("additive: %s: no call to the expected function at +%zx; not hooked", what, rva);
			return false;
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!stub)
		{
			Log("additive: %s: no memory near the exe; not hooked", what);
			return false;
		}
		stub[0] = 0xFF;  // jmp [rip]
		stub[1] = 0x25;
		memset(stub + 2, 0, 4);
		memcpy(stub + 6, &fn, 8);
		*orig = target;
		int32_t newRel = static_cast<int32_t>(stub - (site + 5));
		DWORD old;
		VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
		memcpy(site + 1, &newRel, 4);
		VirtualProtect(site + 1, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, 5);
		Log("additive: %s hooked", what);
		return true;
	}

	// cam_shake= / camera_free= for the held weapon: its own line, else its wop_alias source's, else the global.
	const CamTune *FindCamTune(const WopWeapon &w)
	{
		static const WopWeapon *s_w;
		static unsigned s_gen;
		static const CamTune *s_t;
		if (&w == s_w && s_gen == g_camGen)
			return s_t;
		s_w = &w;
		s_gen = g_camGen;
		s_t = nullptr;
		for (const char *name : {w.weapon, w.source})
			for (int i = 0; i < g_camTuneCount && !s_t && name[0]; i++)
				if (strcmp(g_camTunes[i].weapon, name) == 0)
					s_t = &g_camTunes[i];
		return s_t;
	}
	float CamShake(const WopWeapon &w, int i)
	{
		const CamTune *t = FindCamTune(w);
		return t && t->shake[i] >= 0.0f ? t->shake[i] : g_camShake[i];
	}
	bool CamPitchUp(const WopWeapon &w)
	{
		const CamTune *t = FindCamTune(w);
		return (t && t->pitchUp >= 0 ? t->pitchUp : g_camPitchUp) != 0;
	}
	float CamFree(const WopWeapon &w)
	{
		const CamTune *t = FindCamTune(w);
		float f = t && t->free >= 0.0f ? t->free : g_camFree;
		return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
	}

	// IW8-style camera animation (camera_free). BO3's 0x140599B00, right after the refdef axis is built, copies the
	// refdef into the gun basis (position) and then, when the viewmodel has tag_camera, overwrites the refdef and
	// refdefViewAngles with the tag. The gun's ORIENTATION is read from refdefViewAngles later (CG_AddViewWeapon ->
	// 0x14126D330), so it turns with the camera animation while its position doesn't. IW8 instead keeps the
	// viewmodel on the pre-animation view and moves only the camera. So: remember refdefViewAngles as ViewAxisHook
	// builds the axis (before the tag), and while the gun is placed hand it the pre-animation angles, then put the
	// camera's back. 0x140599B00 itself must NOT be wrapped: it is control-flow obfuscated and spins forever when
	// its caller isn't CG_CalcViewValues (froze the game on the intro, 2026-10-01).
	float *g_camAngles;              // cg+0x2D88E0, seen by ViewAxisHook this frame
	float g_camPre[3], g_camPost[3];  // refdefViewAngles before the tag_camera override / when the gun is placed
	uintptr_t kAxisToAngles = 0x2389D90, kAnglesToAxis = 0x238B9E0;  // (axis, angles) / (angles, axis), NOTES_view.txt
	// Gun: WEAPON_ANGLES / WEAPON_ORIGIN patterns, view-relative, before the gun is placed. ang = pitch/yaw/roll
	// (degrees) on top of bob/sway/gun kick; placement+0x10 = view-space offset (forward, left, up).
	using CalcWeaponPosFn = void (*)(void *cg, float *placement, float *ang);
	CalcWeaponPosFn g_calcWeaponPos;
	void GunHookWork(void *cg, float *placement, float *ang);
	void CalcWeaponPosHook(void *cg, float *placement, float *ang)
	{
		GunHookWork(cg, placement, ang);
		float *view = reinterpret_cast<float *>(static_cast<uint8_t *>(cg) + kCgRefdefViewAngles);
		bool moved = false;  // did the tag_camera override (or anything after ViewAxisHook) turn the view this frame?
		if (view == g_camAngles)
		{
			memcpy(g_camPost, view, sizeof(g_camPost));
			for (int i = 0; i < 3; i++)
				moved |= fabsf(Iw8AngleNormalize180(g_camPost[i] - g_camPre[i])) > 1e-4f;
		}
		const float f = moved && WopLive() ? CamFree(*g_wopHeld) : 0.0f;
		if (f <= 0.0f)
		{
			g_calcWeaponPos(cg, placement, ang);
			return;
		}
		// Axes are rows (fwd, left, up) in world space, so post = L * pre with L the tag's rotation in view space,
		// and the gun gets pre * post^T * cur: the pre-animation view, keeping whatever changed refdefViewAngles
		// after the tag (nothing, normally). A fraction blends the resulting angles back toward the camera's.
		float cam[3], pre[9], post[9], cur[9], m[9], gun[9], target[3];
		memcpy(cam, view, sizeof(cam));
		auto anglesToAxis = reinterpret_cast<void (*)(const float *, float *)>(g_base + kAnglesToAxis);
		anglesToAxis(g_camPre, pre);
		anglesToAxis(g_camPost, post);
		anglesToAxis(cam, cur);
		for (int r = 0; r < 3; r++)  // m = pre * post^T
			for (int c = 0; c < 3; c++)
				m[r * 3 + c] = pre[r * 3] * post[c * 3] + pre[r * 3 + 1] * post[c * 3 + 1] + pre[r * 3 + 2] * post[c * 3 + 2];
		for (int r = 0; r < 3; r++)  // gun = m * cur
			for (int c = 0; c < 3; c++)
				gun[r * 3 + c] = m[r * 3] * cur[c] + m[r * 3 + 1] * cur[3 + c] + m[r * 3 + 2] * cur[6 + c];
		reinterpret_cast<void (*)(const float *, float *)>(g_base + kAxisToAngles)(gun, target);
		for (int i = 0; i < 3; i++)
			view[i] = cam[i] + f * Iw8AngleNormalize180(target[i] - cam[i]);
		g_calcWeaponPos(cg, placement, ang);
		memcpy(view, cam, sizeof(cam));
	}
	void GunHookWork(void *cg, float *placement, float *ang)
	{
		PerfScope all(kPtGunHook);
		if (WopLive())
		{
			for (int i = 0; i < 3; i++)
			{
				ang[i] += g_wopHeld->weaponAngles[i];
				placement[4 + i] += g_wopHeld->weaponOrigin[i];
			}
			if (g_wopHeld->hasSpring[1])  // IW8 gun kick (BO3's own is cancelled in the fire hook)
			{
				const WopWeapon &w = *g_wopHeld;
				const float *k = w.gunKick.angles;
				ang[0] += k[0];
				ang[1] += k[1];
				// Gun tilt (BG_ComputeAndApplyWeaponMovement_TiltAngles): pitch*P, yaw*Y, roll = yaw*R, factors lerped
				// hip -> ADS, rotated about a point <offset> forward (CG_CalculateWeaponMovement_CalcAngles), so
				// the rear of the gun moves against the muzzle.
				float a = w.lastAds, t[4];
				for (int i = 0; i < 4; i++)
					t[i] = w.tilt[0][i] + (w.tilt[1][i] - w.tilt[0][i]) * a;
				float tilt[3] = {t[0] * k[0], t[1] * k[1], t[2] * k[1]};
				if (tilt[0] != 0 || tilt[1] != 0 || tilt[2] != 0)
				{
					float axis[3][3];
					WopAnglesToAxis(tilt, axis);
					for (int i = 0; i < 3; i++)
					{
						ang[i] += tilt[i];
						placement[4 + i] += t[3] * ((i == 0 ? 1.0f : 0.0f) - axis[0][i]);
					}
				}
			}
			for (int i = 0; i < 3; i++)
			{
				ang[i] += g_wopDebug[3][i];
				placement[4 + i] += g_wopDebug[2][i];
			}
		}
		// MW19 sway (bo3_sway.h): its own gate (the held weapon has sway lines), independent of the WOPs
		PerfScope t(kPtSway);
		SwayApply(static_cast<const uint8_t *>(cg) + kCgPredictedPs, static_cast<const uint8_t *>(cg), placement, ang);
	}

	// Camera: VIEW_ANGLES / VIEW_ORIGIN patterns, visual only. Added where the refdef view axis is built,
	// so the gun (placed from a copy of that basis) is carried with the camera as in IW8; aim comes from the
	// usercmd angles and isn't touched. refdefViewAngles is rebuilt every frame, so nothing accumulates.
	using AnglesToAxisFn = void (*)(float *angles, float *axis);
	AnglesToAxisFn g_viewAnglesToAxis;
	void ViewAxisHook(float *angles, float *axis)
	{
		bool live;
		{
			PerfScope t(kPtViewHook);
			live = WopLive();
			if (live)
			{
				const float py = CamShake(*g_wopHeld, 0), roll = CamShake(*g_wopHeld, 1);  // cam_shake=
				float pitch = g_wopHeld->viewAngles[0] * py;
				if (CamPitchUp(*g_wopHeld))
					pitch = -fabsf(pitch);  // negative pitch = up
				angles[0] += pitch + g_wopDebug[1][0];
				for (int i = 1; i < 3; i++)
					angles[i] += g_wopHeld->viewAngles[i] * (i == 2 ? roll : py) + g_wopDebug[1][i];
			}
			SwayViewAngles(angles);  // the MW19 idle's camera part (visual only)
			g_camAngles = angles;  // camera_free: the view before the tag_camera override
			memcpy(g_camPre, angles, sizeof(g_camPre));
		}
		g_viewAnglesToAxis(angles, axis);
		if (live)
		{
			PerfScope t(kPtViewHook);
			float *org = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(angles) - kCgRefdefViewAngles + kCgViewOrg);
			float o[3];
			const float os = CamShake(*g_wopHeld, 2);
			for (int i = 0; i < 3; i++)
				o[i] = g_wopHeld->viewOrigin[i] * os + g_wopDebug[0][i];
			for (int j = 0; j < 3; j++)
				org[j] += o[0] * axis[j] + o[1] * axis[3 + j] + o[2] * axis[6 + j];
		}
	}

	// Per-shot kick: runs BG_WeaponFireRecoil, then replaces its random rectangle kick with the IW8 angular set
	// for this shot of the burst. View kick (kickAVel) is set, not added; gun kick is added to vGunSpeed, so
	// the original's delta is swapped for ours. ADS sets for view kick only at full ADS, for gun kick at any
	// ADS (as BO3 picks its own blocks).
	using FireRecoilFn = void (*)(void *ps, float stickX, float stickY, float *gunSpeed, float *kickAVel,
	                              uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9);
	FireRecoilFn g_fireRecoil;
	void FireRecoilHook(void *ps, float stickX, float stickY, float *gunSpeed, float *kickAVel, uintptr_t a6,
	                    uintptr_t a7, uintptr_t a8, uintptr_t a9)
	{
		float gunBefore[2] = {gunSpeed ? gunSpeed[0] : 0, gunSpeed ? gunSpeed[1] : 0};
		g_fireRecoil(ps, stickX, stickY, gunSpeed, kickAVel, a6, a7, a8, a9);
		PerfScope t(kPtFireHook);
		SwayOnFire(ps);  // the MW19 sway's fire fraction
		auto *p = static_cast<uint8_t *>(ps);
		int variant = static_cast<int>(*reinterpret_cast<uint64_t *>(p + kPsWeapon) & 0x1FF);
		WopWeapon *w = nullptr;
		for (int i = 0; i < g_wopCount; i++)
			if (g_wops[i].variant == variant && g_wops[i].kickCount)
				w = &g_wops[i];
		if (!w)
			return;
		double now = GameNow();
		if (now - w->lastKick > w->fireTime * 1.5 + 0.05)
			w->kickShot = 0;
		else if (!w->cadenceWarned)
		{
			// The IW8 view kick is very sensitive to the shot interval (malima: 80 vs 94 ms nearly doubles the ADS
			// climb), so say so when wop_weapon's fire time doesn't match how fast the gun actually fires.
			w->shotIntervalSum += now - w->lastKick;
			if (++w->shotIntervals == 20)
			{
				double measured = w->shotIntervalSum / w->shotIntervals;
				if (fabs(measured - w->fireTime) > 0.1 * w->fireTime)
					Log("additive: %s fires every %.0f ms but wop_weapon says %.0f ms; the kick data was authored for the latter",
					    w->weapon, measured * 1000.0, w->fireTime * 1000.0);
				w->cadenceWarned = true;
			}
		}
		w->lastKick = now;
		int shot = ++w->kickShot;  // BG_GetShotCountForRecoil: the burst's first shot is 1
		float ads = *reinterpret_cast<float *>(p + kPsAdsFraction);
		bool adsSet = ads >= g_iw8kc->adsKickFrac;  // bg_viewAndGunKickAdsFrac (0.0001 in IW8)
		float pitch, yaw;
		if (kickAVel && g_wopForceYaw != 0.0f)  // wop_debug_yaw=<deg>: every shot kicks straight sideways
		{
			kickAVel[0] = 0.0f;
			kickAVel[1] = g_wopForceYaw;
			kickAVel[2] = 0.0f;
		}
		else if (kickAVel && WopShotKick(*w, adsSet, false, shot, pitch, yaw))
		{
			if (w->hasSpring[0])
			{
				// IW8 spring: our own state takes the kick; BO3's integrator gets nothing (kickAngles is ours)
				Iw8KickFire(w->viewKick, pitch, yaw);
				kickAVel[0] = kickAVel[1] = kickAVel[2] = 0.0f;
			}
			else
			{
				kickAVel[0] = pitch;  // already signed (negative = up)
				kickAVel[1] = yaw;
				kickAVel[2] = -0.5f * yaw;
			}
		}
		if (gunSpeed && WopShotKick(*w, adsSet, true, shot, pitch, yaw))
		{
			if (w->hasSpring[1])
			{
				Iw8KickFire(w->gunKick, pitch, yaw);
				gunSpeed[0] = gunBefore[0];  // cancel BO3's own gun kick
				gunSpeed[1] = gunBefore[1];
			}
			else
			{
				gunSpeed[0] = gunBefore[0] + pitch;
				gunSpeed[1] = gunBefore[1] + yaw;
			}
		}
		if (shot <= 3 || shot % 10 == 0)
			HotLog("additive: kick shot %d ads %.2f view vel (%.1f %.1f) gun vel (%.1f %.1f)", shot, ads, w->viewKick.vel[0],
			    w->viewKick.vel[1], w->gunKick.vel[0], w->gunKick.vel[1]);
	}

	void InstallRecoilHooks()
	{
		RedirectCall(kCallCalcWeaponPos, kCalcWeaponPos, reinterpret_cast<void *>(&CalcWeaponPosHook),
		             reinterpret_cast<void **>(&g_calcWeaponPos), "weapon offsets: gun placement");
		for (int k = 0; k < kCallViewAxisCount; k++)
		{
			const uintptr_t rva = kCallViewAxis[k];
			void *orig = nullptr;
			// both sites call AnglesToAxis (0x14238B9E0, IDA-verified 2026-10-01); anything else isn't hooked
			if (RedirectCall(rva, kAnglesToAxis, reinterpret_cast<void *>(&ViewAxisHook), &orig, "weapon offsets: view axis"))
				g_viewAnglesToAxis = reinterpret_cast<AnglesToAxisFn>(orig);
		}
		bool kicks = false;
		for (int i = 0; i < g_wopCount; i++)
			kicks |= g_wops[i].kickCount > 0;
		if (kicks || SwayConfigured())  // sway only needs the shot times
			RedirectCall(kCallFireRecoil, kFireRecoil, reinterpret_cast<void *>(&FireRecoilHook),
			             reinterpret_cast<void **>(&g_fireRecoil), "weapon offsets: per-shot kick");
		if (g_wopKickReturn && kicks)
			InstallKickReturn();
		else
			Log("additive: kick return off (wop_kickreturn=1 turns it on)");
	}

	// Installs the detour and patches. Enhanced only: the addresses above are the Enhanced exe's.
	void InstallAdditives()
	{
		FinishLocomotionConfig();
		bool loco = LocomotionConfigured();
		FinishSwayConfig();
		bool sway = SwayConfigured();
		static bool s_swayLogged;
		if (sway && !s_swayLogged)
		{
			s_swayLogged = true;
			LogSwayConfig("");
		}
		if (!g_additiveCount && !g_ammoHideCount && !g_wopCount && !g_emptyMeleeFix && !loco && !sway && !IkConfigured())
			return;
		if (!WtExeSupported())
		{
			Log("additive: unknown exe (only Enhanced and retail have addresses); nothing installed");
			return;
		}
		if (g_emptyMeleeFix && !WtDebugSkip("melee"))
		{
			PatchBytesCtx(kMeleeOwnGun, kMeleeCtx, sizeof(kMeleeCtx), {0x74, kMeleeJzRel}, {0xEB, kMeleeJzRel}, "empty melee: gun keeps its own melee");
			PatchClipEmptyCall(kEmptyRaiseCall, "empty raise: normal/quick raise");
			PatchClipEmptyCall(kEmptyDropCall, "empty drop: normal/quick drop");
		}
		if ((!g_additiveCount && !g_ammoHideCount && !g_wopCount && !loco && !sway && !IkConfigured()) || g_additiveHooked)
			return;
		// Tie the fixed data RVAs to code that uses them (IDA 2026-10-01; the empty-melee
		// patches above check their own bytes): lea r12, XAnimInfo; mov rax, [SL table];
		// mov edx, [variant count] (the dword right before the variant table) in the registration block.
		if (!CodeRefers(kXAnimInfoRef, 7, kXAnimInfo) || !CodeRefers(kSLTableRef, 7, kSLTable) ||
		    !CodeRefers(kVariantCountRef, 6, kVariantCount))
		{
			Log("additive: XAnimInfo / SL table / variant table constants don't match the code; anim hook, WOP / sway, IK not installed");
			return;
		}
		Log("additive: data constants match the code (XAnimInfo, SL table, variant table)");
		FinishSlotConfig();  // drops slot lines that can't work, before any slot name is written (bo3_slots.h)
		if ((g_wopCount || sway) && !WtDebugSkip("recoil"))  // the gun / view / fire hooks carry the WOPs, the kick springs and the sway
		{
			InstallRecoilHooks();
			g_swayHooked = sway;
		}

		// Hook AFTER the call to CG_UpdateViewWeaponAnim returns, not the call or the function: it
		// starts with an obfuscated state machine that spun forever whenever anything sat between
		// it and its caller (a prologue detour or a redirected call both did). At the return point
		// rsi = ps and r14 = vm (both callee-saved; retail: r14 = ps, rdi = vm), rsp is 16-byte aligned, and no
		// volatile register is live: the next instructions (displaced into the stub) set their own.
		//   0x4F3860: 66 41 0F 6E 8F 88 A8 11 00   movd xmm1, [r15+11A888h]
		//   0x4F3869: 41 B8 04 00 00 00            mov r8d, 4
		// Retail (0x44B460): ps = r14, vm = rdi, cg = rsi, and 17 plain bytes before the next rip-relative instruction:
		//   48 8B 0F                     mov rcx, [rdi]
		//   41 B8 04 00 00 00            mov r8d, 4
		//   66 0F 6E 8E 88 A8 11 00      movd xmm1, [rsi+11A888h]
		if (WtDebugSkip("anim"))
			return;
		static const uint8_t kDisplacedEnh[] = {0x66, 0x41, 0x0F, 0x6E, 0x8F, 0x88, 0xA8, 0x11, 0x00,
		                                        0x41, 0xB8, 0x04, 0x00, 0x00, 0x00};
		static const uint8_t kDisplacedRetail[] = {0x48, 0x8B, 0x0F, 0x41, 0xB8, 0x04, 0x00, 0x00, 0x00,
		                                           0x66, 0x0F, 0x6E, 0x8E, 0x88, 0xA8, 0x11, 0x00};
		const uint8_t *kDisplaced = IsRetailExe() ? kDisplacedRetail : kDisplacedEnh;
		const size_t kDisplacedLen = IsRetailExe() ? sizeof(kDisplacedRetail) : sizeof(kDisplacedEnh);
		uint8_t *site = At<uint8_t>(kAfterUpdateCall);
		int32_t rel;
		memcpy(&rel, site - 4, 4);
		if (site[-5] != 0xE8 || site + rel != At<uint8_t>(kUpdateViewWeaponAnim) ||
		    memcmp(site, kDisplaced, kDisplacedLen) != 0)
		{
			Log("additive: the code after the CG_UpdateViewWeaponAnim call doesn't match; not hooked");
			return;
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!stub)
		{
			Log("additive: no memory near the exe for the hook stub; not hooked");
			return;
		}
		uint8_t *p = stub;
		auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
		emit({0x48, 0x83, 0xEC, 0x20});  // sub rsp, 20h (shadow space; rsp stays aligned)
		if (IsRetailExe())
		{
			emit({0x4C, 0x89, 0xF1});    // mov rcx, r14   ps
			emit({0x48, 0x89, 0xFA});    // mov rdx, rdi   vm
		}
		else
		{
			emit({0x48, 0x89, 0xF1});    // mov rcx, rsi   ps
			emit({0x4C, 0x89, 0xF2});    // mov rdx, r14   vm
		}
		emit({0x48, 0xB8});              // mov rax, AfterViewWeaponAnim
		void *fn = reinterpret_cast<void *>(&AfterViewWeaponAnim);
		memcpy(p, &fn, 8);
		p += 8;
		emit({0xFF, 0xD0});              // call rax
		emit({0x48, 0x83, 0xC4, 0x20});  // add rsp, 20h
		memcpy(p, kDisplaced, kDisplacedLen);
		p += kDisplacedLen;
		emit({0xFF, 0x25, 0, 0, 0, 0});  // jmp [rip] -> after the displaced code
		uint8_t *back = site + kDisplacedLen;
		memcpy(p, &back, 8);

		uint8_t jump[sizeof(kDisplacedRetail)] = {0xFF, 0x25, 0, 0, 0, 0};  // FF 25 + target + NOPs to the end
		memcpy(jump + 6, &stub, 8);
		for (size_t k = 14; k < kDisplacedLen; k++)
			jump[k] = 0x90;
		DWORD old;
		VirtualProtect(site, kDisplacedLen, PAGE_EXECUTE_READWRITE, &old);
		memcpy(site, jump, kDisplacedLen);
		VirtualProtect(site, kDisplacedLen, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, kDisplacedLen);
		g_additiveHooked = true;
		Log("additive: hooked after CG_UpdateViewWeaponAnim (%d layer(s))", g_additiveCount);
		// Registration and tree-build hooks first, so nothing registered from here on is missed; then every variant
		// already in the table.
		InstallPerfHooks();
		ResolveAllVariants();
		InstallIk();  // the viewmodel skeleton hooks (bo3_ik.h), only with ik= lines
		if (g_perfCfgWatch)
			StartCfgWatch();
		else
			Log("perf: cfg live-tuning check on the game thread every 120 frames (perf_cfgwatch=0)");
	}
}

#include "bo3_locomotion.h"
#include "bo3_sway.h"
#include "bo3_perf.h"
#include "bo3_ik.h"
#include "bo3_roundguard.h"
