// MW19-style viewmodel locomotion for BO3 weapons (included at the end of bo3_additive.h; runs in its hook after
// CG_UpdateViewWeaponAnim, i.e. after the engine's own walk / juke writes and before the tree advances).
// Research: the author's research notes (walk, bob, jukes, IW8 jog rules).
//
// Nodes (viewmodel XAnimTree, node = weapAnimFiles slot):
//   walk   additive root 182 -> leaf 183 (walkAnim). The engine sets both weights every frame (0x1404E4F00) and
//          the leaf time to bobCycle / 256, i.e. the whole anim once per stride. Here the leaf is held at rate 0
//          and its time is set from the bob with generations: t = (gen * 256 + bob) / (strides * 256), gen counting
//          bob wraps, so an MW19 4-stride walk_loop runs at the bob's cadence and footsteps stay in sync.
//   jog    additive root 184 -> leaf 185..192 (the juke group, jukeLeftAnim = 185; 0x1404E2980 forces them to 0 every
//          frame unless juking). Visual only: it is crossfaded with the walk (182 = move * (1 - j), 184 = move * j,
//          move = the engine's walk weight) and shares the walk's phase.
//   idle active  IW8's hip idle additive (WEAP_ANIM_ADDITIVE_HIP_IDLE, cg_weapons.cpp PlayAdditiveHipIdleAnim) on a
//          spare additive pair, 193 -> 194 by default (dead swim group; also used by additive= on three weapons,
//          which is refused per weapon). Weight 1 at the hip, 0 from any ADS, 0.2 s linear blend, looping at the
//          anim's own frame rate and restarted on each fade-in, as IW8 does. BO3's idle (node 1) is not touched.
//
// Config (stub_boot.cfg / bo3_additive.cfg), per weapon; a weapon with no lines of its own takes its
// wop_alias / locomotion_alias source's:
//   locomotion=<weapon>,walk,bob,<strides>[,<rate>]
//       walkAnim (183) locked to the bob cycle, <strides> bob cycles per loop (4 for MW19 walk_loop);
//       <rate> scales the phase (1 keeps footstep sync)
//   locomotion=<weapon>,jog,<leaf 185..192>[,<weight>[,<rate>[,<strides>]]]
//       the jog loop in that juke leaf (185 = jukeLeftAnim), same phase as the walk, <strides> per loop (default 4)
//   locomotion_alias=<weapon>,<source weapon>
//   idle_active=<weapon>,<xanim|*>,<leaf 194|196|185..192>[,<weight>[,<rate>]]
//       <xanim> is written into the leaf's slot (as additive= does; the tree picks it up on the next weapon raise),
//       * keeps the slot's own anim. Refused when the root is used by the weapon's additive= or jog lines.
// Globals:
//   locomotion_jog=<start speed frac>,<keep speed frac>,<start angle>,<keep angle>,<blend s>,<ads blend s>,<fire blend s>
//       jog when standing, weapon ready, not ADS / sprinting / juking, speed >= start (keep) x the player's max
//       speed and moving within start (keep) degrees of the view's forward; blend in/out, faster when leaving for
//       ADS or firing. Default 0.6,0.5,30,40,0.3,0.15,0.15 (IW8 uses 75 degrees, which lets diagonals jog)
//   idle_active_fade=<iw8|linear>[,<blend s>]   iw8: on only at the hip (ADS < 0.001), blended; linear: x (1 - ADS)
//   locomotion_debug=1                          state changes and a 2 Hz trace in the log
//   locomotion_enable=0|1, idle_active_enable=0|1  A/B switches (live; default 1): everything here off / idle active off
#pragma once

namespace
{
	constexpr size_t kPsPmType = 0x08, kPsBobCycle = 0x0C, kPsPmFlags = 0x10, kPsVelocity = 0x3C, kPsSpeed = 0xCC;
	constexpr size_t kPsViewAngles = 0x318, kPsMoveSpeedScale = 0x7D4;
	uintptr_t kIsDualWield = 0x27CF860;  // BG_IsDualWield(Weapon) (bo3_inspect.h kInsIsDualWield refers to it)
	constexpr uint32_t kWalkRoot = 182, kWalkLeaf = 183, kJukeRoot = 184;
	constexpr float kReadyHold = 0.1f;  // weapon ready this long before a jog may start (auto fire flickers to 0)

	// Engine calls, through pointers so the offline harness can stand in for them.
	struct LocoEngine
	{
		uint32_t (*getInfo)(void *tree, uint32_t node);
		SetGoalWeightFn setGoal;
		void (*syncTime)(uint32_t info, float time);
		bool (*isDualWield)(uint64_t weapon);
	} g_locoEngine;

