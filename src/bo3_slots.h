// Purpose slots: more additive layers per viewmodel than the two free swim groups (193 / 195) give, by building extra
// additive ROOTS out of spare juke leaves and hanging spare base slots under them as their leaves. Included by
// weapon_tech.h after bo3_slide.h. Research: xpakcap\additives\NOTES_additives*.txt (XAnimCalc, tree build) and the
// IDA dumps of the 2026-10-02 slot survey (CC980's whole ps-anim -> node map; the jump / land / fall / juke drivers).
//
// Why a spare base slot can't just play additively where it is: the viewmodel XAnimTree is node 0 "root" with children
// 1..196 (node = szXAnims slot). XAnimCalc 0x142405350 blends a parent's weighted children in index order: every child
// before the first weighted ADDITIVE blend node (XAnim_s entry flag 0x10) is a base child, normalised per bone; only
// the children from there on are added. Additivity is that flag on the parent, nothing in the XAnimParts. So slots
// 1..175 always blend into the base pose, and an additive root must have an index above every base slot the engine can
// weight (173..175 are the ADS up / down anims): 176..196 only. Those are the engine's groups (table 0x142E82890):
// 176 jump, 178 land, 180 fall, 182 walk, 184 juke (leaves 185..192), 193 / 195 swim. Karelia's guns use jump / land /
// fall (vm_arak_*), the walk, and the slide gesture on 187 / 188 / 191; 192 (jukeBackwardADSAnim) is blank on all of
// them, and 185 / 186 / 189 / 190 (juke L / R / L ADS / R ADS) on the guns without jukes.
//
// What this does, per viewmodel tree (after each build; the build rewrites every entry, so it is undone by itself):
//   - a spare juke leaf R becomes an additive blend under node 0: XAnim_s entry(R) child count 1, flags 0x10, parent 0
//     (what XAnimBlend 0x1423FD8D0 writes for the engine's groups);
//   - a never-selected base slot L becomes its leaf: entry(L) parent R. L holds the layer's xanim because its NAME is
//     written into szXAnims[L] before the build (bo3_perf.h BindSub, as for 194 / 196).
// Everything at runtime walks the XAnimInfo lists, which EnsureInfo 0x142401370 builds from entry +0x08 (parent),
// +0x02 (children) and +0x10 (parts / flags) when a node is first weighted; so the edit is made only while neither node
// has an info, and the layer is driven like the engine's own groups (instant weights, rate 0 leaf, time set).
//
// The engine and these nodes: the juke driver 0x1404E2980 sets 184..192 to weight 0 every frame unless juking (we write
// after it); it juke-drives 185 / 186 / 189 / 190 only when juke L / R / L ADS / R ADS are all named, and 192 only when
// F / B / F ADS / B ADS are. CC980 (ps anim -> node) never returns 117 / 118 / 119 / 124 and no viewmodel code names
// them; CG_StartWeaponAnim sets 1..172 to 0 on each anim start (we write after it).
//
// Purposes (fixed; additive=<weapon>,<kind>,slot:<purpose>,<xanim>[,<weight>[,<mag|rate>]][,side:<right|left|both>]):
//   purpose     root  leaf  kinds        root needs                                  status
//   bullets     192   117   bullet       slot 192 blank (jukeBackwardADSAnim)          live
//   empty       190   118   empty        not all of juke L/R/LADS/RADS named           live
//   recoil_ads  189   119   recoil       as empty; the recoil runs at ADS (x ads)     live
//   empty_left  186   124   empty        as empty; a dual-wield gun                    live (side:left of empty)
//   bullets_left 185  108   bullet       as empty; a dual-wield gun                    live (side:left of bullets)
//   idle_alt    186   124   -            (its nodes went to empty_left, 2026-10-02)    reserved (no driver yet)
//   gesture     185   -     -            (its root went to bullets_left)               reserved (mantle / ladder / fire mode)
//   reload_add  190   -     -            shares empty's root (empty is off in reloads) reserved
//   idle        193   194   any          (the old root number; same as additive=..,193,..)
//   recoil      195   196   any          (the old root number; same as additive=..,195,..)
// Not ours: 176/177 jump, 178/179 land, 180/181 fall (BO3's own, named on Karelia's guns), 182/183 walk, 184 slide
// gesture (187 / 188 / 191).
//
// Dual wield / akimbo (2026-10-02): side:right (the default) drives a line from the RIGHT gun (its clip, ps hand block
// +0x54..+0x64, viewmodel node +0x368); side:left from the LEFT gun: its clip is held[i] == BG_GetClipWeapon(
// BG_GetDualWieldWeapon(weapon)) (0x1427C6590 / 0x1427D18E0; bo3_inspect.h InspectHandClip), its weaponState ps+0x78, its
// viewmodel node vm+0x370 (161..172: 167 lastShotL, 171 / 172 reload empty / reload). Both guns are in ONE DObj and tree;
// the akimbo rig names the left gun's bones apart (j_slide1, tag_pistol_offset1, tag_weapon_le, ...), so a side:left
// line needs its own left-gun xanim (Maya\BO7\akimbo_left\gen_left_additives.py makes one from the right's). Each side has
// its own additive root (the purpose's _left def), so the two sides fade independently. Children of a blend node are a
// contiguous index range, so a left leaf can't share the right's root (118's neighbours are taken). 124 is never
// selected by 0x1427CC980; 108 is adsRechamberAnim (ps anim 12), which only an ADS shot selects, and a dual-wield gun
// never ADSes (its ADS button fires the left gun; left lines refuse a gun that isn't dual wield). NOT 125..160: on a
// dual-wield gun those hold dw anims (125 = vm_..._dw_sprint_in on the decho PaP, seen 2026-10-02). side:both is
// refused: one xanim can't follow two clips.
// Per-side recoil: every slot line counts its own side's clip going down (fire-hand detection falls out of that), but
// there is no third free root for a left recoil layer, and a dual-wield gun never ADSes (recoil_ads); not built.
// Config:
//   slots_take_jukes=<weapon>|all   repeatable: use 185 / 186 / 189 / 190 even when the gun's jukes are all named (its
//                                   juke L / R anims then don't play during a rocket-shield juke)
//   slots_debug=0|1                 live: bind / refuse / state lines
//   slots_dump=0|1                  log every szXAnims slot (0..196) of each weapon a slot line binds, once (slot map check)
// Fails closed: the tree-build table / XAnim_s layout / EnsureInfo code must be the Enhanced exe's (one log line, every
// slot line dropped); a weapon whose nodes are taken (jukes, locomotion, slide) gets a log line and no layer.
#pragma once

