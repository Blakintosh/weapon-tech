/* weapon_tech.cfg format v2: the sectioned layout, normalised to the flat key=value lines every parser reads.
 *
 * Plain C (C99 / C++17 alike), no engine and no allocation beyond malloc: weapon_tech.dll includes it (bo3_additive.h:
 * ReadCfgText runs every cfg text through it, the live reloads included) and so does the weapontech linker feature
 * (LinkerDump\hook\features\weapontech_check.c), so both read the same lines.
 *
 * A file with no [section] header is returned untouched (wtc_normalise gives NULL): the old flat format, exactly as
 * before. Otherwise:
 *   - lines before the first [section] pass through as they are (old flat lines);
 *   - [features]   name = on|off (last_shot = auto|iw|hold, vmfov = off|mw|<deg>) -> "feature=<name>,<value>"
 *                  (the DLL's gates, bo3_features.h); last_shot and vmfov values also emit empty_lastshot= / vmfov=;
 *   - [<feature>]  key = value -> <full key>=<value>. A key may drop its section's prefix (key in [inspect] =
 *                  inspect_key); full key names are accepted everywhere. A guns list (guns = a, b:6.6, ...) expands to
 *                  one line per gun: see kWtcLists. Several guns lines add up;
 *   - [weapon:<name>] the per-weapon form a GDT compiler emits: wt* keys named as in gdt_schema\DESIGN_weapontech_gdt.md
 *                  (kWtcTails / the composites below), or any per-weapon cfg key without the weapon (wop = 1,0,2,...).
 *                  Lines for that weapon there replace its entries in the feature sections' guns lists.
 *   - "# ==== BEGIN generated:<tool> ====" ... "# ==== END generated:<tool> ====" fence a generator's block; the
 *     section in force before BEGIN is restored at END, so a block can open its own [section].
 * Comments (#, //) and blank lines inside sections are dropped; a value keeps its trailing "# comment" (the parsers
 * ignore it), except in [features] and guns lists. Lines are read 255 characters at a time, as the parsers do. */
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  /* strcat / strcpy: every use is bounds-checked */
#endif

typedef struct
{
	int sectioned;     /* the text had [section] headers */
	int unknownCount;  /* unknown section names, comma separated in unknown[] */
	char unknown[512];
	int warnCount;     /* warnings, '\n' separated in warn[] (the first ones) */
	char warn[2048];
	int lines;         /* canonical lines emitted */
} WtcInfo;

typedef struct
{
	char *p;
	size_t n, cap;
} WtcBuf;

static void wtc_put(WtcBuf *b, const char *s, size_t n)
{
	if (b->n + n + 2 > b->cap)
	{
		size_t cap = b->cap ? b->cap * 2 : 1 << 16;
		while (cap < b->n + n + 2)
			cap *= 2;
		char *q = (char *)realloc(b->p, cap);
		if (!q)
			return;
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}
static void wtc_puts(WtcBuf *b, const char *s) { wtc_put(b, s, strlen(s)); }

static void wtc_warn(WtcInfo *info, const char *fmt, const char *a, const char *b)
{
	char msg[384];
	snprintf(msg, sizeof(msg), fmt, a ? a : "", b ? b : "");
	info->warnCount++;
	size_t have = strlen(info->warn);
	if (have + strlen(msg) + 2 < sizeof(info->warn))
	{
		if (have)
			strcat(info->warn, "\n");
		strcat(info->warn, msg);
	}
}

/* One canonical line: key=value\n (value may be empty). */
static void wtc_line(WtcBuf *out, WtcInfo *info, const char *key, const char *value)
{
	char line[1024];
	int n = snprintf(line, sizeof(line), "%s=%s", key, value);
	if (n > 255)
		wtc_warn(info, "line longer than 255 characters after normalising (the parsers split it): '%.200s'", line, NULL);
	wtc_puts(out, line);
	wtc_puts(out, "\n");
	info->lines++;
}

static char *wtc_trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = 0;
	return s;
}

static int wtc_ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	return *a == *b;
}

/* key == fam, or key starts with fam + "_" */
static int wtc_family(const char *key, const char *fam)
{
	size_t n = strlen(fam);
	return strncmp(key, fam, n) == 0 && (key[n] == 0 || key[n] == '_');
}

/* Every key family the parsers own: a key in one of these is a full name in any section. */
static const char *const kWtcFamilies[] = {
    "additive", "slots", "belt_dump", "ammohide", "wop", "cam_shake", "camera_free", "locomotion", "idle_active", "sway",
    "inspect", "empty_lastshot", "empty_melee_fix", "additive_melee_fade", "interrupt", "segreload", "slide", "vmfov",
    "ik", "perf", "cfg_dump", "feature"};