	LocoEngine &Engine()
	{
		if (!g_locoEngine.getInfo)
		{
			g_locoEngine.getInfo = &XGetInfo;
			g_locoEngine.setGoal = reinterpret_cast<SetGoalWeightFn>(g_base + kSetGoalWeight);
			g_locoEngine.syncTime = reinterpret_cast<void (*)(uint32_t, float)>(g_base + kSyncTime);
			g_locoEngine.isDualWield = reinterpret_cast<bool (*)(uint64_t)>(g_base + kIsDualWield);
		}
		return g_locoEngine;
	}

	struct LocoWeapon
	{
		char weapon[64];
		bool walk = false, jog = false, idle = false;
		int walkStrides = 4, jogStrides = 4;
		float walkRate = 1, jogRate = 1, jogWeight = 1;
		uint32_t jogNode = 185, idleNode = 194;
		char idleXanim[96] = {};  // "*" = the slot's own anim
		float idleWeight = 1, idleRate = 1;
		bool idleRefused = false;  // its root is taken by this weapon's additive= or jog
		bool warnedJog = false, warnedIdle = false;
		int variant = -1;  // for the idle_active slot patch
	};
	constexpr int kMaxLocos = 160;
	LocoWeapon g_locos[kMaxLocos];
	int g_locoCount;
	struct LocoAlias
	{
		char weapon[64], source[64];
	};
	LocoAlias g_locoAliases[kMaxLocos];
	int g_locoAliasCount;
	bool g_locoFinished;

	struct LocoJogParams
	{
		float start = 0.6f, keep = 0.5f, startAngle = 30, keepAngle = 40, blend = 0.3f, adsBlend = 0.15f, fireBlend = 0.15f;
	} g_jogParams;
	bool g_idleFadeLinear;
	float g_idleBlend = 0.2f;
	bool g_locoDebug;

	LocoWeapon *FindLoco(const char *weapon, bool create)
	{
		for (int i = 0; i < g_locoCount; i++)
			if (strcmp(g_locos[i].weapon, weapon) == 0)
				return &g_locos[i];
		if (!create || g_locoCount >= kMaxLocos)
			return nullptr;
		LocoWeapon &w = g_locos[g_locoCount++];
		w = LocoWeapon{};
		strcpy_s(w.weapon, weapon);
		return &w;
	}

	// Splits "a,b,c  # comment" into trimmed fields. Returns the field count.
	int SplitFields(const char *value, char out[][96], int max)
	{
		char buf[256];
		strncpy_s(buf, value, _TRUNCATE);
		if (char *hash = strchr(buf, '#'))
			*hash = 0;
		int n = 0;
		for (char *p = buf; n < max;)
		{
			size_t len = strcspn(p, ",");
			char *start = p, *end = p + len;
			while (start < end && (*start == ' ' || *start == '\t'))
				start++;
			while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
				end--;
			size_t keep = static_cast<size_t>(end - start) < 95 ? end - start : 95;
			memcpy(out[n], start, keep);
			out[n++][keep] = 0;
			if (!p[len])
				break;
			p += len + 1;
		}
		return n;
	}

	bool ParseFloat(const char *s, float &v) { return sscanf_s(s, "%f", &v) == 1; }
	bool ParseInt(const char *s, int &v) { return sscanf_s(s, "%d", &v) == 1; }
	uint32_t IdleRoot(uint32_t leaf) { return leaf <= 192 ? kJukeRoot : leaf - 1; }

	void RecordLocoAlias(const char *weapon, const char *source)
	{
		if (g_locoAliasCount >= kMaxLocos || !weapon[0] || !source[0])
			return;
		LocoAlias &a = g_locoAliases[g_locoAliasCount++];
		strcpy_s(a.weapon, weapon);
		strcpy_s(a.source, source);
	}

