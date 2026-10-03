// weapon_tech.dll host (weapon_tech.cpp): everything weapon-specific, split out of stub_boot.dll.
//
//   bo3_additive.h    hooks, cfg parsing + live reload, additive layers (empty / recoil / bullet), ammohide,
//                     empty-melee fix, and the IW8 view / gun hooks
//   bo3_wop.h         IW8 weapon offset patterns (wop_*)
//   bo3_iw8kick.h     IW8 kick springs and kick return
//   bo3_sway.h        MW19 advanced hip sway, idle, stance pivots, ADS gun bob (sway_*)
//   bo3_locomotion.h  MW19 walk / jog / idle active (locomotion*, idle_active*)
//   bo3_ik.h          IW-style viewmodel hand IK on the gun's tag_ik_loc_* (ik*=), honouring the anims' IK notetracks
//   bo3_interrupt.h   IW-style interrupts: raise / reload / rechamber end early at their interrupt point (fire, ADS, ...)
//   bo3_slide.h       MW2019 slide (opt-in, slide_enable=1): IW8 slide movement in BG + the MW slide gesture additive layer
//   bo3_vmfov.h       viewmodel FOV pin (opt-in, vmfov=): gun placement compensated for the world FOV
//   bo3_slots.h       purpose slots: extra additive roots from spare juke leaves (additive=..,slot:<purpose>,..)
//   bo3_perf.h        variant resolution from the engine's events (registration + tree build hooks, fallback poll)
//   bo3_arxan.h       the Arxan neutraliser, once per process (stub_boot.dll carries the same guard)
//   bo3_assets.h      the rawfile pool, for the cfg baked into the map
//
// Config format: flat key=value lines, or (format v2) the sectioned layout of wt_cfgv2.h: [features] master switches
// (bo3_features.h), one [section] per feature, guns lists, [weapon:<name>] sections for a GDT compiler, fenced generator
// blocks. A sectioned file is normalised to flat lines before any parser sees it; a flat file is read as it always was.
// cfg_dump=1 writes the parsed state (weapon_tech.cfgdump.txt) and the normalised lines (weapon_tech.cfgnorm.txt) next to
// the log, to compare two cfgs (tools\cfg_migrate.py --check does the same offline with wt_cfgdump.exe).
//
// Config source, first found wins (the log says which):
//   1. weapon_tech.cfg next to the DLL: live tuning while the game runs (the watcher thread, perf_cfgwatch=1).
//   2. the rawfile weapon_tech/weapon_tech.cfg in the loaded zones (the weapontech linker feature bakes it into
//      the map); read once, no live tuning.
// Logs to weapon_tech.log next to the exe; after init the game thread only queues lines and a background thread writes
// them (bo3_zone.h StartLogWriter). A/B switches and perf_timing=1: bo3_additive.h.
#pragma once
#include "bo3_additive.h"
#include "bo3_arxan.h"
#include "bo3_assets.h"
#include "bo3_segreload.h"
#include "bo3_inspect.h"
#include "bo3_interrupt.h"
#include "bo3_slide.h"
#include "bo3_vmfov.h"
#include "bo3_slots.h"
#include "bo3_retail.h"
#include "bo3_crashlog.h"

namespace
{
	constexpr char kWeaponTechRawCfg[] = "weapon_tech/weapon_tech.cfg";

	enum class CfgSource { None, File, Rawfile };
	CfgSource g_wtSource;
	char g_wtSourceName[MAX_PATH];
	bool g_wtInitialised, g_wtActive;
	int g_wtBadLines, g_wtRoutingLines, g_wtLines, g_wtUnknownLines;

	// stub_boot.cfg's keys: they do nothing here.
	bool IsRoutingLine(const char *line)
	{
		for (const char *key : {"route=", "folder=", "alias=", "extend=", "preload=", "persist=", "stencil="})
			if (strncmp(line, key, strlen(key)) == 0)
				return true;
		return false;
	}