typedef struct
{
	const char *name;    /* [name] */
	const char *prefix;  /* short keys get prefix + "_"; NULL: keys are kept as written */
} WtcSection;
static const WtcSection kWtcSections[] = {
    {"features", NULL},   {"general", NULL},       {"additives", "additive"}, {"ammo_hide", "ammohide"},
    {"kick", "wop"},      {"camera", NULL},        {"locomotion", "locomotion"}, {"inspect", "inspect"},
    {"last_shot", "empty_lastshot"}, {"empty_melee", NULL}, {"interrupts", "interrupt"}, {"segreload", "segreload"},
    {"slide", "slide"},   {"vmfov", "vmfov"},      {"ik", "ik"}};

/* Short names that aren't prefix + "_" + key. */
static const char *const kWtcAliases[][3] = {
    {"camera", "shake", "cam_shake"},          {"camera", "free", "camera_free"},
    {"inspect", "hide_hud", "inspect_hidehud"}, {"last_shot", "default", "empty_lastshot"},
    {"empty_melee", "fix", "empty_melee_fix"},  {"empty_melee", "raise", "interrupt_empty_melee"},
    {"empty_melee", "additive_fade", "additive_melee_fade"}, {"vmfov", "mode", "vmfov"},
    {"additives", "take_jukes_all", "slots_take_jukes"}};

/* guns lists: [section] listkey = a, b:p1:p2, ... -> cfgkey=<gun>,... */
enum { WTC_ONOFF, WTC_MODE, WTC_FIXED0, WTC_BARE };
typedef struct
{
	const char *section, *listkey, *cfgkey;
	int kind;               /* WTC_ONOFF: <gun>,<1|0>[,params]; WTC_MODE: <gun>,<param> (def when bare); WTC_FIXED0: <gun>,0;
	                           WTC_BARE: <gun> */
	const char *def;        /* WTC_MODE: the mode of a bare gun (NULL: required) */
	const char *wkey;       /* the group a [weapon:] section overrides it with (pre-scan tag) */
} WtcList;
static const WtcList kWtcLists[] = {
    {"inspect", "guns", "inspect", WTC_ONOFF, NULL, "inspect"},
    {"ik", "guns", "ik", WTC_ONOFF, NULL, "ik"},
    {"segreload", "guns", "segreload_empty", WTC_MODE, "end", "segreload"},
    {"last_shot", "guns", "empty_lastshot", WTC_MODE, NULL, "last_shot"},
    {"ammo_hide", "auto_off", "ammohide_auto", WTC_FIXED0, NULL, "ammohide_auto"},
    {"additives", "take_jukes", "slots_take_jukes", WTC_BARE, NULL, "take_jukes"}};

/* [weapon:<w>] keys whose value is the cfg line's tail: wtKey = v -> cfgkey=<w>,<pre><v>. A '#' at the end of the name
 * = numbered (wtKick1..wtKick24); they keep their file order. group: the mode key that can switch them off. */
enum { WG_NONE, WG_RECOIL, WG_SWAY, WG_LOCO, WG_INTR };
typedef struct
{
	const char *wt, *cfgkey, *pre;
	int group;
	const char *tag;  /* pre-scan tag (guns-list override) or NULL */
} WtcTail;
static const WtcTail kWtcTails[] = {
    {"wtFireTimeMs", "wop_weapon", "", WG_RECOIL, NULL},
    {"wtKickPct", "wop_kickpct", "", WG_RECOIL, NULL},
    {"wtKick#", "wop_kick", "", WG_RECOIL, NULL},
    {"wtSpringViewHip", "wop_spring", "0,0,", WG_RECOIL, NULL},
    {"wtSpringViewAds", "wop_spring", "0,1,", WG_RECOIL, NULL},
    {"wtSpringGunHip", "wop_spring", "1,0,", WG_RECOIL, NULL},
    {"wtSpringGunAds", "wop_spring", "1,1,", WG_RECOIL, NULL},
    {"wtTilt", "wop_tilt", "", WG_RECOIL, NULL},
    {"wtWopCurveHoldSlow", "wop_curve", "0,", WG_RECOIL, NULL},
    {"wtWopCurveHoldFast", "wop_curve", "1,", WG_RECOIL, NULL},
    {"wtWopCurveKick", "wop_curve", "2,", WG_RECOIL, NULL},
    {"wtWopCurveSnapDecay", "wop_curve", "3,", WG_RECOIL, NULL},
    {"wtWopCurveAds", "wop_curve", "4,", WG_RECOIL, NULL},
    {"wtWopCurveAlwaysOn", "wop_curve", "5,", WG_RECOIL, NULL},
    {"wtWop#", "wop", "", WG_RECOIL, NULL},
    {"wtCameraFree", "camera_free", "", WG_NONE, NULL},
    {"wtSwayAdv", "sway_adv", "", WG_SWAY, NULL},
    {"wtSwayAdvGun", "sway_advgun", "", WG_SWAY, NULL},
    {"wtSwayAdvFire", "sway_advfire", "", WG_SWAY, NULL},
    {"wtSwayIdle1", "sway_idle", "1,", WG_SWAY, NULL},
    {"wtSwayIdle2", "sway_idle", "2,", WG_SWAY, NULL},
    {"wtSwayIdleMisc", "sway_idlemisc", "", WG_SWAY, NULL},
    {"wtSwayStance", "sway_stance", "", WG_SWAY, NULL},
    {"wtSwayAdsBob", "sway_adsbob", "", WG_SWAY, NULL},
    {"wtSwayGraphDeadzone", "sway_graph", "0,", WG_SWAY, NULL},
    {"wtSwayGraphGun", "sway_graph", "1,", WG_SWAY, NULL},
    {"wtAdditiveSlot#", "additive", "", WG_NONE, NULL},
    {"wtAmmoHide", "ammohide", "", WG_NONE, NULL},
    {"wtAmmoHideOrder", "ammohide_order", "", WG_NONE, NULL},
    {"wtAmmoHideSpend", "ammohide_spend", "", WG_NONE, NULL},
    {"wtAmmoHideReverse", "ammohide_reverse", "", WG_NONE, NULL},
    {"wtSegReloadEmpty", "segreload_empty", "", WG_NONE, "segreload"},
    {"wtInterrupt#", "interrupt", "", WG_INTR, NULL},
    {"wtEmptyLastShot", "empty_lastshot", "", WG_NONE, "last_shot"}};