	// locomotion_jog= / idle_active_fade= / locomotion_debug= (also re-read live by ReloadAdditiveTuning).
	bool ParseLocomotionGlobal(const char *line, bool &known)
	{
		known = true;
		char f[8][96];
		if (strncmp(line, "locomotion_jog=", 15) == 0)
		{
			LocoJogParams p;
			float *v[] = {&p.start, &p.keep, &p.startAngle, &p.keepAngle, &p.blend, &p.adsBlend, &p.fireBlend};
			if (SplitFields(line + 15, f, 8) != 7)
				return false;
			for (int i = 0; i < 7; i++)
				if (!ParseFloat(f[i], *v[i]) || *v[i] < 0)
					return false;
			if (p.keep > p.start || p.keepAngle < p.startAngle || p.blend <= 0 || p.adsBlend <= 0 || p.fireBlend <= 0)
				return false;
			g_jogParams = p;
			return true;
		}
		if (strncmp(line, "idle_active_fade=", 17) == 0)
		{
			int n = SplitFields(line + 17, f, 3);
			float blend = 0.2f;
			if (n < 1 || n > 2 || (strcmp(f[0], "iw8") != 0 && strcmp(f[0], "linear") != 0) ||
			    (n == 2 && (!ParseFloat(f[1], blend) || blend <= 0)))
				return false;
			g_idleFadeLinear = strcmp(f[0], "linear") == 0;
			g_idleBlend = blend;
			return true;
		}
		if (strncmp(line, "locomotion_debug=", 17) == 0)
		{
			int on;
			if (!ParseInt(line + 17, on))
				return false;
			g_locoDebug = on != 0;
			return true;
		}
		// the A/B switches (bo3_additive.h)
		if (strncmp(line, "locomotion_enable=", 18) == 0 || strncmp(line, "idle_active_enable=", 19) == 0)
		{
			const bool loco = line[1] == 'o';
			int on;
			if (!ParseInt(line + (loco ? 18 : 19), on))
				return false;
			(loco ? g_locoEnable : g_idleActiveEnable) = on != 0;
			return true;
		}
		known = false;
		return true;
	}

	// locomotion=, locomotion_alias=, idle_active= and the globals. `known` is false for other keys.
	bool ParseLocomotionLine(const char *line, bool &known)
	{
		bool ok = ParseLocomotionGlobal(line, known);
		if (known)
			return ok;
		known = true;
		char f[8][96];
		if (strncmp(line, "locomotion_alias=", 17) == 0)
		{
			if (SplitFields(line + 17, f, 3) != 2 || !f[0][0] || !f[1][0])
				return false;
			RecordLocoAlias(f[0], f[1]);
			return true;
		}
		if (strncmp(line, "locomotion=", 11) == 0)
		{
			int n = SplitFields(line + 11, f, 8);
			if (n < 3 || !f[0][0] || strlen(f[0]) >= 64)
				return false;
			LocoWeapon tmp{};
			if (strcmp(f[1], "walk") == 0)
			{
				// walk,bob,<strides>[,<rate>]
				if (n < 4 || n > 5 || strcmp(f[2], "bob") != 0 || !ParseInt(f[3], tmp.walkStrides) || tmp.walkStrides < 1 ||
				    tmp.walkStrides > 64 || (n == 5 && (!ParseFloat(f[4], tmp.walkRate) || tmp.walkRate <= 0)))
					return false;
				LocoWeapon *w = FindLoco(f[0], true);
				if (!w)
					return false;
				w->walk = true;
				w->walkStrides = tmp.walkStrides;
				w->walkRate = tmp.walkRate;
				return true;
			}
			if (strcmp(f[1], "jog") == 0)
			{
				// jog,<leaf>[,<weight>[,<rate>[,<strides>]]]
				int node;
				if (n > 6 || !ParseInt(f[2], node) || node < 185 || node > 192 ||
				    (n >= 4 && (!ParseFloat(f[3], tmp.jogWeight) || tmp.jogWeight < 0 || tmp.jogWeight > 2)) ||
				    (n >= 5 && (!ParseFloat(f[4], tmp.jogRate) || tmp.jogRate <= 0)) ||
				    (n >= 6 && (!ParseInt(f[5], tmp.jogStrides) || tmp.jogStrides < 1 || tmp.jogStrides > 64)))
					return false;
				LocoWeapon *w = FindLoco(f[0], true);
				if (!w)
					return false;
				w->jog = true;
				w->jogNode = static_cast<uint32_t>(node);
				w->jogWeight = tmp.jogWeight;
				w->jogRate = tmp.jogRate;
				w->jogStrides = tmp.jogStrides;
				return true;
			}
			return false;
		}
		if (strncmp(line, "idle_active=", 12) == 0)
		{
			// <weapon>,<xanim|*>,<leaf>[,<weight>[,<rate>]]
			LocoWeapon tmp{};
			int n = SplitFields(line + 12, f, 8), node;
			if (n < 3 || n > 5 || !f[0][0] || strlen(f[0]) >= 64 || !f[1][0] || !ParseInt(f[2], node) ||
			    !(node == 194 || node == 196 || (node >= 185 && node <= 192)) ||
			    (n >= 4 && (!ParseFloat(f[3], tmp.idleWeight) || tmp.idleWeight < 0 || tmp.idleWeight > 2)) ||
			    (n >= 5 && (!ParseFloat(f[4], tmp.idleRate) || tmp.idleRate <= 0)))
				return false;
			LocoWeapon *w = FindLoco(f[0], true);
			if (!w)
				return false;
			w->idle = true;
			strcpy_s(w->idleXanim, f[1]);
			w->idleNode = static_cast<uint32_t>(node);
			w->idleWeight = tmp.idleWeight;
			w->idleRate = tmp.idleRate;
			return true;
		}
		known = false;
		return true;
	}

