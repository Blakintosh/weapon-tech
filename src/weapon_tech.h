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
		return text;
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