/* [weapon:<w>] keys that build one cfg line together (emitted after the tails, in this order). */
enum
{
	CF_SRC, CF_KR_ON, CF_KR_MAINT, CF_KR_NODAMP, CF_CAM_A, CF_CAM_R, CF_CAM_O, CF_CAM_PU, CF_WALK_N, CF_WALK_RATE,
	CF_JOG_LEAF, CF_JOG_W, CF_JOG_RATE, CF_JOG_N, CF_IA_ANIM, CF_IA_LEAF, CF_IA_W, CF_IA_RATE, CF_AE_ANIM, CF_AE_ROOT,
	CF_AE_W, CF_AR_ANIM, CF_AR_ROOT, CF_AR_W, CF_AR_RATE, CF_AB_ANIM, CF_AB_ROOT, CF_AB_W, CF_AB_MAG, CF_IK_ON,
	CF_IK_HANDS, CF_IK_W, CF_IK_ORIENT, CF_INS_ON, CF_INS_T, CF_AUTOHIDE, CF_RECOIL, CF_SWAY, CF_LOCO, CF_INTR, CF_COUNT
};
static const char *const kWtcComposite[CF_COUNT] = {
    "wtSource", "wtKickReturn", "wtKickMaintain", "wtKickNoDampening", "wtCamShakeAngles", "wtCamShakeRoll",
    "wtCamShakeOrigin", "wtCamShakePitchUp", "wtLocoWalkStrides", "wtLocoWalkRate", "wtLocoJogLeaf", "wtLocoJogWeight",
    "wtLocoJogRate", "wtLocoJogStrides", "wtIdleActiveAnim", "wtIdleActiveLeaf", "wtIdleActiveWeight", "wtIdleActiveRate",
    "wtAdditiveEmptyAnim", "wtAdditiveEmptyRoot", "wtAdditiveEmptyWeight", "wtAdditiveRecoilAnim", "wtAdditiveRecoilRoot",
    "wtAdditiveRecoilWeight", "wtAdditiveRecoilRate", "wtAdditiveBulletAnim", "wtAdditiveBulletRoot",
    "wtAdditiveBulletWeight", "wtAdditiveBulletMag", "wtIk", "wtIkHands", "wtIkNotelessWeight", "wtIkOrient", "wtInspect",
    "wtInspectTime", "wtAmmoHideAuto", "wtRecoil", "wtSway", "wtLoco", "wtInterrupt"};

/* -1 not a mode word, 0 off, 1 on */
static int wtc_mode(const char *v)
{
	if (wtc_ieq(v, "on") || wtc_ieq(v, "1") || wtc_ieq(v, "true") || wtc_ieq(v, "yes"))
		return 1;
	if (wtc_ieq(v, "off") || wtc_ieq(v, "0") || wtc_ieq(v, "false") || wtc_ieq(v, "no"))
		return 0;
	return -1;
}

/* value up to an inline "#" comment, trimmed (in place) */
static char *wtc_nocomment(char *v)
{
	char *h = strchr(v, '#');
	if (h)
		*h = 0;
	return wtc_trim(v);
}