	// Live reload of an idle_active= line: only its weight and rate change (the xanim and leaf need a restart).
	void LiveIdleActiveLine(const char *line)
	{
		char f[8][96];
		int n = SplitFields(line + 12, f, 8);
		LocoWeapon *w = n >= 3 ? FindLoco(f[0], false) : nullptr;
		if (!w || !w->idle)
			return;
		float weight = 1, rate = 1;
		if ((n >= 4 && (!ParseFloat(f[3], weight) || weight < 0 || weight > 2)) || (n >= 5 && (!ParseFloat(f[4], rate) || rate <= 0)))
		{
			Log("locomotion: live reload: bad line '%s'", line);
			return;
		}
		if (w->idleWeight != weight || w->idleRate != rate)
			Log("locomotion: live idle_active %s weight %.2f rate %.2f", w->weapon, weight, rate);
		w->idleWeight = weight;
		w->idleRate = rate;
	}

	// After the whole cfg is read: aliases take their source's lines (chains resolve over a few passes), then the
	// idle_active roots are checked against the same weapon's additive= and jog lines.
	void FinishLocomotionConfig()
	{
		if (g_locoFinished)
			return;
		g_locoFinished = true;
		for (int pass = 0; pass < 4; pass++)
		{
			bool added = false;
			for (int a = 0; a < g_locoAliasCount; a++)
			{
				const LocoAlias &al = g_locoAliases[a];
				const LocoWeapon *src = FindLoco(al.source, false);
				if (!src || FindLoco(al.weapon, false))
					continue;
				LocoWeapon copy = *src;
				LocoWeapon *w = FindLoco(al.weapon, true);
				if (!w)
					break;
				*w = copy;
				strcpy_s(w->weapon, al.weapon);
				w->variant = -1;
				added = true;
			}
			if (!added)
				break;
		}
		for (int i = 0; i < g_locoCount; i++)
		{
			LocoWeapon &w = g_locos[i];
			if (!w.idle)
				continue;
			uint32_t root = IdleRoot(w.idleNode);
			if (root == kJukeRoot && w.jog)
			{
				w.idleRefused = true;
				Log("locomotion: idle_active %s: leaf %u shares root 184 with its jog; refused (use 194 or 196)", w.weapon, w.idleNode);
				continue;
			}
			for (int a = 0; a < g_additiveCount; a++)
				if (g_additives[a].root == root && strcmp(g_additives[a].weapon, w.weapon) == 0)
				{
					w.idleRefused = true;
					Log("locomotion: idle_active %s: root %u is used by its additive= line (%s); refused", w.weapon, root,
					    g_additives[a].xanim);
					break;
				}
		}
		int walk = 0, jog = 0, idle = 0;
		for (int i = 0; i < g_locoCount; i++)
		{
			walk += g_locos[i].walk;
			jog += g_locos[i].jog;
			idle += g_locos[i].idle && !g_locos[i].idleRefused;
		}
		if (g_locoCount)
			Log("locomotion: %d weapon(s): %d walk, %d jog, %d idle_active; jog %.2f/%.2f speed, %.0f/%.0f deg, blend %.2f/%.2f/%.2f s; "
			    "idle_active fade %s %.2f s",
			    g_locoCount, walk, jog, idle, g_jogParams.start, g_jogParams.keep, g_jogParams.startAngle, g_jogParams.keepAngle,
			    g_jogParams.blend, g_jogParams.adsBlend, g_jogParams.fireBlend, g_idleFadeLinear ? "linear" : "iw8", g_idleBlend);
	}

	bool LocomotionConfigured() { return g_locoCount > 0; }

	// Writes each idle_active xanim into its variant's slot (the viewmodel tree reads slot names when it is built on
	// a weapon raise). One pass over the variants (bo3_perf.h, which also patches right before each tree build).
	void PatchLocomotionSlots()
	{
		FinishLocomotionConfig();
		ResolveKinds(1u << kSubLoco);
	}

	// ---- tree access -------------------------------------------------------------------------------------------
	uint8_t *LocoInfo(void *dobj, uint32_t node)
	{
		void *tree = *reinterpret_cast<void **>(dobj);
		uint32_t i = tree ? Engine().getInfo(tree, node) : 0;
		return i ? At<uint8_t>(kXAnimInfo + 0x58 * i) : nullptr;
	}