	// An unknown key (no parser claims it; a typo like ik_enabel=1) is logged once per key at load and counted (apart
	// from the bad lines: keys of a feature this DLL doesn't have yet, like slide_*, aren't errors).
	void NoteUnknownKey(const char *line)
	{
		const char *s = line + strspn(line, " \t");
		if (!*s || *s == '#')
			return;  // blank or a comment
		static char s_seen[64][48];
		static int s_seenCount;
		char key[48] = {};
		const size_t n = (std::min)(strcspn(s, "= \t"), sizeof(key) - 1);
		memcpy(key, s, n);
		g_wtUnknownLines++;
		for (int i = 0; i < s_seenCount; i++)
			if (strcmp(s_seen[i], key) == 0)
				return;
		if (s_seenCount < 64)
			strcpy_s(s_seen[s_seenCount++], key);
		Log("weapon_tech: unknown key '%s' in '%s' (ignored; typo?)", key, line);
	}

	// Parses a whole cfg (every line ParseWeaponLine knows). Counts lines, bad lines and routing lines.
	void ParseWeaponTechText(const char *text)
	{
		char line[256];
		for (const char *p = text; (p = NextCfgLine(p, line, sizeof(line))) != nullptr;)
		{
			line[strcspn(line, "\r\n")] = 0;
			g_wtLines++;
			if (IsRoutingLine(line))
			{
				if (!g_wtRoutingLines++)
					Log("weapon_tech: '%s' is a stub_boot.cfg line (routing); ignored here", line);
				continue;
			}
			bool known;
			if (!ParseFeatureLine(line, known))  // feature=<name>,<value> from [features] (bo3_features.h)
			{
				g_wtBadLines++;
				Log("weapon_tech: bad [features] line '%s'", line);
				continue;
			}
			if (known)
				continue;
			if (strncmp(line, "cfg_dump=", 9) == 0)
			{
				g_cfgDump = atoi(line + 9) != 0;
				continue;
			}
			if (!ParseSegReloadLine(line, known))  // segreload_* (bo3_segreload.h); notes wop_alias= and passes it on
			{
				g_wtBadLines++;
				Log("segreload: bad config line '%s'", line);
				continue;
			}
			if (known)
				continue;
			if (!ParseInspectLine(line, known))  // inspect* (bo3_inspect.h); notes wop_alias= and passes it on
			{
				g_wtBadLines++;
				Log("inspect: bad config line '%s'", line);
				continue;
			}
			if (known)
				continue;
			if (!ParseInterruptLine(line, known))  // interrupt* (bo3_interrupt.h); notes wop_alias= and passes it on
			{
				g_wtBadLines++;
				Log("interrupt: bad config line '%s'", line);
				continue;
			}
			if (known)
				continue;
			if (!ParseSlideLine(line, known))  // slide_* (bo3_slide.h)
			{
				g_wtBadLines++;
				Log("slide: bad config line '%s'", line);
				continue;
			}
			if (known)
				continue;
			if (!ParseVmFovLine(line, known))  // vmfov* (bo3_vmfov.h)
			{
				g_wtBadLines++;
				Log("vmfov: bad config line '%s'", line);
				continue;
			}
			if (known)
				continue;
			if (!ParseWeaponLine(line, known))
			{
				g_wtBadLines++;
				Log("additive: bad config line '%s'", line);
			}
			else if (!known)
				NoteUnknownKey(line);
		}
	}

	// A NUL-terminated copy of the baked rawfile (malloc'd), or nullptr. *copies = how many were loaded.
	char *ReadBakedCfg(int *copies)
	{
		*copies = 0;
		const XAssetPool *pool = RawFilePool();
		if (!pool)
		{
			Log("weapon_tech: rawfile pool not found; can't look for the baked '%s'", kWeaponTechRawCfg);
			return nullptr;
		}
		char *text = nullptr;
		*copies = ForEachRawFile(*pool, kWeaponTechRawCfg, [&](RawFile &item) {
			if (text)
				return;  // the first one (normally the only one)
			text = static_cast<char *>(malloc(static_cast<size_t>(item.len) + 1));
			if (!text)
				return;
			memcpy(text, item.buffer, item.len);
			text[item.len] = 0;
		});
		return NormaliseCfgText(text, kWeaponTechRawCfg);
	}