static const WtcSection *wtc_section(const char *name)
{
	for (size_t i = 0; i < sizeof(kWtcSections) / sizeof(kWtcSections[0]); i++)
		if (wtc_ieq(kWtcSections[i].name, name))
			return &kWtcSections[i];
	return NULL;
}

/* The full key for `key` written in section `sec` (sec NULL: kept). */
static void wtc_fullkey(const WtcSection *sec, const char *key, char *out, size_t cap)
{
	snprintf(out, cap, "%s", key);
	if (!sec)
		return;
	for (size_t i = 0; i < sizeof(kWtcAliases) / sizeof(kWtcAliases[0]); i++)
		if (wtc_ieq(kWtcAliases[i][0], sec->name) && strcmp(kWtcAliases[i][1], key) == 0)
		{
			snprintf(out, cap, "%s", kWtcAliases[i][2]);
			return;
		}
	if (!sec->prefix)
		return;
	for (size_t i = 0; i < sizeof(kWtcFamilies) / sizeof(kWtcFamilies[0]); i++)
		if (wtc_family(key, kWtcFamilies[i]))
			return;
	snprintf(out, cap, "%s_%s", sec->prefix, key);
}

/* ---- [weapon:<w>] --------------------------------------------------------------------------------------------- */
typedef struct
{
	char name[64];
	int active;
	char *comp[CF_COUNT];  /* malloc'd values, NULL = unset */
	WtcBuf tails;          /* key=value lines, in file order */
} WtcWeapon;

static void wtc_weapon_reset(WtcWeapon *w)
{
	for (int i = 0; i < CF_COUNT; i++)
	{
		free(w->comp[i]);
		w->comp[i] = NULL;
	}
	free(w->tails.p);
	memset(&w->tails, 0, sizeof(w->tails));
	w->active = 0;
	w->name[0] = 0;
}

static const char *wtc_c(const WtcWeapon *w, int f, const char *def) { return w->comp[f] ? w->comp[f] : def; }

/* "1"/"0" from a mode value (absent = def) */
static const char *wtc_onoff(const WtcWeapon *w, int f, int def)
{
	if (!w->comp[f])
		return def ? "1" : "0";
	int m = wtc_mode(w->comp[f]);
	return m == 0 ? "0" : "1";
}
static int wtc_group_off(const WtcWeapon *w, int f) { return w->comp[f] && wtc_mode(w->comp[f]) == 0; }