	// The engine's own "set time" idiom (as SetNodeTime), through LocoEngine.
	void LocoSetTime(void *dobj, uint32_t node, float t)
	{
		void *tree = *reinterpret_cast<void **>(dobj);
		uint32_t i = tree ? Engine().getInfo(tree, node) : 0;
		if (!i)
			return;
		uint8_t *info = At<uint8_t>(kXAnimInfo + 0x58 * i);
		*reinterpret_cast<float *>(info + 0x10) = t;
		*reinterpret_cast<float *>(info + 0x14) = t;
		*reinterpret_cast<int32_t *>(info + 0x28) = 0;
		*reinterpret_cast<uint16_t *>(info + 0x3A) = 0xFFFF;
		Engine().syncTime(i, t);
	}

	// XAnim_s (XAnimTree +0): entry n at +0x28 + 0x18 n, +0x02 u16 child count (0 = leaf), +0x10 XAnimParts*.
	// (rc: resident pages from the working set, else one VirtualQuery per memory region for a batch of these calls;
	// nullptr = ask every time)
	bool LocoReadable(ReadCache *rc, const void *p, size_t n) { return rc ? rc->Readable(p, n) : FastReadable(p, n); }
	const uint8_t *LeafParts(void *dobj, uint32_t node, ReadCache *rc = nullptr)
	{
		auto *tree = *reinterpret_cast<uint8_t **>(dobj);
		const uint8_t *xa = tree && LocoReadable(rc, tree, 8) ? *reinterpret_cast<uint8_t **>(tree) : nullptr;
		const uint8_t *entry = xa ? xa + 0x28 + 0x18 * node : nullptr;
		if (!entry || !LocoReadable(rc, entry, 0x18) || *reinterpret_cast<const uint16_t *>(entry + 2) != 0)
			return nullptr;
		return *reinterpret_cast<const uint8_t *const *>(entry + 0x10);
	}

	// 1 / length in seconds of a leaf's anim, 0 when missing (an unloaded xanim is a default clone with no frames).
	float LeafFrequency(void *dobj, uint32_t node, ReadCache *rc = nullptr)
	{
		const uint8_t *parts = LeafParts(dobj, node, rc);
		return parts && LocoReadable(rc, parts, 0x58) ? *reinterpret_cast<const float *>(parts + 0x54) : 0.0f;
	}

	// A leaf is usable as an additive when it holds a real anim: a blank slot is built with the IDLE (tree build
	// 0x1404EC2D0), and the idle added on top of the pose would wreck it.
	bool UsableAdditiveLeaf(void *dobj, uint32_t node, ReadCache *rc = nullptr)
	{
		const uint8_t *parts = LeafParts(dobj, node, rc);
		return parts && parts != LeafParts(dobj, 1, rc) && LeafFrequency(dobj, node, rc) > 0.0f;
	}

	float Approach(float v, float target, float step)
	{
		return v < target ? (v + step > target ? target : v + step) : (v - step < target ? target : v - step);
	}
	float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

	// ---- per-frame state ------------------------------------------------------------------------------------------
	struct LocoState
	{
		int variant = -1;
		void *dobj = nullptr, *tree = nullptr;
		LocoWeapon *cfg = nullptr;
		bool jogOk = false, idleOk = false;  // the tree's leaves hold real anims
		float idleFreq = 0;                 // the idle_active leaf's frequency, read when idleOk is
		int recheck = 0;
		unsigned builds = 0;                // g_treeBuilds when the leaves were last looked at
		int prevBob = -1, gen = 0;
		double phase = 0;  // strides (bob cycles), shared by walk and jog
		double last = -1, lastBusy = -1e9, nextTrace = 0;
		float move = 0;    // the engine's walk weight (tracked: 182 is ours while jogging)
		float j = 0;       // jog blend
		bool jogWant = false, driving = false;
		float idleGate = 0, idleW = 0;
		double idlePhase = 0;
		bool idleOn = false;
		const char *jogWhy = "";
		float walkT = 0, jogT = 0;
	} g_loco;

	// The locomotion lines of the weapon variant's name (a hash lookup on the name; bo3_perf.h).
	LocoWeapon *FindLocoForVariant(int variant)
	{
		ReadCache rc;
		int i = FindConfigByVariantName(variant, kSubLoco, rc);
		return i >= 0 ? &g_locos[i] : nullptr;
	}

	// Hands the walk root back to the engine at the weight it last had from us.
	void ReleaseWalkRoot(LocoState &s, bool write)
	{
		if (s.driving && write && LocoInfo(s.dobj, kWalkRoot))
			Engine().setGoal(s.dobj, kWalkRoot, s.move, 0.0f, 1.0f, 0, 0, 0);
		s.driving = false;
		s.j = 0;
		s.jogWant = false;
	}