	// ---- [features] (bo3_features.h) ---------------------------------------------------------------------------------
	// After the whole cfg is parsed and before anything installs: an off feature's tables are emptied and its switch set
	// off; on sets the opt-in features' switches. Features [features] doesn't name are left as the keys set them.
	void ApplyFeatureGates()
	{
		g_featGated = true;
		if (FeatOff(kFtAdditives))
		{
			g_additiveCount = 0;
			g_additiveEnable = false;
			g_slotsTakeJukeCount = 0;
			g_slotsTakeAllJukes = false;
		}
		if (FeatOff(kFtAmmoHide))
		{
			g_ammoHideCount = g_hideOrderCount = g_ammoHideAutoOffCount = 0;
			g_ammoHideAuto = g_rgEnable = false;
			g_rgDebug = 0;
		}
		if (FeatOff(kFtKick))
			g_wopCount = 0;
		if (g_feat[kFtKickReturn] >= 0)
			g_wopKickReturn = FeatOn(kFtKickReturn);
		if (FeatOff(kFtCamera))
		{
			ResetCameraTuning();
			g_camTuneCount = 0;
		}
		if (FeatOff(kFtLocomotion))
		{
			g_locoCount = g_locoAliasCount = g_swayCount = g_swayAliasCount = 0;
			g_locoEnable = g_idleActiveEnable = g_swayCfg.enable = false;
		}
		if (g_feat[kFtInspect] >= 0)
			g_insEnable = FeatOn(kFtInspect);
		if (g_feat[kFtEmptyMelee] >= 0)
			g_emptyMeleeFix = FeatOn(kFtEmptyMelee);
		if (FeatOff(kFtEmptyMelee))
			g_intEmptyMelee = false;
		if (FeatOff(kFtInterrupts))
			g_intEnable = g_intTrace = false;
		if (FeatOff(kFtSegReload))
			g_srEnable = false;
		if (g_feat[kFtSlide] >= 0)
			g_slEnable = FeatOn(kFtSlide), g_slSeenEnable = true;
		if (FeatOff(kFtVmFov))
			g_vm.mode = kVmOff;
		if (g_feat[kFtIk] >= 0)
		{
			if (FeatOn(kFtIk))
				g_ikEnable = true;
			else
				IkResetConfig();
		}
	}

	// Distinct weapon names among the first n items (structs that start with char weapon[64]).
	template <typename T> int DistinctWeapons(const T *items, int n)
	{
		int distinct = 0;
		for (int i = 0; i < n; i++)
		{
			bool seen = false;
			for (int j = 0; j < i && !seen; j++)
				seen = _stricmp(items[i].weapon, items[j].weapon) == 0;
			distinct += !seen;
		}
		return distinct;
	}

	// One line: what each feature ended up with (after the gates), e.g.
	//   features: additives(42 guns) ammo_hide(18) kick(40) kick_return:on camera(1) locomotion(50, sway 30) ...
	void LogFeatureSummary()
	{
		char out[1024];
		size_t n = 0;
		auto add = [&](const char *fmt, auto... a) {
			if (n < sizeof(out))
				n += static_cast<size_t>(snprintf(out + n, sizeof(out) - n, fmt, a...));
		};
		auto onoff = [&](const char *name, bool on, int count) {
			if (!on)
				add(" %s:off", name);
			else if (count >= 0)
				add(" %s(%d)", name, count);
			else
				add(" %s:on", name);
		};
		int ammo = DistinctWeapons(g_ammoHides, g_ammoHideCount);
		for (int i = 0; i < g_hideOrderCount; i++)
		{
			bool seen = false;
			for (int j = 0; j < g_ammoHideCount && !seen; j++)
				seen = _stricmp(g_hideOrders[i].weapon, g_ammoHides[j].weapon) == 0;
			for (int j = 0; j < i && !seen; j++)
				seen = _stricmp(g_hideOrders[i].weapon, g_hideOrders[j].weapon) == 0;
			ammo += !seen;
		}
		const int additiveGuns = DistinctWeapons(g_additives, g_additiveCount);
		if (FeatOff(kFtAdditives))
			add(" additives:off");
		else
			add(" additives(%d guns%s)", additiveGuns, g_additiveEnable ? "" : ", additive_enable=0");
		onoff("ammo_hide", !FeatOff(kFtAmmoHide), ammo);
		onoff("kick", !FeatOff(kFtKick), g_wopCount);
		onoff("kick_return", g_wopKickReturn, -1);
		onoff("camera", !FeatOff(kFtCamera), g_camTuneCount);
		if (FeatOff(kFtLocomotion))
			add(" locomotion:off");
		else
			add(" locomotion(%d, sway %d%s)", g_locoCount, g_swayCount,
			    !g_locoEnable || !g_idleActiveEnable || !g_swayCfg.enable ? ", a switch off" : "");
		onoff("inspect", g_insEnable, g_insCount);
		add(" last_shot:%s(+%d)", g_el.def == kElIw ? "iw" : g_el.def == kElHold ? "hold" : "auto", g_el.count);
		onoff("empty_melee", g_emptyMeleeFix, -1);
		onoff("interrupts", g_intEnable, g_intCount);
		onoff("segreload", g_srEnable, g_srCount);
		onoff("slide", g_slEnable, -1);
		if (g_vm.mode == kVmPin)
			add(" vmfov:%.0f", g_vm.fov);
		else
			add(" vmfov:%s", g_vm.mode == kVmMw ? "mw" : "off");
		onoff("ik", g_ikEnable || (g_ikWeaponCount && g_ikDebug), g_ikWeaponCount);
		Log("weapon_tech: features (%s):%s", g_cfgSectioned ? "format v2, [features] applied" : "flat cfg", out);
	}