static void wtc_weapon_flush(WtcWeapon *w, WtcBuf *out, WtcInfo *info)
{
	if (!w->active)
		return;
	char v[1024];
	const char *n = w->name;
	if (w->comp[CF_SRC])  /* first: the alias copies the source's wop block, so the weapon's own lines go on top */
	{
		snprintf(v, sizeof(v), "%s,%s", n, w->comp[CF_SRC]);
		wtc_line(out, info, "wop_alias", v);
	}
	if (w->tails.p)
	{
		wtc_puts(out, w->tails.p);
		for (const char *p = w->tails.p; *p; p++)
			info->lines += *p == '\n';
	}
	if (!wtc_group_off(w, CF_RECOIL) && (w->comp[CF_KR_ON] || w->comp[CF_KR_MAINT] || w->comp[CF_KR_NODAMP]))
	{
		if (w->comp[CF_KR_NODAMP])
			snprintf(v, sizeof(v), "%s,%s,%s,%s", n, wtc_onoff(w, CF_KR_ON, 1), wtc_c(w, CF_KR_MAINT, "0"),
			         wtc_onoff(w, CF_KR_NODAMP, 0));
		else if (w->comp[CF_KR_MAINT])
			snprintf(v, sizeof(v), "%s,%s,%s", n, wtc_onoff(w, CF_KR_ON, 1), w->comp[CF_KR_MAINT]);
		else
			snprintf(v, sizeof(v), "%s,%s", n, wtc_onoff(w, CF_KR_ON, 1));
		wtc_line(out, info, "wop_kickreturn", v);
	}
	if (w->comp[CF_CAM_A] || w->comp[CF_CAM_R] || w->comp[CF_CAM_O] || w->comp[CF_CAM_PU])
	{
		/* -1 = not set here (falls back to the alias source, then the global) */
		int k = snprintf(v, sizeof(v), "%s,%s,%s,%s", n, wtc_c(w, CF_CAM_A, "-1"), wtc_c(w, CF_CAM_R, "-1"),
		                 wtc_c(w, CF_CAM_O, "-1"));
		if (w->comp[CF_CAM_PU])
			snprintf(v + k, sizeof(v) - k, ",%s", wtc_onoff(w, CF_CAM_PU, 0));
		wtc_line(out, info, "cam_shake", v);
	}
	if (!wtc_group_off(w, CF_LOCO))
	{
		if (w->comp[CF_WALK_N] && atoi(w->comp[CF_WALK_N]) > 0)
		{
			int k = snprintf(v, sizeof(v), "%s,walk,bob,%s", n, w->comp[CF_WALK_N]);
			if (w->comp[CF_WALK_RATE])
				snprintf(v + k, sizeof(v) - k, ",%s", w->comp[CF_WALK_RATE]);
			wtc_line(out, info, "locomotion", v);
		}
		if (w->comp[CF_JOG_LEAF] && atoi(w->comp[CF_JOG_LEAF]) > 0)
		{
			int k = snprintf(v, sizeof(v), "%s,jog,%s", n, w->comp[CF_JOG_LEAF]);
			if (w->comp[CF_JOG_W] || w->comp[CF_JOG_RATE] || w->comp[CF_JOG_N])
				k += snprintf(v + k, sizeof(v) - k, ",%s", wtc_c(w, CF_JOG_W, "1"));
			if (w->comp[CF_JOG_RATE] || w->comp[CF_JOG_N])
				k += snprintf(v + k, sizeof(v) - k, ",%s", wtc_c(w, CF_JOG_RATE, "1"));
			if (w->comp[CF_JOG_N])
				snprintf(v + k, sizeof(v) - k, ",%s", w->comp[CF_JOG_N]);
			wtc_line(out, info, "locomotion", v);
		}
		if (w->comp[CF_IA_ANIM])
		{
			int k = snprintf(v, sizeof(v), "%s,%s,%s", n, w->comp[CF_IA_ANIM], wtc_c(w, CF_IA_LEAF, "194"));
			if (w->comp[CF_IA_W] || w->comp[CF_IA_RATE])
				k += snprintf(v + k, sizeof(v) - k, ",%s", wtc_c(w, CF_IA_W, "1"));
			if (w->comp[CF_IA_RATE])
				snprintf(v + k, sizeof(v) - k, ",%s", w->comp[CF_IA_RATE]);
			wtc_line(out, info, "idle_active", v);
		}
	}
	static const struct
	{
		const char *kind, *root;
		int anim, rootf, weight, extra;
	} kinds[] = {{"empty", "195", CF_AE_ANIM, CF_AE_ROOT, CF_AE_W, -1},
	             {"recoil", "195", CF_AR_ANIM, CF_AR_ROOT, CF_AR_W, CF_AR_RATE},
	             {"bullet", "193", CF_AB_ANIM, CF_AB_ROOT, CF_AB_W, CF_AB_MAG}};
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
	{
		if (!w->comp[kinds[i].anim] || !w->comp[kinds[i].anim][0])
			continue;
		const int hasExtra = kinds[i].extra >= 0 && w->comp[kinds[i].extra];
		int k = snprintf(v, sizeof(v), "%s,%s,%s,%s", n, kinds[i].kind, wtc_c(w, kinds[i].rootf, kinds[i].root),
		                 w->comp[kinds[i].anim]);
		if (w->comp[kinds[i].weight] || hasExtra)
			k += snprintf(v + k, sizeof(v) - k, ",%s", wtc_c(w, kinds[i].weight, "1"));
		if (hasExtra)
			snprintf(v + k, sizeof(v) - k, ",%s", w->comp[kinds[i].extra]);
		wtc_line(out, info, "additive", v);
	}
	if (w->comp[CF_IK_ON] || w->comp[CF_IK_HANDS] || w->comp[CF_IK_W] || w->comp[CF_IK_ORIENT])
	{
		int k = snprintf(v, sizeof(v), "%s,%s", n, wtc_onoff(w, CF_IK_ON, 1));
		if (w->comp[CF_IK_HANDS] || w->comp[CF_IK_W] || w->comp[CF_IK_ORIENT])
			k += snprintf(v + k, sizeof(v) - k, ",%s", wtc_c(w, CF_IK_HANDS, "lr"));
		if (w->comp[CF_IK_W] || w->comp[CF_IK_ORIENT])
			k += snprintf(v + k, sizeof(v) - k, ",%s", wtc_c(w, CF_IK_W, "1"));
		if (w->comp[CF_IK_ORIENT])
			snprintf(v + k, sizeof(v) - k, ",%s", wtc_onoff(w, CF_IK_ORIENT, 1));
		wtc_line(out, info, "ik", v);
	}
	if (w->comp[CF_INS_ON] || w->comp[CF_INS_T])
	{
		int k = snprintf(v, sizeof(v), "%s,%s", n, wtc_onoff(w, CF_INS_ON, 1));
		if (w->comp[CF_INS_T])
			snprintf(v + k, sizeof(v) - k, ",%s", w->comp[CF_INS_T]);
		wtc_line(out, info, "inspect", v);
	}
	if (w->comp[CF_AUTOHIDE])
	{
		snprintf(v, sizeof(v), "%s,%s", n, wtc_onoff(w, CF_AUTOHIDE, 1));
		wtc_line(out, info, "ammohide_auto", v);
	}
	wtc_weapon_reset(w);
}