	void ApplyLocomotion(uint8_t *ps, uint8_t *vm, double now)
	{
		if (!g_locoCount)
			return;
		FinishLocomotionConfig();
		LocoState &s = g_loco;
		void *dobj = *reinterpret_cast<void **>(vm + kVmDObj);
		void *tree = dobj ? *reinterpret_cast<void **>(dobj) : nullptr;
		if (!tree)
		{
			s.variant = -1;
			return;
		}
		if (!g_locoEnable)
		{
			// locomotion_enable=0: hand everything back once (the walk root and its leaf's rate to the engine, idle active
			// to 0), then leave the tree alone until it is turned on again (which starts over as a weapon change does)
			if (s.variant >= 0 && dobj == s.dobj && tree == s.tree)
			{
				ReleaseWalkRoot(s, true);
				if (s.idleOn && s.cfg)
					Engine().setGoal(dobj, IdleRoot(s.cfg->idleNode), 0.0f, 0.0f, 1.0f, 0, 0, 0);
				if (s.cfg && s.cfg->walk)
					if (uint8_t *leaf = LocoInfo(dobj, kWalkLeaf))
						*reinterpret_cast<float *>(leaf + 0x24) = 1.0f;
			}
			s.idleOn = false;
			s.variant = -1;
			s.last = -1;
			return;
		}
		float dt = s.last < 0 ? 0.0f : static_cast<float>(now - s.last);
		dt = dt < 0 ? 0 : dt > 0.1f ? 0.1f : dt;
		s.last = now;
		uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		int variant = static_cast<int>(weapon & 0x1FF);

		// A weapon change (or a rebuilt tree) starts every blend over; the old tree's nodes are gone.
		if (variant != s.variant || dobj != s.dobj || tree != s.tree)
		{
			bool wasDriving = s.driving;
			s.driving = false;
			s.variant = variant;
			s.dobj = dobj;
			s.tree = tree;
			s.cfg = FindLocoForVariant(variant);
			s.j = 0;
			s.jogWant = false;
			s.idleGate = s.idleW = 0;
			s.idleOn = false;
			s.idlePhase = 0;
			s.lastBusy = now;
			uint8_t *root = LocoInfo(dobj, kWalkRoot);
			s.move = root ? *reinterpret_cast<float *>(root + 0x20) : 0.0f;
			LocoWeapon *c = s.cfg;
			ReadCache rc;
			s.jogOk = c && c->jog && UsableAdditiveLeaf(dobj, c->jogNode, &rc);
			s.idleOk = c && c->idle && !c->idleRefused && UsableAdditiveLeaf(dobj, c->idleNode, &rc);
			s.idleFreq = s.idleOk ? LeafFrequency(dobj, c->idleNode, &rc) : 0.0f;
			s.recheck = 0;
			s.builds = g_treeBuilds;
			if (c && c->jog && !s.jogOk && !c->warnedJog)
			{
				c->warnedJog = true;
				Log("locomotion: %s: jog leaf %u holds no anim of its own (blank slot = idle, or not in the zone); jog off", c->weapon,
				    c->jogNode);
			}
			if (c && c->idle && !c->idleRefused && !s.idleOk && !c->warnedIdle)
			{
				c->warnedIdle = true;
				Log("locomotion: %s: idle_active leaf %u holds no anim of its own yet ('%s'; not in the zone, or the slot patch "
				    "lands on the next raise); idle_active off for this raise", c->weapon, c->idleNode, c->idleXanim);
			}
			static int s_logs;
			if (c && s_logs++ < 200)
				Log("locomotion: holding %s (variant %d)%s: walk %s, jog %s, idle_active %s%s", c->weapon, variant,
				    wasDriving ? ", jog reset" : "", c->walk ? "bob-locked" : "engine", s.jogOk ? "on" : "off",
				    s.idleOk ? "on" : "off", s.idleOk && g_idleFadeLinear ? " (linear fade)" : "");
		}

		// Bob phase in strides: gen counts wraps (either way, so a prediction correction across the wrap unwinds). Held
		// while stopped: the engine resets the bob to 0 below 1 u/s and fades the walk with its last pose.
		const float *vel = reinterpret_cast<const float *>(ps + kPsVelocity);
		float xyspeed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
		int bob = *reinterpret_cast<int32_t *>(ps + kPsBobCycle) & 0xFF;
		if (s.prevBob >= 0)
		{
			int d = bob - s.prevBob;
			if (d < -128)
				s.gen++;
			else if (d > 128)
				s.gen--;
			s.gen &= (1 << 20) - 1;
		}
		s.prevBob = bob;
		if (!(bob == 0 && xyspeed < 2.0f))
			s.phase = s.gen + bob / 256.0;

		LocoWeapon *c = s.cfg;
		if (!c)
			return;
		// The tree may be rebuilt a frame after the weapon value changes, and an idle_active slot patch only lands on
		// a later raise: look at the leaves again now and then (a few VirtualQuery calls). With the tree-build hook in
		// (bo3_perf.h), right after each build, and every 240 frames as a fallback.
		bool recheck = ++s.recheck % 30 == 0;
		if (g_treeHookLive)
			recheck = s.builds != g_treeBuilds || s.recheck % 240 == 0;
		if (recheck)
		{
			s.builds = g_treeBuilds;
			ReadCache rc;
			bool jogOk = c->jog && UsableAdditiveLeaf(dobj, c->jogNode, &rc);
			bool idleOk = c->idle && !c->idleRefused && UsableAdditiveLeaf(dobj, c->idleNode, &rc);
			s.idleFreq = idleOk ? LeafFrequency(dobj, c->idleNode, &rc) : 0.0f;
			if (jogOk != s.jogOk || idleOk != s.idleOk)
				Log("locomotion: %s: leaves now: jog %s, idle_active %s", c->weapon, jogOk ? "on" : "off", idleOk ? "on" : "off");
			if (!jogOk && s.jogOk)
				ReleaseWalkRoot(s, true);
			if (!idleOk && s.idleOn)
			{
				Engine().setGoal(dobj, IdleRoot(c->idleNode), 0.0f, 0.0f, 1.0f, 0, 0, 0);
				s.idleOn = false;
				s.idleGate = s.idleW = 0;
			}
			s.jogOk = jogOk;
			s.idleOk = idleOk;
		}

		uint64_t pmFlags = *reinterpret_cast<uint64_t *>(ps + kPsPmFlags);
		int pmType = *reinterpret_cast<int32_t *>(ps + kPsPmType);
		int state = *reinterpret_cast<int32_t *>(ps + kPsWeaponState);
		int playing = *reinterpret_cast<int32_t *>(vm + kVmRightAnim);
		float ads = Clamp01(*reinterpret_cast<float *>(ps + kPsAdsFraction));
		bool juke = (pmFlags & 0x40) != 0;
		// Hard stand-down: juking (the engine drives 184..192 then), dead / last stand / linked (pm_type), dual wield.
		const char *down = juke ? "juking" : pmType != 0 ? "pm_type" : Engine().isDualWield(weapon) ? "dual wield" : nullptr;
		if (state != 0)
			s.lastBusy = now;

		// ---- walk: 183 locked to the bob -------------------------------------------------------------------------
		if (c->walk)
		{
			double p = fmod(s.phase * c->walkRate, static_cast<double>(c->walkStrides));
			s.walkT = static_cast<float>(p / c->walkStrides);
			if (s.walkT > 0.9999f)
				s.walkT = 0.9999f;
			if (!down)
				if (uint8_t *leaf = LocoInfo(dobj, kWalkLeaf))
				{
					*reinterpret_cast<float *>(leaf + 0x24) = 0.0f;  // rate 0: the advance would add a frame's worth
					LocoSetTime(dobj, kWalkLeaf, s.walkT);
				}
		}

		// ---- jog: crossfaded with the walk, same phase ------------------------------------------------------------
		uint8_t *walkRoot = LocoInfo(dobj, kWalkRoot);
		float engineGoal = walkRoot ? *reinterpret_cast<float *>(walkRoot + 0x1C) : 0.0f;  // written by the engine this frame
		float moveStep = dt / (engineGoal > s.move ? 0.10f : 0.15f);  // the engine's own walk blend times
		s.move += (engineGoal - s.move) * (moveStep > 1 ? 1 : moveStep);
		if (s.jogOk)
		{
			if (down)
				ReleaseWalkRoot(s, !juke);
			else
			{
				float speedMax = *reinterpret_cast<int32_t *>(ps + kPsSpeed) * *reinterpret_cast<float *>(ps + kPsMoveSpeedScale);
				if (!(speedMax > 1.0f))
					speedMax = 190.0f;
				float frac = xyspeed / speedMax;
				float viewYaw = *reinterpret_cast<float *>(ps + kPsViewAngles + 4);
				float moveYaw = atan2f(vel[1], vel[0]) * 57.29578f;
				float angle = fabsf(Iw8AngleNormalize180(moveYaw - viewYaw));
				bool sprinting = playing >= 84 && playing <= 95;
				bool ready = state == 0 && now - s.lastBusy >= kReadyHold;
				const LocoJogParams &g = g_jogParams;
				const char *why = (pmFlags & 7) ? "not standing"
				                  : state != 0    ? "weapon busy"
				                  : !ready        ? "weapon settling"
				                  : ads > 0.001f  ? "ADS"
				                  : sprinting     ? "sprinting"
				                  : frac < (s.jogWant ? g.keep : g.start) ? "slow"
				                  : angle > (s.jogWant ? g.keepAngle : g.startAngle) ? "direction"
				                                                                     : nullptr;
				bool want = !why;
				float blend = want ? g.blend : ads > 0.001f ? g.adsBlend : (state != 0 || !ready) ? g.fireBlend : g.blend;
				if (want != s.jogWant && g_locoDebug)
					Log("locomotion: jog %s (%s): speed %.2f of %.0f, %.0f deg off forward, state %d, ads %.2f, j %.2f", want ? "ON" : "off",
					    want ? "forward, full speed" : why, frac, speedMax, angle, state, ads, s.j);
				s.jogWant = want;
				s.jogWhy = why ? why : "jogging";
				s.j = Approach(s.j, want ? 1.0f : 0.0f, dt / blend);
				double p = fmod(s.phase * c->jogRate, static_cast<double>(c->jogStrides));
				s.jogT = static_cast<float>(p / c->jogStrides);
				if (s.jogT > 0.9999f)
					s.jogT = 0.9999f;
				if (s.j > 0.0005f)
				{
					// 182 keeps a sliver while the walk is on, so the engine doesn't free and rebuild it every frame.
					float walkW = s.move * (1.0f - s.j);
					if (s.move > 0.01f && walkW < 0.002f)
						walkW = 0.002f;
					if (walkRoot)
						Engine().setGoal(dobj, kWalkRoot, walkW, 0.0f, 1.0f, 0, 0, 0);
					float jogW = s.move * s.j * c->jogWeight;
					Engine().setGoal(dobj, kJukeRoot, jogW, 0.0f, 1.0f, 0, 0, 0);
					if (jogW >= 0.001f)
					{
						Engine().setGoal(dobj, c->jogNode, 1.0f, 0.0f, 0.0f, 0, 0, 0);
						LocoSetTime(dobj, c->jogNode, s.jogT);
					}
					s.driving = true;
				}
				else if (s.driving)
					ReleaseWalkRoot(s, true);
			}
		}

		// ---- idle active: IW8 PlayAdditiveHipIdleAnim ----------------------------------------------------------------
		if (s.idleOk)
		{
			uint32_t leaf = c->idleNode, root = IdleRoot(leaf);
			bool gate = g_idleActiveEnable && !down && state == 0 && (playing == 1 || playing == 2) && !s.jogWant &&
			            (g_idleFadeLinear || ads < 0.001f);
			s.idleGate = Approach(s.idleGate, gate ? 1.0f : 0.0f, dt / g_idleBlend);
			s.idleW = s.idleGate * (g_idleFadeLinear ? 1.0f - ads : 1.0f) * c->idleWeight;
			if (root == kJukeRoot && juke)
			{
				s.idleGate = s.idleW = 0;  // the engine owns the juke group while juking
				s.idleOn = false;
			}
			else if (s.idleW >= 0.001f)
			{
				if (!s.idleOn)
					s.idlePhase = 0;  // IW8 restarts it (restart = 1) on each fade-in
				s.idleOn = true;
				Engine().setGoal(dobj, root, s.idleW, 0.0f, 1.0f, 0, 0, 0);
				Engine().setGoal(dobj, leaf, 1.0f, 0.0f, 0.0f, 0, 0, 0);
				s.idlePhase = fmod(s.idlePhase + dt * s.idleFreq * c->idleRate, 1.0);  // the leaf's frequency, read with idleOk
				LocoSetTime(dobj, leaf, static_cast<float>(s.idlePhase));
			}
			else if (s.idleOn)
			{
				Engine().setGoal(dobj, root, 0.0f, 0.0f, 1.0f, 0, 0, 0);
				s.idleOn = false;
			}
		}

		if (g_locoDebug && now >= s.nextTrace && (xyspeed > 2.0f || s.j > 0 || s.idleW > 0))
		{
			s.nextTrace = now + 0.5;
			Log("locomotion: bob %d gen %d phase %.2f walk t %.3f jog t %.3f | move %.2f j %.2f (%s)%s | idle %.2f | speed %.0f ads %.2f "
			    "state %d anim %d",
			    bob, s.gen, s.phase, s.walkT, s.jogT, s.move, s.j, s.jogWhy, down ? " STOOD DOWN" : "", s.idleW, xyspeed, ads, state,
			    playing);
			auto node = [&](uint32_t n) {
				if (const uint8_t *i = LocoInfo(dobj, n))
				{
					auto f = [&](size_t o) { return *reinterpret_cast<const float *>(i + o); };
					Log("locomotion:   node %u: time %.3f goal %.3f weight %.3f rate %.2f", n, f(0x10), f(0x1C), f(0x20), f(0x24));
				}
			};
			node(kWalkRoot);
			node(kWalkLeaf);
			if (s.jogOk)
			{
				node(kJukeRoot);
				node(c->jogNode);
			}
			if (s.idleOk)
			{
				node(IdleRoot(c->idleNode));
				node(c->idleNode);
			}
		}
	}
}