namespace
{
	enum SlotStatus : uint8_t { kSlotLive, kSlotReserved, kSlotLegacy };
	enum SlotRootReq : uint8_t { kRootReq192, kRootReqJuke, kRootReqNone };
	struct SlotDef
	{
		const char *name;
		uint32_t root, leaf;
		SlotRootReq req;
		SlotStatus status;
	};
	constexpr SlotDef kSlotDefs[] = {
	    {"bullets", 192, 117, kRootReq192, kSlotLive},
	    {"empty", 190, 118, kRootReqJuke, kSlotLive},
	    {"recoil_ads", 189, 119, kRootReqJuke, kSlotLive},
	    {"empty_left", 186, 124, kRootReqJuke, kSlotLive},
	    {"bullets_left", 185, 108, kRootReqJuke, kSlotLive},
	    {"idle_alt", 186, 124, kRootReqJuke, kSlotReserved},
	    {"gesture", 185, 0, kRootReqJuke, kSlotReserved},
	    {"reload_add", 190, 0, kRootReqJuke, kSlotReserved},
	    {"idle", 193, 194, kRootReqNone, kSlotLegacy},
	    {"recoil", 195, 196, kRootReqNone, kSlotLegacy},
	};
	constexpr int kSlotDefCount = sizeof(kSlotDefs) / sizeof(kSlotDefs[0]);
	constexpr uint32_t kJukeRootNode = 184, kJukeLeaves[4] = {185, 186, 189, 190};
	enum SlotSide { kSideRight, kSideLeft, kSideBoth };

	// ---- Engine addresses (RVAs; Enhanced, bo3_retail.h sets retail's) and the code / data they are checked against -----
	uintptr_t kSlotGroupTable = 0x2E82890;   // {name, root, first, last, pad} x 7, read by the tree build
	uintptr_t kSlotGroupLoopEnd = 0x4EC4A9;  // lea rax, [table end]: the build's group loop
	uintptr_t kSlotGroupLoopEndField = 0x10; // the field of table[7] that lea points at (retail 0xC)
	uintptr_t kSlotBaseLoop = 0x4EC4CF;      // mov edi, 175: then nodes 1..175 from the names (retail mov ebx, 175)
	uintptr_t kSlotRootInit = 0x4EC343;      // root: 196 children, flags 0 / first child 1 (retail: a call, kSlotRootInitFn)
	uintptr_t kSlotRootInitFn = 0;           // retail: the node-init function the root init calls
	uintptr_t kSlotCreateStores = 0x23FD875; // XAnimCreate: entry +0x00 = 1 (no children), +0x10 = parts
	uintptr_t kSlotEnsureInfo = 0x24013AB;   // EnsureInfo: parent (+0x08), children (+0x02), parts (+0x10)
	constexpr size_t kXaEntries = 0x28, kXaStride = 0x18, kXaBytes = kXaEntries + kXaStride * 197;

	struct SlotRun
	{
		float w = 0;          // current layer weight (own fade; the engine zeroes the juke group every frame)
		bool ok = false;      // eligible on the held variant
		bool bound = false;   // the tree has our structure for it
		bool waitLogged = false;
		const char *why = "";
	};
	SlotRun g_slotRun[kMaxAdditives];
	bool g_slotsLive, g_slotsDebug, g_slotsDump, g_slotsTakeAllJukes;
	char g_slotsTakeJukes[32][64];
	int g_slotsTakeJukeCount;
	int g_slotLineCount;

	const SlotDef *FindSlotDef(const char *name)
	{
		for (const SlotDef &d : kSlotDefs)
			if (_stricmp(d.name, name) == 0)
				return &d;
		return nullptr;
	}
	const SlotDef *SlotDefOf(const AdditiveConfig &c) { return c.purpose >= 0 && c.purpose < kSlotDefCount ? &kSlotDefs[c.purpose] : nullptr; }

