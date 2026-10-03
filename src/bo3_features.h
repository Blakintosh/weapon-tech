// [features] master switches (cfg format v2, wt_cfgv2.h). The normaliser turns each "name = value" line of the
// [features] section into "feature=<name>,<value>"; ParseFeatureLine records it here. weapon_tech.h applies the switches
// once the whole cfg is parsed (ApplyFeatureGates): a feature that is off has its tables cleared and its switch set to
// off, so it installs nothing even when its lines are in the file; on sets an opt-in feature's switch (inspect_enable,
// slide_enable, ik_enable, empty_melee_fix, wop_kickreturn). A feature [features] doesn't name keeps the old keys'
// behaviour, so a file without [features] works exactly as before. [features] is read at start; the live reloads keep
// an off feature off (ApplyTuningText, IkReloadText, VmFovReloadText).
#pragma once
#include "wt_cfgv2.h"

namespace
{
	enum WtFeature
	{
		kFtAdditives, kFtAmmoHide, kFtKick, kFtKickReturn, kFtCamera, kFtLocomotion, kFtInspect, kFtLastShot,
		kFtEmptyMelee, kFtInterrupts, kFtSegReload, kFtSlide, kFtVmFov, kFtIk, kFtCount
	};
	const char *const kFtNames[kFtCount] = {"additives", "ammo_hide", "kick", "kick_return", "camera", "locomotion", "inspect",
	                                        "last_shot", "empty_melee", "interrupts", "segreload", "slide", "vmfov", "ik"};
	int8_t g_feat[kFtCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // -1 not in [features], 0 off, 1 on
	char g_featValue[kFtCount][16];  // last_shot / vmfov: the value as written
	bool g_featGated;                // ApplyFeatureGates ran (the live reloads check FeatOff from then on)
	bool g_cfgSectioned;             // the cfg had [sections] (format v2)
	bool g_cfgDump;                  // cfg_dump=1: weapon_tech.cfgdump.txt (parsed state) + weapon_tech.cfgnorm.txt

	bool FeatOff(int f) { return g_feat[f] == 0; }
	bool FeatOn(int f) { return g_feat[f] == 1; }

	// feature=<name>,<value> (from [features]). Unknown names are logged and ignored.
	bool ParseFeatureLine(const char *line, bool &known)
	{
		known = strncmp(line, "feature=", 8) == 0;
		if (!known)
			return true;
		char name[32] = {}, value[32] = {};
		if (sscanf_s(line + 8, "%31[^,],%31[^ \t#]", name, static_cast<unsigned>(sizeof(name)), value,
		             static_cast<unsigned>(sizeof(value))) != 2)
			return false;
		// a few spellings people will use
		static const char *const kAlias[][2] = {{"additive", "additives"}, {"ammohide", "ammo_hide"}, {"interrupt", "interrupts"},
		                                        {"kickreturn", "kick_return"}, {"lastshot", "last_shot"}, {"emptymelee", "empty_melee"},
		                                        {"empty_lastshot", "last_shot"}, {"loco", "locomotion"}, {"sway", "locomotion"}};
		for (const auto &a : kAlias)
			if (!_stricmp(name, a[0]))
				strcpy_s(name, a[1]);
		for (int f = 0; f < kFtCount; f++)
		{
			if (_stricmp(name, kFtNames[f]) != 0)
				continue;
			strcpy_s(g_featValue[f], value);
			const int m = !_stricmp(value, "on") ? 1 : !_stricmp(value, "off") ? 0 : -1;
			if (f == kFtLastShot)  // a mode: auto / iw / hold (the normaliser also emitted it as empty_lastshot=)
				return g_feat[f] = 1, m >= 0 || !_stricmp(value, "auto") || !_stricmp(value, "iw") || !_stricmp(value, "hold");
			if (f == kFtVmFov)  // off / mw / <deg> (also emitted as vmfov=)
				return g_feat[f] = static_cast<int8_t>(m == 0 ? 0 : 1), true;
			if (m < 0)
				return false;
			g_feat[f] = static_cast<int8_t>(m);
			return true;
		}
		Log("weapon_tech: [features] has no feature '%s' (ignored; the features are: additives ammo_hide kick kick_return "
		    "camera locomotion inspect last_shot empty_melee interrupts segreload slide vmfov ik)", name);
		return true;
	}

	// What the normaliser reported (unknown sections, warnings). Logged in full once; later reloads only when it changes.
	void LogCfgNormalise(const WtcInfo &info, const char *source)
	{
		static int s_lastWarn = -1, s_lastUnknown = -1;
		if (info.warnCount == s_lastWarn && info.unknownCount == s_lastUnknown)
			return;
		s_lastWarn = info.warnCount;
		s_lastUnknown = info.unknownCount;
		if (info.unknownCount)
			Log("weapon_tech: %s: %d unknown section(s): %s (their lines are read with the keys as written)", source,
			    info.unknownCount, info.unknown);
		if (info.warnCount)
		{
			Log("weapon_tech: %s: %d cfg format warning(s):", source, info.warnCount);
			const char *p = info.warn;
			while (*p)
			{
				size_t n = strcspn(p, "\n");
				Log("weapon_tech:   %.*s", static_cast<int>(n), p);
				p += n + (p[n] == '\n');
			}
		}
	}

	// A cfg's text in the form every parser reads: a sectioned (v2) file is normalised to flat lines (wt_cfgv2.h); a flat
	// file is returned as it is. Takes ownership of `raw` (malloc'd); returns malloc'd text.
	char *NormaliseCfgText(char *raw, const char *source)
	{
		if (!raw)
			return nullptr;
		WtcInfo info;
		char *flat = wtc_normalise(raw, &info);
		if (!flat)
			return raw;
		free(raw);
		g_cfgSectioned = true;
		LogCfgNormalise(info, source);
		return flat;
	}
}