	// ---- cfg_dump=1 ---------------------------------------------------------------------------------------------------
	// The parsed state (after the [features] gates, before anything installs) as FNV-1a hashes of every config table and
	// switch: two cfgs that dump the same configure the DLL the same. Per-entry lines name the weapon, so a difference
	// can be found.
	uint64_t Fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull)
	{
		for (size_t i = 0; i < n; i++)
			h = (h ^ static_cast<const uint8_t *>(p)[i]) * 1099511628211ull;
		return h;
	}
	uint64_t FnvStr(const char *s, uint64_t h = 1469598103934665603ull) { return Fnv(s, strlen(s), h); }
	void WriteCfgStateDump(FILE *f, uint64_t *total)
	{
		uint64_t all = 1469598103934665603ull;
		auto table = [&](const char *name, const void *base, size_t stride, int count, auto nameOf) {
			if (count < 0)
				count = 0;
			uint64_t h = Fnv(base, stride * static_cast<size_t>(count));
			all = Fnv(&h, sizeof(h), all);
			fprintf(f, "%s count=%d hash=%016llx\n", name, count, static_cast<unsigned long long>(h));
			for (int i = 0; i < count; i++)
				fprintf(f, "  %s[%d] %s %016llx\n", name, i, nameOf(i),
				        static_cast<unsigned long long>(Fnv(static_cast<const uint8_t *>(base) + stride * i, stride)));
		};
		auto scalars = [&](const char *name, const void *p, size_t n) {
			uint64_t h = Fnv(p, n);
			all = Fnv(&h, sizeof(h), all);
			fprintf(f, "%s hash=%016llx\n", name, static_cast<unsigned long long>(h));
		};
#define WT_TABLE(arr, count, field) table(#arr, arr, sizeof(arr[0]), count, [&](int i) -> const char * { return arr[i].field; })
#define WT_NAMES(arr, count) table(#arr, arr, sizeof(arr[0]), count, [&](int i) -> const char * { return arr[i]; })
		WT_TABLE(g_additives, g_additiveCount, weapon);
		WT_NAMES(g_slotsTakeJukes, g_slotsTakeJukeCount);
		WT_TABLE(g_ammoHides, g_ammoHideCount, weapon);
		WT_TABLE(g_hideOrders, g_hideOrderCount, weapon);
		WT_NAMES(g_ammoHideAutoOff, g_ammoHideAutoOffCount);
		WT_TABLE(g_el.o, g_el.count, weapon);
		WT_TABLE(g_wops, g_wopCount, weapon);
		WT_TABLE(g_camTunes, g_camTuneCount, weapon);
		{
			// LocoWeapon is assigned from a temporary (padding = stack bytes): hashed member by member
			uint64_t h = 1469598103934665603ull;
			for (int i = 0; i < g_locoCount; i++)
			{
				const LocoWeapon &w = g_locos[i];
				uint64_t e = FnvStr(w.weapon);
				const int iv[] = {w.walk, w.jog, w.idle, w.walkStrides, w.jogStrides, static_cast<int>(w.jogNode),
				                  static_cast<int>(w.idleNode), w.idleRefused};
				const float fv[] = {w.walkRate, w.jogRate, w.jogWeight, w.idleWeight, w.idleRate};
				e = Fnv(iv, sizeof(iv), Fnv(fv, sizeof(fv), FnvStr(w.idleXanim, e)));
				fprintf(f, "  g_locos[%d] %s %016llx\n", i, w.weapon, static_cast<unsigned long long>(e));
				h = Fnv(&e, sizeof(e), h);
			}
			all = Fnv(&h, sizeof(h), all);
			fprintf(f, "g_locos count=%d hash=%016llx\n", g_locoCount, static_cast<unsigned long long>(h));
		}
		WT_TABLE(g_locoAliases, g_locoAliasCount, weapon);
		WT_TABLE(g_sways, g_swayCount, weapon);
		WT_TABLE(g_swayAliases, g_swayAliasCount, weapon);
		WT_TABLE(g_ikWeapons, g_ikWeaponCount, weapon);
		WT_TABLE(g_ikAliases, g_ikAliasCount, weapon);
		WT_TABLE(g_ikNotesCfg, g_ikNotesCfgCount, xanim);
		WT_TABLE(g_srEntries, g_srCount, weapon);
		WT_TABLE(g_srAliases, g_srAliasCount, weapon);
		WT_TABLE(g_insEntries, g_insCount, weapon);
		WT_TABLE(g_insAliases, g_insAliasCount, weapon);
		WT_TABLE(g_intEntries, g_intCount, weapon);
		WT_TABLE(g_intAliases, g_intAliasCount, weapon);
		WT_NAMES(g_slOff, g_slOffCount);
#undef WT_TABLE
#undef WT_NAMES
		struct
		{
			bool additiveEnable, additiveMeleeFade, additiveNamesOnly, beltDump, emptyMeleeFix, slotsTakeAll, slotsDebug, slotsDump;
			bool ammoHideAuto, rgEnable, ammoHideReload, wopKickReturn, kickConstsIw8, locoEnable, idleActiveEnable;
			bool idleFadeLinear, locoDebug, ikEnable, ikDebug, ikAlways, srEnable, insEnable, insHideHud, insDebugEmpty;
			bool insAkimbo, intEnable, intDebug, intTrace, intEmptyMelee, slEnable, slSeenEnable;
			bool perfEventHooks, perfCfgWatch, hotLog, perfTiming;
			int rgDebug, elDef, insKey, insEmpty, perfPollMs, camPitchUp;
			float rgPark, wopForceYaw, idleBlend, ikBlend, camFree, camShake[3], wopDebug[5][3];
		} sw;
		memset(&sw, 0, sizeof(sw));  // padding included: the struct is hashed as bytes
		sw.additiveEnable = g_additiveEnable, sw.additiveMeleeFade = g_additiveMeleeFade, sw.additiveNamesOnly = g_additiveNamesOnly;
		sw.beltDump = g_beltDump, sw.emptyMeleeFix = g_emptyMeleeFix, sw.slotsTakeAll = g_slotsTakeAllJukes;
		sw.slotsDebug = g_slotsDebug, sw.slotsDump = g_slotsDump, sw.ammoHideAuto = g_ammoHideAuto, sw.rgEnable = g_rgEnable;
		sw.ammoHideReload = g_ammoHideReload, sw.wopKickReturn = g_wopKickReturn, sw.kickConstsIw8 = g_iw8kc == &kIw8KickReal;
		sw.locoEnable = g_locoEnable, sw.idleActiveEnable = g_idleActiveEnable, sw.idleFadeLinear = g_idleFadeLinear;
		sw.locoDebug = g_locoDebug, sw.ikEnable = g_ikEnable, sw.ikDebug = g_ikDebug, sw.ikAlways = g_ikAlways;
		sw.srEnable = g_srEnable, sw.insEnable = g_insEnable, sw.insHideHud = g_insHideHud, sw.insDebugEmpty = g_insDebugEmpty;
		sw.insAkimbo = g_insAkimboCfg, sw.intEnable = g_intEnable, sw.intDebug = g_intDebug, sw.intTrace = g_intTrace;
		sw.intEmptyMelee = g_intEmptyMelee, sw.slEnable = g_slEnable;  // not slide_enable's 'seen' flag: only a log line reads it
		sw.perfEventHooks = g_perfEventHooks, sw.perfCfgWatch = g_perfCfgWatch, sw.hotLog = g_hotLog, sw.perfTiming = g_perfTiming;
		sw.rgDebug = g_rgDebug, sw.elDef = g_el.def, sw.insKey = g_insKey, sw.insEmpty = g_insEmpty, sw.perfPollMs = g_perfPollMs;
		sw.camPitchUp = g_camPitchUp, sw.rgPark = g_rgPark, sw.wopForceYaw = g_wopForceYaw, sw.idleBlend = g_idleBlend;
		sw.ikBlend = g_ikBlend, sw.camFree = g_camFree;
		memcpy(sw.camShake, g_camShake, sizeof(sw.camShake));
		memcpy(sw.wopDebug, g_wopDebug, sizeof(sw.wopDebug));
		scalars("switches", &sw, sizeof(sw));
		fprintf(f, "  additive_enable %d melee_fade %d empty_melee_fix %d ammohide_auto %d guard %d kickreturn %d kick_consts %s "
		           "locomotion_enable %d idle_active_enable %d sway_enable %d ik_enable %d ik_debug %d segreload_enable %d "
		           "inspect_enable %d key %d interrupt_enable %d interrupt_empty_melee %d slide_enable %d last_shot %d perf_hotlog %d\n",
		        sw.additiveEnable, sw.additiveMeleeFade, sw.emptyMeleeFix, sw.ammoHideAuto, sw.rgEnable, sw.wopKickReturn,
		        sw.kickConstsIw8 ? "iw8" : "legacy", sw.locoEnable, sw.idleActiveEnable, g_swayCfg.enable, sw.ikEnable, sw.ikDebug,
		        sw.srEnable, sw.insEnable, sw.insKey, sw.intEnable, sw.intEmptyMelee, sw.slEnable, sw.elDef, sw.hotLog);
		scalars("loco_jog", &g_jogParams, sizeof(g_jogParams));
		{
			const SwayGlobals &g = g_swayCfg;  // member by member (assigned from temporaries: padding = stack bytes)
			uint64_t h = 1469598103934665603ull;
			const int iv[] = {g.enable, g.parts[0], g.parts[1], g.parts[2], g.parts[3], g.parts[4], g.smoothing, g.torsoSpring,
			                  g.gunDir, g.gunSpring, g.hasGraph[0], g.hasGraph[1], g.debug, g.gameClock, g.frameId, g.camLead,
			                  g.graph[0].n, g.graph[1].n};
			const float fv[] = {g.scale[0], g.scale[1], g.scale[2], g.scale[3], g.smoothClamp, g.gunSign, g.gunBobMax, g.gunBobIn,
			                    g.gunBobOut, g.bobLag, g.idleFwdTime, g.idleFwdMag, g.idleViewTime[0], g.idleViewTime[1],
			                    g.substepHz, g.bobHz};
			h = Fnv(fv, sizeof(fv), Fnv(iv, sizeof(iv), h));
			for (const SwayGraph &gr : g.graph)
				h = Fnv(gr.y, sizeof(float) * gr.n, Fnv(gr.x, sizeof(float) * gr.n, h));
			all = Fnv(&h, sizeof(h), all);
			fprintf(f, "sway_globals hash=%016llx\n", static_cast<unsigned long long>(h));
		}
		{
			const SlideSettings &s = g_slParse;
			uint64_t h = 1469598103934665603ull;
			const int iv[] = {s.inTimeMs, s.maxTimeMs, s.outTimeMs, s.inAirTimeMs, s.sprintDelayMs, s.viewBlendInMs,
			                  s.viewBlendOutMs, s.subsequentTime, s.lateJumpGraceMs, s.cameraTimeMs, s.cameraEase, s.viewInterp,
			                  s.adsEnds, s.sprintLock, s.camera, s.view, s.gesture, s.snapRound, s.debug,
			                  static_cast<int>(s.nodes[0]), static_cast<int>(s.nodes[1]), static_cast<int>(s.nodes[2]),
			                  s.gestureRef, s.swapNative};
			const float fv[] = {s.inMaxSpeedScale, s.inAcceleration, s.fricNormal, s.fricDownhill, s.fricUphill, s.fricBlocked,
			                    s.outFricStart, s.outFricFinish, s.inAirFricStart, s.inAirFricFinish, s.strafeScale, s.jumpScale,
			                    s.sprintSpeedScale, s.crouchSpeedScale, s.minStartSpeed, s.minContinueSpeed, s.subsequentScale,
			                    s.cameraPitch, s.viewAngles[0], s.viewAngles[1], s.viewAngles[2], s.stopspeed, s.friction,
			                    s.blendIn, s.blendOut, s.blendCross, s.gestureWeight, s.gestureFps};
			h = FnvStr(s.suitName, Fnv(fv, sizeof(fv), Fnv(iv, sizeof(iv), h)));
			all = Fnv(&h, sizeof(h), all);
			fprintf(f, "slide hash=%016llx\n", static_cast<unsigned long long>(h));
		}
		scalars("vmfov", &g_vm, sizeof(g_vm));
		fprintf(f, "state hash=%016llx\n", static_cast<unsigned long long>(all));
		if (total)
			*total = all;
	}

	// cfg_dump=1: the state dump and the normalised lines, next to weapon_tech.log.
	void DumpCfgState(const char *flatText)
	{
		char path[MAX_PATH];
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		char *slash = strrchr(path, '\\');
		if (!slash)
			return;
		strcpy_s(slash + 1, path + MAX_PATH - (slash + 1), "weapon_tech.cfgdump.txt");
		uint64_t hash = 0;
		if (FILE *f = fopen(path, "w"))
		{
			WriteCfgStateDump(f, &hash);
			fclose(f);
		}
		strcpy_s(slash + 1, path + MAX_PATH - (slash + 1), "weapon_tech.cfgnorm.txt");
		if (FILE *f = fopen(path, "w"))
		{
			fputs(flatText, f);
			fclose(f);
		}
		Log("weapon_tech: cfg_dump: parsed state hash %016llx (weapon_tech.cfgdump.txt; the lines read: weapon_tech.cfgnorm.txt)",
		    static_cast<unsigned long long>(hash));
	}

	// After the parse: the [features] gates, the summary line, cfg_dump.
	void FinishWeaponTechConfig(const char *flatText)
	{
		ApplyFeatureGates();
		LogFeatureSummary();
		if (g_cfgDump)
			DumpCfgState(flatText);
	}

	// Reads the cfg from the first source found. dllDir: the folder weapon_tech.dll was loaded from.
	bool LoadWeaponTechConfig(const char *dllDir)
	{
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\weapon_tech.cfg", dllDir);
		if (char *text = ReadCfgText(path))
		{
			g_wtSource = CfgSource::File;
			strcpy_s(g_wtSourceName, path);
			// Live tuning (bo3_additive.h ReloadAdditiveTuning / bo3_perf.h's watcher): edits to this file while the game
			// runs are picked up for the keys audit\CONFIG_REFERENCE.md marks live (only once an anim-hook feature has
			// installed: WeaponTechInit says so). This records the file's time.
			strcpy_s(g_cfgPath, path);
			ReloadAdditiveTuning();
			ParseWeaponTechText(text);
			FinishWeaponTechConfig(text);
			free(text);
			Log("weapon_tech: config from the loose file %s (%d lines, %d bad, %d unknown key line(s)); live tuning: on if an "
			    "anim-hook feature installs", path, g_wtLines, g_wtBadLines, g_wtUnknownLines);
			return true;
		}
		int copies;
		if (char *text = ReadBakedCfg(&copies))
		{
			g_wtSource = CfgSource::Rawfile;
			snprintf(g_wtSourceName, sizeof(g_wtSourceName), "rawfile %s", kWeaponTechRawCfg);
			ParseWeaponTechText(text);
			FinishWeaponTechConfig(text);
			Log("weapon_tech: config from the %s baked into the map (%zu bytes, %d lines, %d bad, %d unknown key line(s)%s); no "
			    "loose %s, so no live tuning", g_wtSourceName, strlen(text), g_wtLines, g_wtBadLines, g_wtUnknownLines,
			    copies > 1 ? ", first of several loaded copies" : "", path);
			free(text);
			return true;
		}
		Log("weapon_tech: no config: no loose %s and no rawfile '%s' in the loaded zones. Nothing to install", path,
		    kWeaponTechRawCfg);
		return false;
	}

	// The whole init, once per process (the Lua calls it again on every level load). Returns whether the weapon tech is
	// active (the viewmodel hook is in).
	bool WeaponTechInit(const char *dllDir)
	{
		if (g_wtInitialised)
			return g_wtActive;
		g_wtInitialised = true;
		Log("weapon_tech loaded from %s", dllDir);
		if (!LoadWeaponTechConfig(dllDir))
			return false;
		if (!KnownBuild())
		{
			Log("weapon_tech: unrecognised BlackOps3.exe build; nothing installed");
			return false;
		}
		Log("weapon_tech: %s build at %p", g_build->name, g_base);
		// Every address in the weapon headers is pinned to one exe image: Enhanced (the IDB's) or retail (bo3_retail.h).
		// Any other image (SizeOfImage + PE TimeDateStamp): nothing is patched, so Arxan isn't touched either.
		const WtExe exe = DetectWtExe();
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_base + reinterpret_cast<const IMAGE_DOS_HEADER *>(g_base)->e_lfanew);
		const DWORD imageSize = nt->OptionalHeader.SizeOfImage, stamp = nt->FileHeader.TimeDateStamp;
		if (exe == WtExe::Unknown)
		{
			Log("weapon_tech: unsupported exe ('%s' build, SizeOfImage 0x%lX, TimeDateStamp 0x%lX); only Enhanced (0x%lX, 0x%lX) and "
			    "retail (0x%lX, 0x%lX) have addresses: nothing installed (Arxan not touched)", g_build->name, imageSize, stamp,
			    kEnhancedImageSize, kEnhancedStamp, kRetailImageSize, kRetailStamp);
			return false;
		}
		if (exe == WtExe::Retail)
			ApplyRetailAddresses();
		Log("weapon_tech: image is the %s exe (SizeOfImage 0x%lX, TimeDateStamp 0x%lX)%s", WtExeName(), imageSize, stamp,
		    exe == WtExe::Retail ? "; retail addresses applied" : "");
		// An Arxan-protected build kills the game ~20 s after a code patch unless its checks are neutralised first
		// (bo3_arxan.h; once per process, shared with stub_boot.dll).
		if (g_build->arxan && !WtDebugSkip("arxan") && !NeutraliseArxan())
		{
			Log("arxan: not neutralised; weapon tech not installed");
			return false;
		}
		InstallAdditives();
		InstallSegReload();  // independent of the viewmodel hook (bo3_segreload.h)
		InstallInspect();    // MW-style inspect (bo3_inspect.h)
		InstallInterrupt();  // IW-style raise / reload / rechamber interrupts (bo3_interrupt.h)
		InstallSlide();      // MW2019 slide, only with slide_enable=1 (bo3_slide.h)
		InstallVmFov();      // viewmodel FOV pin, only with vmfov= on (bo3_vmfov.h); after InstallAdditives: chains its gun hook
		g_wtActive = g_additiveHooked;
		if (g_wtSource == CfgSource::File && !g_additiveHooked)
			Log("weapon_tech: live tuning OFF after all: the anim hook isn't in (no anim-hook feature configured, or it "
			    "failed: see above; segreload / inspect / interrupt are read once)");
		Log("weapon_tech: %s", g_wtActive ? "ACTIVE (viewmodel hook in)" : "NOT active (see above)");
		return g_wtActive;
	}
}