/* wt key -> tail entry (numbered keys: the name's digits) */
static const WtcTail *wtc_tail(const char *key)
{
	for (size_t i = 0; i < sizeof(kWtcTails) / sizeof(kWtcTails[0]); i++)
	{
		const char *t = kWtcTails[i].wt;
		size_t n = strlen(t);
		if (t[n - 1] == '#')
		{
			if (strncmp(key, t, n - 1) == 0 && isdigit((unsigned char)key[n - 1]))
			{
				const char *d = key + n - 1;
				while (isdigit((unsigned char)*d))
					d++;
				if (!*d)
					return &kWtcTails[i];
			}
		}
		else if (strcmp(key, t) == 0)
			return &kWtcTails[i];
	}
	return NULL;
}

static int wtc_composite(const char *key)
{
	for (int i = 0; i < CF_COUNT; i++)
		if (strcmp(key, kWtcComposite[i]) == 0)
			return i;
	return -1;
}

/* One key = value inside [weapon:<w>]. */
static void wtc_weapon_key(WtcWeapon *w, const char *key, const char *value, WtcInfo *info)
{
	char line[1024];
	int c = wtc_composite(key);
	if (c >= 0)
	{
		free(w->comp[c]);
		size_t n = strlen(value) + 1;
		w->comp[c] = (char *)malloc(n);
		if (w->comp[c])
			memcpy(w->comp[c], value, n);
		return;
	}
	const WtcTail *t = wtc_tail(key);
	if (t)
	{
		const int groupMode[] = {-1, CF_RECOIL, CF_SWAY, CF_LOCO, CF_INTR};
		if (t->group != WG_NONE && wtc_group_off(w, groupMode[t->group]))
			return;
		snprintf(line, sizeof(line), "%s=%s,%s%s\n", t->cfgkey, w->name, t->pre, value);
		wtc_puts(&w->tails, line);
		return;
	}
	if (key[0] == 'w' && key[1] == 't' && isupper((unsigned char)key[2]))
	{
		wtc_warn(info, "[weapon:%s]: unknown key '%s' (ignored)", w->name, key);
		return;
	}
	/* any per-weapon cfg key, the weapon left out: wop = 1,0,2,... -> wop=<w>,1,0,2,... */
	snprintf(line, sizeof(line), "%s=%s,%s\n", key, w->name, value);
	wtc_puts(&w->tails, line);
}

/* group tags a [weapon:<w>] section sets (for the guns-list override) */
static int wtc_weapon_has_tag(const char *key, const char *tag)
{
	const WtcTail *t = wtc_tail(key);
	if (t && t->tag && strcmp(t->tag, tag) == 0)
		return 1;
	if (strcmp(tag, "inspect") == 0)
		return !strcmp(key, "wtInspect") || !strcmp(key, "wtInspectTime") || !strcmp(key, "inspect");
	if (strcmp(tag, "ik") == 0)
		return !strncmp(key, "wtIk", 4) || !strcmp(key, "ik");
	if (strcmp(tag, "segreload") == 0)
		return !strcmp(key, "segreload_empty");
	if (strcmp(tag, "last_shot") == 0)
		return !strcmp(key, "empty_lastshot");
	if (strcmp(tag, "ammohide_auto") == 0)
		return !strcmp(key, "wtAmmoHideAuto") || !strcmp(key, "ammohide_auto");
	if (strcmp(tag, "take_jukes") == 0)
		return !strcmp(key, "slots_take_jukes");
	return 0;
}

/* ---- reading ------------------------------------------------------------------------------------------------------ */
/* The next line as the parsers read it: through '\n', at most cap - 1 characters. NULL at the end. */
static const char *wtc_next(const char *p, char *line, size_t cap)
{
	if (!*p)
		return NULL;
	size_t n = 0;
	while (*p && n + 1 < cap)
	{
		char ch = *p++;
		line[n++] = ch;
		if (ch == '\n')
			break;
	}
	line[n] = 0;
	return p;
}

/* "[name]" -> name (in place), else NULL */
static char *wtc_header(char *s)
{
	s = wtc_trim(s);
	if (*s != '[')
		return NULL;
	char *e = strchr(s, ']');
	if (!e)
		return NULL;
	*e = 0;
	return wtc_trim(s + 1);
}