	// Splits a value on commas (a '#' ends it), trimming blanks. Returns the field count.
	int SlotSplit(const char *value, char out[][96], int max)
	{
		char buf[320];
		strncpy_s(buf, value, _TRUNCATE);
		if (char *hash = strchr(buf, '#'))
			*hash = 0;
		int n = 0;
		for (char *p = buf; n < max;)
		{
			char *comma = strchr(p, ',');
			if (comma)
				*comma = 0;
			while (*p == ' ' || *p == '\t')
				p++;
			size_t len = strlen(p);
			while (len && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n'))
				p[--len] = 0;
			strncpy_s(out[n++], 96, p, _TRUNCATE);
			if (!comma)
				break;
			p = comma + 1;
		}
		return n;
	}

	// additive= with "slot:<purpose>" as its third field (bo3_additive.h ParseAdditive hands it over). Returns -1 when it
	// isn't a slot line, 0 when it is but can't be used (logged), 1 when c is filled. Legacy purposes (idle / recoil) fill
	// root 193 / 195 and purpose -1, i.e. the old code drives them.
	int ParseSlotAdditive(const char *value, AdditiveConfig &c)
	{
		char f[8][96] = {};
		int n = SlotSplit(value, f, 8);
		if (n < 3 || _strnicmp(f[2], "slot:", 5) != 0)
			return -1;
		const SlotDef *d = FindSlotDef(f[2] + 5);
		const bool leftName = d && strstr(d->name, "_left") != nullptr;  // slot:empty_left = slot:empty,..,side:left
		if (n < 4 || !f[0][0] || !f[3][0] || !d)
		{
			Log("slots: '%s': needs <weapon>,<kind>,slot:<purpose>,<xanim> with a known purpose", value);
			return 0;
		}
		// trailing values: numbers in order (weight, then mag / rate), and named tokens (side:)
		float num[2] = {1.0f, 0.0f};
		int nums = 0, side = kSideRight;
		for (int i = 4; i < n; i++)
		{
			if (_strnicmp(f[i], "side:", 5) == 0)
			{
				const char *s = f[i] + 5;
				side = !_stricmp(s, "right") ? kSideRight : !_stricmp(s, "left") ? kSideLeft : !_stricmp(s, "both") ? kSideBoth : -1;
				if (side < 0)
				{
					Log("slots: '%s': side must be right, left or both", value);
					return 0;
				}
			}
			else if (f[i][0] && nums < 2)
				num[nums++] = static_cast<float>(atof(f[i]));
		}
		if (side == kSideBoth)
		{
			Log("slots: '%s': side:both is refused: each gun needs its own xanim (the left gun's bones are named apart); give a "
			    "side:right and a side:left line", value);
			return 0;
		}
		if (leftName)
			side = kSideLeft;
		else if (side == kSideLeft)
		{
			char left[64];
			snprintf(left, sizeof(left), "%s_left", d->name);
			const SlotDef *l = FindSlotDef(left);
			if (!l)
			{
				Log("slots: '%s': slot:%s has no left-hand layer (side:left works on empty and bullets)", value, d->name);
				return 0;
			}
			d = l;
		}
		if (d->status == kSlotReserved)
		{
			Log("slots: '%s': purpose '%s' is reserved (no driver yet); line ignored", value, d->name);
			return 0;
		}
		const AdditiveKind kind = !_stricmp(f[1], "recoil") ? AdditiveKind::Recoil
		                          : !_stricmp(f[1], "bullet") ? AdditiveKind::Bullet
		                          : !_stricmp(f[1], "empty")  ? AdditiveKind::Empty
		                                                      : static_cast<AdditiveKind>(-1);
		if (static_cast<int>(kind) < 0)
		{
			Log("slots: '%s': kind must be empty, bullet or recoil", value);
			return 0;
		}
		if (d == FindSlotDef("recoil_ads") && kind != AdditiveKind::Recoil)
		{
			Log("slots: '%s': slot:recoil_ads takes a recoil line", value);
			return 0;
		}
		if ((d == FindSlotDef("empty_left") && kind != AdditiveKind::Empty) || (d == FindSlotDef("bullets_left") && kind != AdditiveKind::Bullet))
		{
			Log("slots: '%s': the left empty layer takes an empty line, the left bullets layer a bullet line", value);
			return 0;
		}
		strncpy_s(c.weapon, f[0], _TRUNCATE);
		strncpy_s(c.xanim, f[3], _TRUNCATE);
		c.kind = kind;
		c.weight = num[0];
		c.magSize = kind == AdditiveKind::Bullet ? static_cast<int>(num[1]) : 0;
		c.rate = kind == AdditiveKind::Recoil && nums >= 2 && num[1] >= 0 ? num[1] : 1.0f;
		c.side = side;
		c.root = d->root;
		c.leaf = d->leaf;
		c.purpose = d->status == kSlotLegacy ? -1 : static_cast<int>(d - kSlotDefs);
		if (kind == AdditiveKind::Bullet && c.magSize <= 0)
		{
			Log("slots: '%s': a bullet line needs the mag size after the weight", value);
			return 0;
		}
		return 1;
	}

	// Live tuning of a slot line (weight, mag, rate), from ApplyTuningText. False when it isn't a slot line.
	bool SlotLiveTuning(const char *value)
	{
		AdditiveConfig t;
		const int r = ParseSlotAdditive(value, t);
		if (r < 0)
			return false;
		if (r == 0)
			return true;
		for (int i = 0; i < g_additiveCount; i++)
		{
			AdditiveConfig &c = g_additives[i];
			if (strcmp(c.weapon, t.weapon) || c.kind != t.kind || c.root != t.root || c.leaf != t.leaf)
				continue;
			c.weight = t.weight;
			if (t.kind == AdditiveKind::Bullet)
				c.magSize = t.magSize;
			if (t.kind == AdditiveKind::Recoil)
				c.rate = t.rate;
			Log("slots: live tuning: %s %s weight %.2f rate %.2f mag %d", c.weapon, value, c.weight, c.rate, c.magSize);
		}
		return true;
	}

	// slots_* lines (ParseWeaponLine). slots_debug / slots_dump are live too (LiveSlotsLine).
	bool ParseSlotsLine(const char *line)
	{
		int v;
		if (sscanf_s(line, "slots_debug=%d", &v) == 1)
			return (g_slotsDebug = v != 0), true;
		if (sscanf_s(line, "slots_dump=%d", &v) == 1)
			return (g_slotsDump = v != 0), true;
		if (strncmp(line, "slots_take_jukes=", 17) == 0)
		{
			char w[64] = {};
			if (sscanf_s(line + 17, "%63[^ \t#]", w, static_cast<unsigned>(sizeof(w))) != 1)
				return false;
			if (!_stricmp(w, "all"))
				return g_slotsTakeAllJukes = true;
			if (g_slotsTakeJukeCount >= 32)
				return false;
			strcpy_s(g_slotsTakeJukes[g_slotsTakeJukeCount++], w);
			return true;
		}
		return false;
	}
	void LiveSlotsLine(const char *line)
	{
		int v;
		if (sscanf_s(line, "slots_debug=%d", &v) == 1 && (v != 0) != g_slotsDebug)
		{
			g_slotsDebug = v != 0;
			Log("slots: live slots_debug=%d", v);
		}
		else if (sscanf_s(line, "slots_dump=%d", &v) == 1)
			g_slotsDump = v != 0;
	}
	bool SlotTakesJukes(const char *weapon)
	{
		if (g_slotsTakeAllJukes)
			return true;
		for (int i = 0; i < g_slotsTakeJukeCount; i++)
			if (!strcmp(g_slotsTakeJukes[i], weapon))
				return true;
		return false;
	}

	// ---- install-time checks ----------------------------------------------------------------------------------------------
	// The tree-build data and code these edits stand on, as read in the Enhanced exe (IDA 2026-10-02).
	bool SlotShapeOk(const char **what)
	{
		static const uint32_t kGroups[7][3] = {{176, 177, 177}, {178, 179, 179}, {182, 183, 183}, {184, 185, 192},
		                                       {180, 181, 181}, {193, 194, 194}, {195, 196, 196}};
		const uint8_t *t = At<uint8_t>(kSlotGroupTable);
		if (!FastReadable(t, 7 * 24))
			return *what = "group table unreadable", false;
		for (int i = 0; i < 7; i++)
			if (memcmp(t + 24 * i + 8, kGroups[i], 12) != 0)
				return *what = "additive group table differs", false;
		static const uint8_t kLoopEnh[] = {0xBF, 0xAF, 0x00, 0x00, 0x00};  // mov edi, 175
		static const uint8_t kLoopRet[] = {0xBB, 0xAF, 0x00, 0x00, 0x00};  // mov ebx, 175
		const uint8_t *kLoop = IsRetailExe() ? kLoopRet : kLoopEnh;
		static const uint8_t kRootEnh[] = {0x41, 0xB8, 0xC4, 0x00, 0x00, 0x00, 0x41, 0xC7, 0x46, 0x38, 0x00, 0x00, 0x01, 0x00,
		                                   0x66, 0x45, 0x89, 0x46, 0x2A};  // r8d = 196; [r14+38h] = 10000h; [r14+2Ah] = r8w
		// retail: [rsp+28h] = 0 (flags); [rsp+20h] = 196 (children); call init (tree, 0, name, first child = esi = 1)
		static const uint8_t kRootRet[] = {0xC7, 0x44, 0x24, 0x28, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x44, 0x24, 0x20, 0xC4, 0x00, 0x00, 0x00};
		static const uint8_t kCreateEnh[] = {0x48, 0x8D, 0x14, 0x7F, 0xC7, 0x44, 0xD5, 0x28, 0x01, 0x00, 0x00, 0x00,
		                                     0x48, 0x89, 0x44, 0xD5, 0x38};  // entry +0x00 = 1; entry +0x10 = parts
		static const uint8_t kCreateRet[] = {0x48, 0x8D, 0x14, 0x5B, 0xC7, 0x44, 0xD7, 0x28, 0x01, 0x00, 0x00, 0x00,
		                                     0x48, 0x89, 0x44, 0xD7, 0x38};  // the same stores (rbx = node, rdi = tree)
		const uint8_t *kCreate = IsRetailExe() ? kCreateRet : kCreateEnh;
		static_assert(sizeof(kCreateEnh) == sizeof(kCreateRet), "same length");
		static const uint8_t kEnsure[] = {0x49, 0x8B, 0x06, 0x48, 0x8D, 0x14, 0x7F, 0x48, 0x8D, 0x2C, 0xD0, 0x0F, 0xB7, 0x54,
		                                  0xD0, 0x30};  // rbp = xa + 24n; edx = parent (entry +0x08)
		static const uint8_t kEnsure2[] = {0x66, 0x83, 0x7D, 0x2A, 0x00};  // cmp word [rbp+2Ah], 0: children (entry +0x02)
		static const uint8_t kEnsure3[] = {0x48, 0x8B, 0x4D, 0x38};        // mov rcx, [rbp+38h]: parts (entry +0x10)
		if (!CodeRefers(kSlotGroupLoopEnd, 7, kSlotGroupTable + 7 * 24 + kSlotGroupLoopEndField)  /* the loop walks one field of each entry: lea = &table[7].field (Enhanced +0x10 read live 2026-10-02; retail +0xC) */ || !CodeMatches(kSlotBaseLoop, kLoop, sizeof(kLoopEnh)))
			return *what = "tree build group loop differs", false;
		if (IsRetailExe() ? !CodeMatches(kSlotRootInit, kRootRet, sizeof(kRootRet)) || !CallsTo(At<uint8_t>(kSlotRootInit + sizeof(kRootRet)), kSlotRootInitFn)
		                  : !CodeMatches(kSlotRootInit, kRootEnh, sizeof(kRootEnh)))
			return *what = "tree build root init differs", false;
		if (!CodeMatches(kSlotCreateStores, kCreate, sizeof(kCreateEnh)))
			return *what = "XAnimCreate 0x1423FD840 differs", false;
		if (!CodeMatches(kSlotEnsureInfo, kEnsure, sizeof(kEnsure)) || !CodeMatches(kSlotEnsureInfo + 0x15, kEnsure2, sizeof(kEnsure2)) ||
		    !CodeMatches(kSlotEnsureInfo + 0x33, kEnsure3, sizeof(kEnsure3)))
			return *what = "EnsureInfo 0x142401370 differs", false;
		return true;
	}

	void SlotsVmFrame(uint8_t *ps, uint8_t *vm, double now);

	// From InstallAdditives, before any slot name is written: drops the slot lines that can't work (bad exe shape,
	// duplicates), then installs the per-frame driver. Compacts g_additives, so it must run before the variants bind.
	void FinishSlotConfig()
	{
		int lines = 0;
		for (int i = 0; i < g_additiveCount; i++)
			lines += g_additives[i].purpose >= 0;
		if (!lines)
			return;
		const char *what = "";
		const bool shape = SlotShapeOk(&what);
		if (!shape)
			Log("slots: %s: not the %s exe's tree build; all %d slot line(s) dropped", what, WtExeName(), lines);
		int out = 0;
		for (int i = 0; i < g_additiveCount; i++)
		{
			const AdditiveConfig &c = g_additives[i];
			bool keep = c.purpose < 0 || shape;
			for (int j = 0; j < out && keep && c.purpose >= 0; j++)
			{
				const AdditiveConfig &o = g_additives[j];
				if (o.purpose == c.purpose && !strcmp(o.weapon, c.weapon))
				{
					Log("slots: %s: a second slot:%s line (%s) ignored; the first (%s) is kept", c.weapon, kSlotDefs[c.purpose].name,
					    c.xanim, o.xanim);
					keep = false;
				}
			}
			if (keep)
				g_additives[out++] = c;
		}
		g_additiveCount = out;
		g_slotLineCount = 0;
		for (int i = 0; i < g_additiveCount; i++)
			g_slotLineCount += g_additives[i].purpose >= 0;
		if (!g_slotLineCount)
			return;
		g_slotsVmFrame = &SlotsVmFrame;
		g_slotsLive = true;
		Log("slots: %d slot line(s); purposes: bullets 192/117, empty 190/118, recoil_ads 189/119, empty_left 186/124, bullets_left "
		    "185/108 (root/leaf)", g_slotLineCount);
	}

	// ---- per frame ------------------------------------------------------------------------------------------------------
	uint8_t *XaEntry(uint8_t *xa, uint32_t n) { return xa + kXaEntries + kXaStride * n; }
	uint16_t XaKids(uint8_t *xa, uint32_t n) { return *reinterpret_cast<uint16_t *>(XaEntry(xa, n) + 0x02); }
	uint16_t XaParent(uint8_t *xa, uint32_t n) { return *reinterpret_cast<uint16_t *>(XaEntry(xa, n) + 0x08); }
	uint16_t XaFlags(uint8_t *xa, uint32_t n) { return *reinterpret_cast<uint16_t *>(XaEntry(xa, n) + 0x10); }
	uint16_t XaFirst(uint8_t *xa, uint32_t n) { return *reinterpret_cast<uint16_t *>(XaEntry(xa, n) + 0x12); }
	const uint8_t *XaParts(uint8_t *xa, uint32_t n) { return *reinterpret_cast<const uint8_t *const *>(XaEntry(xa, n) + 0x10); }

	// The engine's own groups where the build puts them: root 196 children, 184 -> 185..192 additive, 193 -> 194 additive.
	// (entries we never touch, so this holds before and after our edits)
	bool XaLooksBuilt(uint8_t *xa)
	{
		return FastReadable(xa, kXaBytes) && XaKids(xa, 0) == 196 && XaFirst(xa, 0) == 1 && XaKids(xa, 184) == 8 &&
		       (XaFlags(xa, 184) & 0x10) && XaFirst(xa, 184) == 185 && XaKids(xa, 193) == 1 && (XaFlags(xa, 193) & 0x10) &&
		       XaFirst(xa, 193) == 194;
	}

	const char *VariantSlotName(const uint8_t *vdef, uint32_t slot)
	{
		if (!vdef || !FastReadable(vdef, kVariantAnims + 8))
			return nullptr;
		auto *const *anims = *reinterpret_cast<const char *const *const *>(vdef + kVariantAnims);
		if (!anims || !FastReadable(anims + slot, 8))
			return nullptr;
		const char *s = anims[slot];
		return s && FastReadable(s, 1) ? s : nullptr;
	}
	bool Named(const uint8_t *vdef, uint32_t slot)
	{
		const char *s = VariantSlotName(vdef, slot);
		return s && *s;
	}

	void DumpSlots(const char *weapon, const uint8_t *vdef)
	{
		Log("slots: dump of %s szXAnims (slot = name):", weapon);
		char buf[1024];
		int len = 0;
		for (uint32_t s = 0; s < 197; s++)
		{
			const char *n = VariantSlotName(vdef, s);
			char item[128];
			int k = snprintf(item, sizeof(item), "%u=%s ", s, n ? (*n ? n : "''") : "null");
			if (len + k >= static_cast<int>(sizeof(buf)) - 1)
			{
				Log("slots:   %s", buf);
				len = 0;
			}
			memcpy(buf + len, item, k + 1);
			len += k;
		}
		if (len)
			Log("slots:   %s", buf);
	}

	// The left hand's ps / viewmodel offsets and the clip-weapon calls, as bo3_inspect.h checks them (once; fails closed).
	bool SlotAkimboOk()
	{
		static int s_ok = -1;
		if (s_ok < 0)
		{
			const char *what = "";
			s_ok = InspectCheckAkimbo(what) ? 1 : 0;
			if (!s_ok)
				Log("slots: akimbo: %s differ from the Enhanced exe; side:left lines stay off", what);
		}
		return s_ok == 1;
	}

	// Whether line a may use its nodes on this variant (names, dual wield, locomotion, slide). Sets run.why.
	bool SlotEligible(const AdditiveConfig &c, SlotRun &run, uint64_t weapon, const uint8_t *vdef)
	{
		const SlotDef &d = kSlotDefs[c.purpose];
		run.why = "";
		if (c.side == kSideLeft)
		{
			if (!Engine().isDualWield(weapon))
				return run.why = "side:left on a weapon that isn't dual wield", false;
			if (!SlotAkimboOk())
				return run.why = "the akimbo offsets / calls don't match this exe", false;
		}
		const bool take = SlotTakesJukes(c.weapon);
		if (d.req == kRootReq192 && Named(vdef, 192) && !take)
			return run.why = "slot 192 (jukeBackwardADSAnim) is named on this gun", false;
		if (d.req == kRootReqJuke && !take)
		{
			int named = 0;
			for (uint32_t s : kJukeLeaves)
				named += Named(vdef, s);
			if (named == 4)
				return run.why = "its jukes are named (185/186/189/190); slots_take_jukes=<weapon> to use them anyway", false;
		}
		if (const LocoWeapon *loco = FindLocoForVariant(static_cast<int>(weapon & 0x1FF)))
		{
			if (loco->jog && (loco->jogNode == d.root || loco->jogNode == d.leaf))
				return run.why = "its locomotion jog uses this node", false;
			if (loco->idle && !loco->idleRefused && (loco->idleNode == d.root || loco->idleNode == d.leaf))
				return run.why = "its idle_active uses this node", false;
		}
		if (g_slHooked)
			for (uint32_t n : Sl().nodes)
				if (n == d.root || n == d.leaf)
					return run.why = "the slide gesture uses this node (slide_gesture_nodes)", false;
		return true;
	}

	bool SlotRootOurs(uint8_t *xa, uint32_t R) { return XaKids(xa, R) == 1 && XaFlags(xa, R) == 0x10 && XaParent(xa, R) == 0; }
	bool SlotLeafOurs(uint8_t *xa, uint32_t R, uint32_t L) { return XaKids(xa, L) == 0 && XaParent(xa, L) == R; }

	// Puts the root / leaf structure in place for line c (or confirms it). False while it can't be (yet).
	bool SlotBind(const AdditiveConfig &c, SlotRun &run, void *dobj, uint8_t *tree, uint8_t *xa)
	{
		const SlotDef &d = kSlotDefs[c.purpose];
		const uint32_t R = d.root, L = d.leaf;
		const bool rootOurs = SlotRootOurs(xa, R);
		const bool rootOrig = XaKids(xa, R) == 0 && XaParent(xa, R) == kJukeRootNode;
		const bool leafOurs = SlotLeafOurs(xa, R, L);
		const bool leafOrig = XaKids(xa, L) == 0 && XaParent(xa, L) == 0;
		if (rootOurs && leafOurs)
			return true;
		if (!(rootOurs || rootOrig) || !(leafOurs || leafOrig))
		{
			run.why = "the tree's nodes aren't as the build leaves them";
			return false;
		}
		// The leaf must hold this line's xanim (the name lands on the next build) and a real one.
		const uint8_t *parts = XaParts(xa, L);
		const char *pname = parts && FastReadable(parts, 0x58) ? *reinterpret_cast<const char *const *>(parts) : nullptr;
		if (!pname || !FastReadable(pname, 1) || _stricmp(pname, c.xanim) != 0 || parts == XaParts(xa, 1) ||
		    *reinterpret_cast<const float *>(parts + 0x54) <= 0.0f)
		{
			run.why = "the leaf doesn't hold the xanim yet (lands on the next raise) or it isn't loaded";
			static int s_detail;
			if (s_detail < 40)
			{
				s_detail++;
				Log("slots: %s leaf %u: parts %p name '%s' (want '%s'), idle's parts %p, frequency %.3f", c.weapon, L, parts,
				    pname && FastReadable(pname, 1) ? pname : "?", c.xanim, XaParts(xa, 1),
				    parts && FastReadable(parts, 0x58) ? *reinterpret_cast<const float *>(parts + 0x54) : -1.0f);
			}
			return false;
		}
		if ((!rootOurs && Engine().getInfo(tree, R)) || (!leafOurs && Engine().getInfo(tree, L)))
		{
			// An info made under the old parent: let it go (weight 0 frees it on the advance) and try next frame.
			Engine().setGoal(dobj, R, 0.0f, 0.0f, 1.0f, 0, 0, 0);
			Engine().setGoal(dobj, L, 0.0f, 0.0f, 1.0f, 0, 0, 0);
			run.why = "waiting for the old node infos to be freed";
			return false;
		}
		if (!rootOurs)
		{
			uint8_t *e = XaEntry(xa, R);
			*reinterpret_cast<uint16_t *>(e + 0x02) = 1;                                   // one child (a blend)
			*reinterpret_cast<uint64_t *>(e + 0x10) = 0x10ull | static_cast<uint64_t>(L) << 16;  // additive, first child
			*reinterpret_cast<uint16_t *>(e + 0x08) = 0;                                   // under node 0, after 1..175
		}
		*reinterpret_cast<uint16_t *>(XaEntry(xa, L) + 0x08) = static_cast<uint16_t>(R);
		if (g_slotsDebug)
			Log("slots: %s: %s bound: root %u (additive, under 0) -> leaf %u (%s)", c.weapon, d.name, R, L, c.xanim);
		return true;
	}

	struct SlotFrame
	{
		int variant = -1;
		void *dobj = nullptr;
		uint8_t *tree = nullptr, *xa = nullptr;
		unsigned gen = 0, builds = 0;
		double last = -1;
		bool xaOk = false, writing = false;
		uint32_t roots[8];
		int rootCount = 0;
	} g_slotFrame;

	void SlotsVmFrame(uint8_t *ps, uint8_t *vm, double now)
	{
		SlotFrame &f = g_slotFrame;
		void *dobj = *reinterpret_cast<void **>(vm + kVmDObj);
		uint8_t *tree = dobj ? *reinterpret_cast<uint8_t **>(dobj) : nullptr;
		if (!tree || !FastReadable(tree, 16))
			return;
		uint8_t *xa = *reinterpret_cast<uint8_t **>(tree);
		const uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		const int variant = static_cast<int>(weapon & 0x1FF);
		float dt = f.last < 0 ? 0.0f : static_cast<float>(now - f.last);
		dt = dt < 0 ? 0 : dt > 0.1f ? 0.1f : dt;
		f.last = now;

		const bool fresh = variant != f.variant || dobj != f.dobj || tree != f.tree || xa != f.xa || f.gen != g_perfGen;
		if (fresh)
		{
			const bool newGun = variant != f.variant || dobj != f.dobj;
			f.variant = variant, f.dobj = dobj, f.tree = tree, f.xa = xa, f.gen = g_perfGen;
			f.xaOk = xa && XaLooksBuilt(xa);
			uint8_t *vdef = variant ? At<uint8_t *>(kWeaponVariants)[variant] : nullptr;
			static bool s_dumped[512];
			for (int a = 0; a < g_additiveCount; a++)
			{
				AdditiveConfig &c = g_additives[a];
				if (c.purpose < 0)
					continue;
				SlotRun &run = g_slotRun[a];
				if (newGun)
					run.w = 0;
				run.bound = false;
				run.waitLogged = false;
				if (c.variant != variant)
				{
					run.ok = false;
					continue;
				}
				const bool was = run.ok;
				const char *wasWhy = run.why;
				run.ok = f.xaOk && SlotEligible(c, run, weapon, vdef);
				if (!f.xaOk)
					run.why = "the viewmodel tree isn't laid out as the Enhanced build makes it";
				if (run.ok != was || (!run.ok && strcmp(wasWhy, run.why)) || g_slotsDebug)
					Log("slots: %s slot:%s (%s) %s%s%s", c.weapon, kSlotDefs[c.purpose].name, c.xanim, run.ok ? "ON" : "off",
					    run.ok ? "" : ": ", run.why);
				if (g_slotsDump && vdef && !s_dumped[variant])
				{
					s_dumped[variant] = true;
					DumpSlots(c.weapon, vdef);
				}
			}
		}

		// Per-line targets, own fades
		int lines[16], n = 0;
		for (int a = 0; a < g_additiveCount && n < 16; a++)
			if (g_additives[a].purpose >= 0 && g_slotRun[a].ok && g_additives[a].variant == variant)
				lines[n++] = a;
		if (!n)
		{
			if (f.writing)
				for (int r = 0; r < f.rootCount; r++)
					Engine().setGoal(dobj, f.roots[r], 0.0f, 0.0f, 1.0f, 0, 0, 0);
			f.writing = false;
			f.rootCount = 0;
			return;
		}
		const int state = *reinterpret_cast<int32_t *>(ps + kPsWeaponState);
		const int playing = *reinterpret_cast<int32_t *>(vm + kVmRightAnim);
		const float ads = Clamp01(*reinterpret_cast<float *>(ps + kPsAdsFraction));
		const bool reloading = Reloading(ps, vm);
		const bool meleeOff = g_additiveMeleeFade && MeleeViewState(state);  // additive_melee_fade=1 (bo3_additive.h)
		float leafT[16], leafW[16];
		f.rootCount = 0;
		for (int i = 0; i < n; i++)
		{
			AdditiveConfig &c = g_additives[lines[i]];
			SlotRun &run = g_slotRun[lines[i]];
			const SlotDef &d = kSlotDefs[c.purpose];
			// Checked every frame (four u16 reads): a rebuilt tree is back to the engine's layout, and driving the leaf
			// there would blend the additive into the base pose.
			if (run.bound && !(SlotRootOurs(xa, d.root) && SlotLeafOurs(xa, d.root, d.leaf)))
			{
				run.bound = false;
				run.waitLogged = false;
			}
			if (!run.bound)
			{
				run.bound = SlotBind(c, run, dobj, tree, xa);
				if (!run.bound && !run.waitLogged)
				{
					run.waitLogged = true;
					Log("slots: %s slot:%s waiting: %s", c.weapon, d.name, run.why);
				}
			}
			float target = 0, blend = 0.15f, t = 0;
			// This line's gun: the right one (the held weapon's clip, the right hand's state and node) or, for side:left,
			// the dual-wield left one (its own clip, weaponState ps+0x78 and viewmodel node vm+0x370).
			const bool left = c.side == kSideLeft;
			const int leftState = left ? *reinterpret_cast<int32_t *>(ps + kInsPsLeftWeaponState) : 0;
			const int leftNode = left ? *reinterpret_cast<int32_t *>(vm + kInsVmLeftNode) : 0;
			const int clip = left ? InspectHandClip(ps, weapon, 1) : ClipAmmo(ps, weapon);
			const bool sideReloading = left ? (leftState >= 12 && leftState <= 20) || leftNode == 171 || leftNode == 172 : reloading;
			if (c.kind == AdditiveKind::Empty)
			{
				// When it comes on after the last round: empty_lastshot (bo3_additive.h EmptyLastShotTarget), per side.
				blend = 0.1f;
				// Into the reload: the layer follows the reload anim's weight out (bo3_additive.h EmptyReloadHandoff).
				if (clip == 0 && sideReloading)
				{
					const int reloadNode = left ? (leftNode == 171 || leftNode == 172 ? leftNode : -1) : (playing >= 40 && playing <= 45 ? playing : -1);
					target = EmptyReloadHandoff(c, dobj, reloadNode, now, blend);
				}
				else
				{
					c.elReload = -1;
					target = clip == 0 ? EmptyLastShotTarget(c, dobj, variant, left, left ? leftNode : playing, now, blend) : 0.0f;
				}
				if (target != c.lastTarget && (target == 0 || target == c.weight || c.lastTarget == 0 || c.lastTarget == c.weight))
					HotLog("slots: %s empty%s: clip %d node %d -> weight %.3f (blend %.2f) t %.3f", c.weapon, left ? " (left)" : "", clip,
					       left ? leftNode : playing, target, blend, now);
				c.lastTarget = target;
				t = 0.999f;  // the pose is the last frame (frame 0 is the reference)
			}
			else if (c.kind == AdditiveKind::Bullet)
			{
				// RELOADVIEW (bo3_additive.h ShownClip): the right hand's mag keeps its rounds through a reload until the
				// gramien notes / the ammo add say otherwise; the left hand keeps the old rule.
				const int shown = clip < 0 || left ? clip : ShownClip(ps, vm, clip);
				int k = c.magSize - (shown < 0 ? c.magSize : shown > c.magSize ? c.magSize : shown);
				k = k < 0 ? 0 : k > c.magSize ? c.magSize : k;
				t = static_cast<float>(k + 1) / static_cast<float>(c.magSize + 1);
				t = t > 0.999f ? 0.999f : t;
				target = (sideReloading && (left || !g_ammoHideReload)) || clip < 0 ? 0.0f : c.weight;
			}
			else  // recoil: scrubbed by the burst (as ApplyAdditives); recoil_ads runs at ADS, the old recoil at the hip
			{
				const bool newShot = c.lastShotCount >= 0 && clip >= 0 && clip < c.lastShotCount;
				c.lastShotCount = clip;
				if (newShot)
				{
					if (now - c.lastShot > 0.2)
						c.burstStart = now;
					c.lastShot = now;
				}
				const bool firing = now - c.lastShot < 0.2;
				const double held = (firing ? now : c.lastShot) - c.burstStart;
				t = run.bound ? static_cast<float>(held * NodeFrequency(dobj, d.leaf) * c.rate) : 0.0f;
				t = t < 0 ? 0.0f : t > 0.999f ? 0.999f : t;
				target = firing ? c.weight * (&d == FindSlotDef("recoil_ads") ? ads : 1.0f - ads) : 0.0f;
			}
			if (!g_additiveEnable || !run.bound || meleeOff)
				target = 0;
			const float before = run.w;
			run.w = Approach(run.w, target, blend > 0 ? dt / blend : 1.0f);
			if (g_slotsDebug && (before < 0.001f) != (run.w < 0.001f))
				Log("slots: %s slot:%s %s (clip %d state %d playing %d ads %.2f)", c.weapon, d.name, run.w >= 0.001f ? "on" : "off", clip,
				    left ? leftState : state, left ? leftNode : playing, ads);
			leafW[i] = run.bound ? run.w : 0.0f;
			leafT[i] = t;
			static double s_nextTrace[kMaxAdditives];
			if (g_slotsDebug && now >= s_nextTrace[lines[i]])
			{
				s_nextTrace[lines[i]] = now + 1.0;
				Log("slots: trace %s slot:%s side %s: clip %d state %d node %d -> target %.2f weight %.2f t %.3f%s", c.weapon, d.name,
				    left ? "left" : "right", clip, left ? leftState : state, left ? leftNode : playing, target, run.w, t,
				    run.bound ? "" : " (not bound)");
			}
			bool seen = false;
			for (int r = 0; r < f.rootCount; r++)
				seen |= f.roots[r] == d.root;
			if (!seen && f.rootCount < 8 && run.bound)
				f.roots[f.rootCount++] = d.root;
		}
		// Write: each root at the strongest of its leaves (leaves under one root are normalised, so a lone leaf is full),
		// each leaf at its own weight, rate 0, its time set.
		bool any = false;
		for (int r = 0; r < f.rootCount; r++)
		{
			float rootW = 0;
			for (int i = 0; i < n; i++)
				if (kSlotDefs[g_additives[lines[i]].purpose].root == f.roots[r])
					rootW = leafW[i] > rootW ? leafW[i] : rootW;
			Engine().setGoal(dobj, f.roots[r], rootW < 0.001f ? 0.0f : rootW, 0.0f, 1.0f, 0, 0, 0);
			any |= rootW >= 0.001f;
		}
		for (int i = 0; i < n; i++)
		{
			const SlotDef &d = kSlotDefs[g_additives[lines[i]].purpose];
			if (!g_slotRun[lines[i]].bound)
				continue;
			Engine().setGoal(dobj, d.leaf, leafW[i] < 0.001f ? 0.0f : leafW[i], 0.0f, 0.0f, 0, 0, 0);
			if (leafW[i] >= 0.001f)
				LocoSetTime(dobj, d.leaf, leafT[i]);
		}
		f.writing = any;
	}
}