static int wtc_fence(const char *s, const char *what)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char head[64];
	snprintf(head, sizeof(head), "# ==== %s generated:", what);
	return strncmp(s, head, strlen(head)) == 0;
}

typedef struct
{
	char (*names)[96];  /* "tag\tweapon" */
	int n, cap;
} WtcTags;

static void wtc_tags_add(WtcTags *t, const char *tag, const char *weapon)
{
	if (t->n == t->cap)
	{
		int cap = t->cap ? t->cap * 2 : 64;
		char(*q)[96] = (char(*)[96])realloc(t->names, sizeof(*q) * cap);
		if (!q)
			return;
		t->names = q;
		t->cap = cap;
	}
	snprintf(t->names[t->n++], 96, "%s\t%s", tag, weapon);
}
static int wtc_tags_has(const WtcTags *t, const char *tag, const char *weapon)
{
	char k[96];
	snprintf(k, sizeof(k), "%s\t%s", tag, weapon);
	for (int i = 0; i < t->n; i++)
		if (strcmp(t->names[i], k) == 0)
			return 1;
	return 0;
}

static void wtc_unknown_section(WtcInfo *info, const char *name)
{
	char quoted[80];
	snprintf(quoted, sizeof(quoted), "[%s]", name);
	if (strstr(info->unknown, quoted))
		return;
	info->unknownCount++;
	if (strlen(info->unknown) + strlen(quoted) + 3 < sizeof(info->unknown))
	{
		if (info->unknown[0])
			strcat(info->unknown, ", ");
		strcat(info->unknown, quoted);
	}
}

/* guns list: "a, b:p1:p2, c" -> one line per gun */
static void wtc_list(const WtcList *L, char *value, const WtcTags *tags, WtcBuf *out, WtcInfo *info)
{
	value = wtc_nocomment(value);
	for (char *tok = value; tok && *tok;)
	{
		char *next = strchr(tok, ',');
		if (next)
			*next++ = 0;
		char *g = wtc_trim(tok);
		tok = next;
		if (!*g)
			continue;
		char *params = strchr(g, ':');
		if (params)
			*params++ = 0;
		g = wtc_trim(g);
		if (wtc_tags_has(tags, L->wkey, g))
			continue;  /* its [weapon:] section says it */
		char v[512], p[256] = {0};
		if (params)  /* p1:p2 -> p1,p2 */
		{
			snprintf(p, sizeof(p), "%s", wtc_trim(params));
			for (char *c = p; *c; c++)
				if (*c == ':')
					*c = ',';
		}
		switch (L->kind)
		{
		case WTC_ONOFF:
		{
			const char *mode = "1", *rest = p;
			char first[64] = {0};
			size_t fl = strcspn(p, ",");
			if (fl < sizeof(first))
			{
				memcpy(first, p, fl);
				if (wtc_ieq(first, "on") || wtc_ieq(first, "off"))
				{
					mode = wtc_ieq(first, "on") ? "1" : "0";
					rest = p + fl + (p[fl] == ',');
				}
			}
			snprintf(v, sizeof(v), "%s,%s%s%s", g, mode, *rest ? "," : "", rest);
			break;
		}
		case WTC_MODE:
			if (!p[0] && !L->def)
			{
				wtc_warn(info, "[%s] guns: '%s' needs a value (gun:value); left out", L->section, g);
				continue;
			}
			snprintf(v, sizeof(v), "%s,%s", g, p[0] ? p : L->def);
			break;
		case WTC_FIXED0:
			snprintf(v, sizeof(v), "%s,0", g);
			break;
		default:
			snprintf(v, sizeof(v), "%s", g);
			break;
		}
		wtc_line(out, info, L->cfgkey, v);
	}
}

static void wtc_feature(const char *name, char *value, WtcBuf *out, WtcInfo *info)
{
	value = wtc_nocomment(value);
	char v[128];
	int m = wtc_mode(value);
	snprintf(v, sizeof(v), "%s,%s", name, m == 1 ? "on" : m == 0 ? "off" : value);
	wtc_line(out, info, "feature", v);
	if (strcmp(name, "last_shot") == 0 && m < 0)
		wtc_line(out, info, "empty_lastshot", value);  /* the default mode, live like any empty_lastshot= line */
	if (strcmp(name, "vmfov") == 0 && m != 1)
		wtc_line(out, info, "vmfov", m == 0 ? "off" : value);
}

/* The canonical flat text (malloc'd), or NULL when `text` has no [section] (use it as it is). */
static char *wtc_normalise(const char *text, WtcInfo *info)
{
	memset(info, 0, sizeof(*info));
	char line[256];
	/* pass 1: sectioned at all? which guns does a [weapon:] section claim? */
	WtcTags tags = {NULL, 0, 0};
	char weapon[64] = {0};
	for (const char *p = text; (p = wtc_next(p, line, sizeof(line))) != NULL;)
	{
		char tmp[256];
		memcpy(tmp, line, sizeof(tmp));
		char *h = wtc_header(tmp);
		if (h)
		{
			info->sectioned = 1;
			weapon[0] = 0;
			if (strncmp(h, "weapon:", 7) == 0)
				snprintf(weapon, sizeof(weapon), "%s", wtc_trim(h + 7));
			continue;
		}
		if (!weapon[0])
			continue;
		char *s = wtc_trim(tmp);
		char *eq = strchr(s, '=');
		if (!eq || *s == '#')
			continue;
		*eq = 0;
		s = wtc_trim(s);
		static const char *const kTags[] = {"inspect", "ik", "segreload", "last_shot", "ammohide_auto", "take_jukes"};
		for (size_t i = 0; i < sizeof(kTags) / sizeof(kTags[0]); i++)
			if (wtc_weapon_has_tag(s, kTags[i]))
				wtc_tags_add(&tags, kTags[i], weapon);
	}
	if (!info->sectioned)
	{
		free(tags.names);
		return NULL;
	}
	/* pass 2 */
	WtcBuf out = {NULL, 0, 0};
	wtc_puts(&out, "");
	WtcWeapon wpn;
	memset(&wpn, 0, sizeof(wpn));
	char cur[96] = {0}, stack[8][96];
	int depth = 0, inSection = 0;
	for (const char *p = text; (p = wtc_next(p, line, sizeof(line))) != NULL;)
	{
		if (!inSection)
		{
			char tmp[256];
			memcpy(tmp, line, sizeof(tmp));
			if (!wtc_header(tmp))
			{
				/* before the first [section]: the old flat format, untouched */
				wtc_puts(&out, line);
				if (!strchr(line, '\n'))
					wtc_puts(&out, "\n");
				continue;
			}
			inSection = 1;
		}
		if (wtc_fence(line, "BEGIN"))
		{
			if (depth < 8)
				memcpy(stack[depth++], cur, sizeof(cur));
			continue;
		}
		if (wtc_fence(line, "END"))
		{
			if (depth > 0)
			{
				wtc_weapon_flush(&wpn, &out, info);
				memcpy(cur, stack[--depth], sizeof(cur));
				if (strncmp(cur, "weapon:", 7) == 0)  /* back inside a [weapon:] section */
				{
					wpn.active = 1;
					snprintf(wpn.name, sizeof(wpn.name), "%s", wtc_trim(cur + 7));
				}
			}
			continue;
		}
		char tmp[256];
		memcpy(tmp, line, sizeof(tmp));
		char *h = wtc_header(tmp);
		if (h)
		{
			wtc_weapon_flush(&wpn, &out, info);
			snprintf(cur, sizeof(cur), "%s", h);
			if (strncmp(h, "weapon:", 7) == 0)
			{
				wpn.active = 1;
				snprintf(wpn.name, sizeof(wpn.name), "%s", wtc_trim(h + 7));
				if (!wpn.name[0])
					wtc_warn(info, "[weapon:] with no weapon name", NULL, NULL);
			}
			else if (!wtc_section(h))
				wtc_unknown_section(info, h);
			continue;
		}
		char *s = wtc_trim(tmp);
		if (!*s || *s == '#' || (s[0] == '/' && s[1] == '/'))
			continue;
		char *eq = strchr(s, '=');
		if (!eq)
		{
			wtc_warn(info, "[%s]: no '=' in '%s' (ignored)", cur, s);
			continue;
		}
		*eq = 0;
		char *key = wtc_trim(s), *value = wtc_trim(eq + 1);
		if (wpn.active)
		{
			if (wpn.name[0])
				wtc_weapon_key(&wpn, key, value, info);
			continue;
		}
		if (wtc_ieq(cur, "features"))
		{
			wtc_feature(key, value, &out, info);
			continue;
		}
		const WtcList *list = NULL;
		for (size_t i = 0; i < sizeof(kWtcLists) / sizeof(kWtcLists[0]) && !list; i++)
			if (wtc_ieq(kWtcLists[i].section, cur) && strcmp(kWtcLists[i].listkey, key) == 0)
				list = &kWtcLists[i];
		if (list)
		{
			wtc_list(list, value, &tags, &out, info);
			continue;
		}
		char full[128];
		wtc_fullkey(wtc_section(cur), key, full, sizeof(full));
		wtc_line(&out, info, full, value);
	}
	wtc_weapon_flush(&wpn, &out, info);
	wtc_weapon_reset(&wpn);
	free(tags.names);
	return out.p;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
