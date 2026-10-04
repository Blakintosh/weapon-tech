// IW-style viewmodel hand IK (opt-in per weapon). Included at the end of bo3_additive.h, after bo3_perf.h.
//
// What it does: after the engine builds the viewmodel skeleton (DObjCalcSkel, model space), each enabled hand's
// shoulder -> elbow -> wrist chain is solved so the wrist sits on the gun's IK locator (tag_ik_loc_le / _ri), the way
// IW8's ik_node does (xanim_ik.cpp XAnimIK_Calc / XAnimIKSolve2Bone). Additive layers (idle_active 193->194, the walk 182,
// the jog 184, additive= 195) can then move the gun non-rigidly without the hands slipping off it.
//
// IW8 reference (iw8 code_source): the target is a tag_ik_target_* model attached to the gun's tag_ik_loc_* locator;
// two-bone analytic solve, elbow bent about its animated hinge, shoulder swung by the shortest arc, wrist optionally
// rotated to the target; the weight per chain comes only from the anims' notetracks, averaged over the weighted leaves:
//   ik_in_start_<hand> .. ik_in_end_<hand>    weight ramps 0 -> 1 in anim time
//   ik_out_start_<hand> .. ik_out_end_<hand>  weight ramps 1 -> 0
//   (hand = left_hand / right_hand; an anim with no IK notes counts as weight 1)
//
// BO3 side (Enhanced exe, research: the author's research notes):
//   DObjCalcSkel 0x1423F85D0(DObj*, u32 partBits[12], float *origin): anim calc, then local -> model space for the
//   requested bones; result in DObj+0x58 (DObjAnimMat[numBones]: quat xyzw, trans xyz, transWeight), calculated bits
//   at DObj+0xC8, frame stamp DObj+0xF8, lock DObj+0x100 (held by both callers around the call).
//   Called (for the viewmodel) from CG_DObjCalcBone 0x140268200 (+0x268325: one bone's chain, tag queries) and from the
//   full pose calc 0x140A43890 (+0xA43B64: the render path, every bone). Both call sites are redirected (rel32, no
//   prologue detour). The wrapper is one pointer compare for every DObj that isn't the IK'd viewmodel.
//   Notetracks are read from the XAnimParts at run time (parts+0xC0 notify[16 bytes: u32 scr name, float time 0..1],
//   parts+0xC8 u8 count); an ik_notes= cfg line replaces them for one xanim (for anims whose notes didn't survive the
//   linker).
//
// Target: the wrist is put back where it sits relative to the locator in the anim WITHOUT additives. That relation is
// measured live (calibration) on frames where every additive root (182, 184, 193, 195) has weight 0 and the hand's
// IK weight is 1, and held while additives play (in the ported IW8 rigs it is constant: wrist - tag_ik_loc ~0.35 in
// every frame of the reloads). With no additive weight the solve is skipped (it would be the identity).
//
// Config (weapon_tech.cfg; every ik line is live):
//   ik_enable=0|1                 global switch, default 0 (off)
//   ik=<weapon>,<1|0>[,<hands l|r|lr>[,<noteless weight 0..1>[,<orient 0|1>]]]
//                                 per weapon, default off. hands default lr; noteless weight = an anim without IK notes
//                                 for that hand (IW: 1); orient 1 = the wrist also takes the locator's rotation (default)
//   ik_alias=<weapon>,<source>    (wop_alias= lines are honoured too) a variant (PaP) takes its source's ik= line
//   ik_notes=<xanim>,<l|r>,<is|ie|os|oe>:<time 0..1>[,...]   IK notes for an xanim (in/out start/end), in place of its own
//   ik_blend=<s>                  extra smoothing of the weight (0 = none, IW behaviour)
//   ik_always=0|1                 1 = solve even with no additive weight (A/B: shows any base-pose mismatch)
//   ik_debug=0|1                  notes found per anim, rig bones, 2 Hz trace (weights, additive weight, slip)
#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>
#include <atomic>

namespace
{
	// ---- math (no engine; ik_test.cpp runs it offline) ---------------------------------------------------------------------
	// Quaternions are x, y, z, w (Hamilton), as the engine's DObjAnimMat: rotate(q, local axis) = the axis in model space
	// (QuatToAxis 0x140236840 rows). A model-space delta d on a bone: q' = d * q, p' = pivot + rotate(d, p - pivot).
	struct IkV3
	{
		float x, y, z;
	};
	struct IkQ
	{
		float x, y, z, w;
	};
	inline IkV3 operator+(IkV3 a, IkV3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
	inline IkV3 operator-(IkV3 a, IkV3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
	inline IkV3 operator*(IkV3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
	inline float IkDot(IkV3 a, IkV3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline IkV3 IkCross(IkV3 a, IkV3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
	inline float IkLen(IkV3 a) { return sqrtf(IkDot(a, a)); }
	inline IkQ IkMul(IkQ a, IkQ b)
	{
		return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
	}
	inline IkQ IkConj(IkQ q) { return {-q.x, -q.y, -q.z, q.w}; }
	inline IkQ IkNorm(IkQ q)
	{
		float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		if (n < 1e-12f)
			return {0, 0, 0, 1};
		n = 1.0f / n;
		return {q.x * n, q.y * n, q.z * n, q.w * n};
	}
	inline IkV3 IkRot(IkQ q, IkV3 v)  // q unit
	{
		IkV3 u{q.x, q.y, q.z};
		IkV3 t = IkCross(u, v) * 2.0f;
		return v + t * q.w + IkCross(u, t);
	}
	inline IkQ IkAxisAngle(IkV3 n, float a)  // n unit
	{
		float s = sinf(a * 0.5f);
		return {n.x * s, n.y * s, n.z * s, cosf(a * 0.5f)};
	}
	// The shortest rotation taking direction u onto direction v.
	inline IkQ IkArc(IkV3 u, IkV3 v)
	{
		float lu = IkLen(u), lv = IkLen(v);
		if (lu < 1e-6f || lv < 1e-6f)
			return {0, 0, 0, 1};
		u = u * (1.0f / lu);
		v = v * (1.0f / lv);
		float d = IkDot(u, v);
		if (d < -0.99999f)  // opposite: any perpendicular axis
		{
			IkV3 a = IkCross(u, fabsf(u.x) < 0.9f ? IkV3{1, 0, 0} : IkV3{0, 1, 0});
			return IkAxisAngle(a * (1.0f / IkLen(a)), 3.14159265f);
		}
		IkV3 c = IkCross(u, v);
		return IkNorm({c.x, c.y, c.z, 1.0f + d});
	}
	inline IkQ IkNlerp(IkQ a, IkQ b, float t)
	{
		if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0)
			b = {-b.x, -b.y, -b.z, -b.w};
		return IkNorm({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
	}
	inline float IkClamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

	// The solve's result: three model-space deltas, applied in this order to the arm's bones by group
	//   group 0 (the shoulder and what hangs off it outside the elbow): d2 about S
	//   group 1 (the elbow, its twist / bulge bones):                   d1 about E, then d2 about S
	//   group 2 (the wrist and the hand):                               d1 about E, d2 about S, then d3 about W2
	struct IkDeltas
	{
		IkQ d1, d2, d3;
		IkV3 s, e, w2;
		IkV3 hinge;  // the hinge axis used (kept as the fallback for a straight arm next frame)
	};

	// Two-bone solve, IW8 XAnimIKSolve2Bone style: the elbow bends about its animated hinge (law of cosines, no pole
	// vector), the shoulder swings by the shortest arc, the wrist takes qT when orient. hint: the hinge to use when the
	// arm is straight (|hint| = 0: none). False when the chain is degenerate.
	bool IkSolve2Bone(IkV3 S, IkV3 E, IkV3 W, IkQ qW, IkV3 T, IkQ qT, bool orient, IkV3 hint, IkDeltas &out)
	{
		const IkV3 u = S - E, v = W - E;
		const float a = IkLen(u), b = IkLen(v);
		if (a < 1e-4f || b < 1e-4f)
			return false;
		const float d = IkClamp(IkLen(T - S), fabsf(a - b) + 1e-3f, a + b - 1e-3f);
		const float thetaNow = acosf(IkClamp(IkDot(u, v) / (a * b), -1.0f, 1.0f));
		const float thetaWant = acosf(IkClamp((a * a + b * b - d * d) / (2.0f * a * b), -1.0f, 1.0f));
		IkV3 n = IkCross(u, v);
		float nl = IkLen(n);
		if (nl < 1e-4f * a * b)
		{
			n = hint;
			nl = IkLen(n);
			if (nl < 1e-6f)
				return false;
		}
		n = n * (1.0f / nl);
		out.hinge = n;
		out.s = S;
		out.e = E;
		out.d1 = IkAxisAngle(n, thetaWant - thetaNow);  // + about u x v opens the angle from u towards v
		const IkV3 w1 = E + IkRot(out.d1, v);
		out.d2 = IkArc(w1 - S, T - S);
		out.w2 = S + IkRot(out.d2, w1 - S);
		if (orient)
		{
			const IkQ qW2 = IkNorm(IkMul(out.d2, IkMul(out.d1, qW)));
			IkQ d3 = IkNorm(IkMul(qT, IkConj(qW2)));
			if (d3.w < 0)
				d3 = {-d3.x, -d3.y, -d3.z, -d3.w};
			out.d3 = d3;
		}
		else
			out.d3 = {0, 0, 0, 1};
		return true;
	}

	// One bone of the arm through the deltas (q may be non-unit: the engine's transWeight keeps |q|, d * q does too).
	inline void IkApplyDeltas(const IkDeltas &k, int group, IkV3 &p, IkQ &q)
	{
		if (group >= 1)
		{
			p = k.e + IkRot(k.d1, p - k.e);
			q = IkMul(k.d1, q);
		}
		p = k.s + IkRot(k.d2, p - k.s);
		q = IkMul(k.d2, q);
		if (group == 2)
		{
			p = k.w2 + IkRot(k.d3, p - k.w2);
			q = IkMul(k.d3, q);
		}
	}

	// IK notes of one hand in one anim, sorted by time. type: 0 in_start, 1 in_end, 2 out_start, 3 out_end.
	struct IkMarkers
	{
		int n;
		uint8_t type[8];
		float t[8];
	};
	void IkSortMarkers(IkMarkers &m)
	{
		for (int i = 1; i < m.n; i++)
			for (int j = i; j > 0 && m.t[j] < m.t[j - 1]; j--)
			{
				std::swap(m.t[j], m.t[j - 1]);
				std::swap(m.type[j], m.type[j - 1]);
			}
	}

	// IW8 XAnimCalcNotetrackWeight: 0 -> 1 between in_start and in_end, 1 -> 0 between out_start and out_end, held in
	// between; before the first note, 1 if that note takes the hand out and 0 if it brings it in. No notes: dflt.
	float IkNoteWeight(const IkMarkers &m, float t, float dflt)
	{
		if (m.n <= 0)
			return dflt;
		int last = -1;
		for (int i = 0; i < m.n && m.t[i] <= t; i++)
			last = i;
		if (last < 0)
			return m.type[0] >= 2 ? 1.0f : 0.0f;
		const float t0 = m.t[last];
		switch (m.type[last])
		{
		case 0:  // ramping in: up to the next in_end
			for (int j = last + 1; j < m.n; j++)
				if (m.type[j] == 1)
					return m.t[j] > t0 ? IkClamp((t - t0) / (m.t[j] - t0), 0.0f, 1.0f) : 1.0f;
			return 1.0f;
		case 1:
			return 1.0f;
		case 2:  // ramping out: down to the next out_end
			for (int j = last + 1; j < m.n; j++)
				if (m.type[j] == 3)
					return m.t[j] > t0 ? 1.0f - IkClamp((t - t0) / (m.t[j] - t0), 0.0f, 1.0f) : 0.0f;
			return 0.0f;
		default:
			return 0.0f;
		}
	}

	// "is:0.9,ie:0.93" style marker list -> markers. False on a bad token.
	bool IkParseMarkerToken(const char *tok, IkMarkers &m)
	{
		static const char *const kTypes[4] = {"is", "ie", "os", "oe"};
		if (m.n >= 8)
			return false;
		for (int k = 0; k < 4; k++)
			if (strncmp(tok, kTypes[k], 2) == 0 && tok[2] == ':')
			{
				float t;
				if (sscanf_s(tok + 3, "%f", &t) != 1 || t < 0 || t > 1)
					return false;
				m.type[m.n] = static_cast<uint8_t>(k);
				m.t[m.n++] = t;
				return true;
			}
		return false;
	}
}

#ifndef BO3_IK_MATH_ONLY
namespace
{
	// ---- Engine addresses (RVAs): Enhanced values; bo3_retail.h overwrites them on retail ------------------------------------
	uintptr_t kDObjCalcSkel = 0x23F85D0;        // DObjCalcSkel(DObj*, u32 partBits[12], float *origin)
	uintptr_t kCallSkelCalcBone = 0x268325;     // in CG_DObjCalcBone 0x140268200 (lock held)
	uintptr_t kCallSkelCalcPose = 0xA43B64;     // in the full pose calc 0x140A43890 (lock held)
	// (the scr string table is bo3_additive.h's kSLTable)
	// DObj
	constexpr size_t kDObjSkelMats = 0x58, kDObjSkelWork = 0x60, kDObjSkelBits = 0xC8, kDObjSkelStamp = 0xF8, kDObjDupName = 0x104;
	constexpr size_t kDObjNumModels = 0x122, kDObjFlags = 0x124;
	// DObjCalcSkel with this flag builds every bone in the work buffer +0x60 (parents are read from there too) and, at the
	// end of each call that built anything, publishes ALL bones to +0x58 with the pose origin added (trans += origin).
	// The viewmodel has it. IK edits the work buffer (so later builds and re-publishes carry it) and publishes the bones it
	// moved itself, the same way.
	constexpr uint16_t kDObjFlagDoubleSkel = 0x400;
	// XModel
	constexpr size_t kXModelNumBones = 0x08, kXModelNumRoot = 0x09, kXModelParents = 0x18;
	// XAnimInfo (0x58 each, kXAnimInfo)
	constexpr size_t kInfoParts = 0x08, kInfoTime = 0x10, kInfoWeight = 0x20, kInfoLeaf = 0x30, kInfoNext = 0x44, kInfoChild = 0x48;
	// XAnimParts
	constexpr size_t kPartsNotify = 0xC0, kPartsNotifyCount = 0xC8;
	constexpr uint32_t kIkAdditiveRoots[] = {182, 184, 193, 195};
	constexpr int kIkMaxBones = 384, kIkMaxArm = 128;

	// ---- config --------------------------------------------------------------------------------------------------------------
	struct IkWeapon
	{
		char weapon[64];
		bool on;
		bool hand[2];         // 0 left, 1 right
		float notelessWeight; // an anim without IK notes for the hand (IW8: 1)
		bool orient;
	};
	struct IkAlias
	{
		char weapon[64], source[64];
	};
	struct IkNotesCfg
	{
		char xanim[96];
		int hand;
		IkMarkers m;
	};
	constexpr int kIkMaxWeapons = 64, kIkMaxAliases = 128, kIkMaxNotesCfg = 512;
	IkWeapon g_ikWeapons[kIkMaxWeapons];
	int g_ikWeaponCount;
	IkAlias g_ikAliases[kIkMaxAliases];
	int g_ikAliasCount;
	IkNotesCfg g_ikNotesCfg[kIkMaxNotesCfg];
	int g_ikNotesCfgCount;
	bool g_ikEnable, g_ikDebug, g_ikAlways;
	float g_ikBlend;
	unsigned g_ikCfgGen = 1;  // bumped on every (re)load: the held weapon and the note cache are looked up again
	bool g_ikConfiguredAtStart, g_ikHooked;

	void IkResetConfig()
	{
		g_ikWeaponCount = g_ikAliasCount = g_ikNotesCfgCount = 0;
		g_ikEnable = g_ikDebug = g_ikAlways = false;
		g_ikBlend = 0;
		g_ikCfgGen++;
	}

	int IkSplit(char *s, char **f, int max)
	{
		int n = 0;
		for (char *p = s; n < max;)
		{
			f[n++] = p;
			p = strchr(p, ',');
			if (!p)
				break;
			*p++ = 0;
		}
		for (int i = 0; i < n; i++)  // trim
		{
			while (*f[i] == ' ' || *f[i] == '\t')
				f[i]++;
			for (char *e = f[i] + strlen(f[i]); e > f[i] && (e[-1] == ' ' || e[-1] == '\t'); *--e = 0) {}
		}
		return n;
	}

	// One cfg line. known = false for keys that aren't ours (wop_alias= is read here and left known = false, so the
	// locomotion / sway aliases still get it).
	bool ParseIkLine(const char *line, bool &known)
	{
		known = false;
		char buf[256];
		if (strncmp(line, "wop_alias=", 10) == 0 || strncmp(line, "ik_alias=", 9) == 0)
		{
			known = line[0] == 'i';
			strncpy_s(buf, strchr(line, '=') + 1, _TRUNCATE);
			buf[strcspn(buf, "#")] = 0;
			char *f[3];  // wop_alias= may carry a third field (the source's fire time)
			if (IkSplit(buf, f, 3) < 2 || !f[0][0] || !f[1][0] || strlen(f[0]) >= 64 || strlen(f[1]) >= 64)
				return !known;
			if (g_ikAliasCount < kIkMaxAliases)
			{
				strcpy_s(g_ikAliases[g_ikAliasCount].weapon, f[0]);
				strcpy_s(g_ikAliases[g_ikAliasCount++].source, f[1]);
			}
			return true;
		}
		if (strncmp(line, "ik", 2) != 0)
			return true;
		known = true;
		int v;
		if (sscanf_s(line, "ik_enable=%d", &v) == 1)
			return g_ikEnable = v != 0, true;
		if (sscanf_s(line, "ik_debug=%d", &v) == 1)
			return g_ikDebug = v != 0, true;
		if (sscanf_s(line, "ik_always=%d", &v) == 1)
			return g_ikAlways = v != 0, true;
		if (sscanf_s(line, "ik_blend=%f", &g_ikBlend) == 1)
			return g_ikBlend = IkClamp(g_ikBlend, 0.0f, 2.0f), true;
		if (strncmp(line, "ik=", 3) == 0)
		{
			strncpy_s(buf, line + 3, _TRUNCATE);
			buf[strcspn(buf, "#")] = 0;
			char *f[6];
			int n = IkSplit(buf, f, 6);
			if (n < 2 || n > 5 || !f[0][0] || strlen(f[0]) >= 64 || g_ikWeaponCount >= kIkMaxWeapons)
				return false;
			IkWeapon &w = g_ikWeapons[g_ikWeaponCount];
			strcpy_s(w.weapon, f[0]);
			w.on = atoi(f[1]) != 0;
			w.hand[0] = w.hand[1] = true;
			w.notelessWeight = 1.0f;
			w.orient = true;
			if (n >= 3 && f[2][0])
			{
				w.hand[0] = strchr(f[2], 'l') != nullptr;
				w.hand[1] = strchr(f[2], 'r') != nullptr;
				if (!w.hand[0] && !w.hand[1])
					return false;
			}
			if (n >= 4 && f[3][0])
				w.notelessWeight = IkClamp(static_cast<float>(atof(f[3])), 0.0f, 1.0f);
			if (n >= 5 && f[4][0])
				w.orient = atoi(f[4]) != 0;
			g_ikWeaponCount++;
			return true;
		}
		if (strncmp(line, "ik_notes=", 9) == 0)
		{
			strncpy_s(buf, line + 9, _TRUNCATE);
			buf[strcspn(buf, "#")] = 0;
			char *f[12];
			int n = IkSplit(buf, f, 12);
			if (n < 3 || !f[0][0] || strlen(f[0]) >= 96 || (strcmp(f[1], "l") != 0 && strcmp(f[1], "r") != 0) ||
			    g_ikNotesCfgCount >= kIkMaxNotesCfg)
				return false;
			IkNotesCfg &c = g_ikNotesCfg[g_ikNotesCfgCount];
			strcpy_s(c.xanim, f[0]);
			c.hand = f[1][0] == 'r';
			c.m.n = 0;
			for (int i = 2; i < n; i++)
				if (!IkParseMarkerToken(f[i], c.m))
					return false;
			IkSortMarkers(c.m);
			g_ikNotesCfgCount++;
			return true;
		}
		known = false;  // some other ik* key: not ours
		return true;
	}

	bool IkConfigured() { return g_ikConfiguredAtStart || g_ikWeaponCount > 0; }

	// Live reload (ApplyTuningText): every ik line again from the whole text.
	void IkReloadText(const char *text)
	{
		IkResetConfig();
		char line[256];
		int bad = 0;
		for (const char *p = text; (p = NextCfgLine(p, line, sizeof(line))) != nullptr;)
		{
			line[strcspn(line, "\r\n")] = 0;
			bool known;
			if (!ParseIkLine(line, known) && known)
			{
				bad++;
				Log("ik: live reload: bad line '%s'", line);
			}
		}
		if (FeatOff(kFtIk))  // [features] ik = off: stays off
			IkResetConfig();
		Log("ik: live: ik_enable %d, %d weapon line(s), %d ik_notes line(s), %d alias(es), blend %.2f, always %d, debug %d%s",
		    g_ikEnable, g_ikWeaponCount, g_ikNotesCfgCount, g_ikAliasCount, g_ikBlend, g_ikAlways, g_ikDebug,
		    g_ikHooked ? "" : " (hooks not in: needs ik= lines at game start)");
	}

	// ---- rig: the arm bones, the locators and who hangs off whom, per viewmodel DObj --------------------------------------------
	struct IkHandRig
	{
		bool ok;
		int16_t s, e, w, t;            // j_shoulder, j_elbow, j_wrist, tag_ik_loc
		int16_t bone[kIkMaxArm];       // every bone under the shoulder (the shoulder too)
		uint8_t group[kIkMaxArm];      // 0 shoulder, 1 elbow, 2 wrist (IkDeltas)
		int n;
	};
	struct IkRig
	{
		const void *dobj;
		uint64_t models;
		int16_t numBones;
		IkHandRig hand[2];
	};
	IkRig g_ikRigs[2];  // double buffered: the skel hook may be reading one on another thread while the other is built
	std::atomic<IkRig *> g_ikRig{nullptr};

	// Per-hand state shared by the frame (game thread) and the skel hook (whichever thread builds the skeleton, under the
	// DObj lock). Plain floats: a frame-late value is harmless.
	struct IkHandState
	{
		float weight;          // the IK weight this frame (notes, blended)
		bool calibrated;
		IkV3 offPos;           // wrist relative to the locator, in the locator's frame
		IkQ offRot;
		IkV3 hint;             // last hinge
		const void *appliedObj;
		int appliedStamp;
		float appliedWrist[8]; // the wrist's mat as we wrote it (a rebuilt skeleton has another)
		float slip;            // pre-IK wrist -> target distance (debug)
		float calAdd;          // the additive weight the calibration was taken at
	};
	IkHandState g_ikHand[2];
	std::atomic<void *> g_ikDObj{nullptr};  // the viewmodel DObj while the held weapon has IK on (else null)
	float g_ikAddW;                          // the strongest additive root weight this frame
	bool g_ikOrient = true;
	bool g_ikApply;  // ik_enable: write the solve; without it (ik_debug=1) the hook only calibrates and measures the slip

	uint32_t IkScr(const char *s) { return SlString(s, 0); }

	// The text of a scr string (table *(u8**)0x143B1F308, 28 bytes an entry, text at +4), or nullptr. Cold paths only.
	const char *IkScrText(uint32_t id)
	{
		const uint8_t *sl = *At<uint8_t *>(kSLTable);
		if (!id || !sl || id > 0x100000)
			return nullptr;
		const char *t = reinterpret_cast<const char *>(sl + 28 * static_cast<size_t>(id) + 4);
		return FastReadable(t, 64) && strnlen(t, 64) < 64 ? t : nullptr;
	}

	int16_t IkBone(void *dobj, uint32_t name)
	{
		int16_t idx = -2;
		auto getBone = reinterpret_cast<int (*)(void *, uint32_t, int16_t *, uint32_t)>(g_base + kDObjGetBoneIndex);
		return getBone(dobj, name, &idx, 0xFFFF) ? idx : -1;
	}

	// parent[] for every bone of the DObj, across its models: a model's own bones by its parent list, its root bones
	// under the bone it is attached to, and the bones it shares by name with an earlier model (copied from that one,
	// DObj+0x104 scr blob: u32 mask[12], then (dst, src) s16 pairs, -1 ends) under their source.
	bool IkBuildParents(uint8_t *dobj, int16_t numBones, int16_t *parent)
	{
		for (int b = 0; b < numBones; b++)
			parent[b] = -1;
		const int numModels = dobj[kDObjNumModels];
		auto *const *models = *reinterpret_cast<uint8_t *const *const *>(dobj + kDObjModels);
		if (!models || !FastReadable(models, numModels * 10))
			return false;
		const uint16_t *attach = reinterpret_cast<const uint16_t *>(models + numModels);
		int start = 0;
		for (int m = 0; m < numModels; m++)
		{
			const uint8_t *xm = models[m];
			if (!xm || !FastReadable(xm, 0x20))
				return false;
			const int nb = xm[kXModelNumBones], nr = xm[kXModelNumRoot];
			const uint8_t *plist = *reinterpret_cast<const uint8_t *const *>(xm + kXModelParents);
			if (start + nb > numBones || (nb > nr && (!plist || !FastReadable(plist, nb - nr))))
				return false;
			for (int i = 0; i < nb; i++)
				parent[start + i] = i >= nr ? static_cast<int16_t>(start + i - plist[i - nr])
				                            : attach[m] != 0xFFFF ? static_cast<int16_t>(attach[m]) : int16_t(-1);
			start += nb;
		}
		const uint32_t dup = *reinterpret_cast<const uint32_t *>(dobj + kDObjDupName);
		const uint8_t *sl = *At<uint8_t *>(kSLTable);
		if (dup && sl)
		{
			const int16_t *pair = reinterpret_cast<const int16_t *>(sl + 28 * static_cast<size_t>(dup) + 4 + 48);
			for (int k = 0; k < kIkMaxBones && FastReadable(pair, 4) && *reinterpret_cast<const int32_t *>(pair) != -1; k++, pair += 2)
				if (pair[0] >= 0 && pair[0] < numBones && pair[1] >= 0 && pair[1] < numBones)
					parent[pair[0]] = pair[1];
		}
		return true;
	}

	void IkBuildHand(void *dobj, const int16_t *parent, int16_t numBones, int h, IkHandRig &r)
	{
		static uint32_t s_names[2][4];
		static const char *const kNames[2][4] = {{"j_shoulder_le", "j_elbow_le", "j_wrist_le", "tag_ik_loc_le"},
		                                         {"j_shoulder_ri", "j_elbow_ri", "j_wrist_ri", "tag_ik_loc_ri"}};
		if (!s_names[h][0])
			for (int k = 0; k < 4; k++)
				s_names[h][k] = IkScr(kNames[h][k]);
		r.ok = false;
		r.n = 0;
		r.s = IkBone(dobj, s_names[h][0]);
		r.e = IkBone(dobj, s_names[h][1]);
		r.w = IkBone(dobj, s_names[h][2]);
		r.t = IkBone(dobj, s_names[h][3]);
		if (r.s < 0 || r.e < 0 || r.w < 0 || r.t < 0)
		{
			Log("ik: %s hand: %s%s%s%s not in the viewmodel; this hand is off", h ? "right" : "left", r.s < 0 ? kNames[h][0] : "",
			    r.e < 0 ? " " : "", r.e < 0 ? kNames[h][1] : "", r.w < 0 || r.t < 0 ? " (wrist / tag_ik_loc)" : "");
			return;
		}
		// Every bone under the shoulder, grouped by the first of wrist / elbow / shoulder above it.
		for (int b = 0; b < numBones; b++)
		{
			int g = -1, depth = 0;
			for (int p = b; p >= 0 && depth < 64; p = parent[p], depth++)
			{
				if (p == r.w) { g = 2; break; }
				if (p == r.e) { g = 1; break; }
				if (p == r.s) { g = 0; break; }
			}
			if (g < 0)
				continue;
			if (b == r.t)
			{
				Log("ik: %s hand: tag_ik_loc is under the arm (it would chase itself); this hand is off", h ? "right" : "left");
				return;
			}
			if (r.n >= kIkMaxArm)
			{
				Log("ik: %s hand: more than %d bones under the shoulder; this hand is off", h ? "right" : "left", kIkMaxArm);
				return;
			}
			r.bone[r.n] = static_cast<int16_t>(b);
			r.group[r.n++] = static_cast<uint8_t>(g);
		}
		// the chain must be shoulder -> elbow -> wrist
		int ge = -1, gw = -1;
		for (int i = 0; i < r.n; i++)
		{
			if (r.bone[i] == r.e) ge = r.group[i];
			if (r.bone[i] == r.w) gw = r.group[i];
		}
		if (ge != 1 || gw != 2)
		{
			Log("ik: %s hand: the elbow / wrist aren't under the shoulder; this hand is off", h ? "right" : "left");
			return;
		}
		r.ok = true;
	}

	IkRig *IkRigFor(uint8_t *dobj)
	{
		const uint64_t models = *reinterpret_cast<uint64_t *>(dobj + kDObjModels);
		const int16_t numBones = *reinterpret_cast<int16_t *>(dobj + kDObjNumBones);
		IkRig *cur = g_ikRig.load(std::memory_order_acquire);
		if (cur && cur->dobj == dobj && cur->models == models && cur->numBones == numBones)
			return cur;
		IkRig &r = g_ikRigs[cur == &g_ikRigs[0] ? 1 : 0];
		memset(&r, 0, sizeof(r));
		r.dobj = dobj;
		r.models = models;
		r.numBones = numBones;
		static int16_t parent[kIkMaxBones];
		if (numBones <= 0 || numBones > kIkMaxBones || !IkBuildParents(dobj, numBones, parent))
			Log("ik: viewmodel DObj %p (%d bones): couldn't read its models; IK off for it", dobj, numBones);
		else
			for (int h = 0; h < 2; h++)
				IkBuildHand(dobj, parent, numBones, h, r.hand[h]);
		Log("ik: rig for DObj %p, %d bones: left %s (%d arm bones, wrist %d, loc %d), right %s (%d, wrist %d, loc %d)", dobj,
		    numBones, r.hand[0].ok ? "ok" : "off", r.hand[0].n, r.hand[0].w, r.hand[0].t, r.hand[1].ok ? "ok" : "off", r.hand[1].n,
		    r.hand[1].w, r.hand[1].t);
		for (IkHandState &s : g_ikHand)
			s.calibrated = false, s.appliedObj = nullptr, s.hint = {0, 0, 0};
		g_ikRig.store(&r, std::memory_order_release);
		return &r;
	}

	// ---- notes: per XAnimParts, from its notify list or an ik_notes= line ------------------------------------------------------
	struct IkNoteEntry
	{
		const uint8_t *parts;
		unsigned gen, perfGen;
		IkMarkers m[2];
	};
	IkNoteEntry g_ikNoteCache[256];

	const IkNoteEntry &IkNotesFor(const uint8_t *parts)
	{
		IkNoteEntry &e = g_ikNoteCache[(reinterpret_cast<uintptr_t>(parts) >> 4) & 255];
		if (e.parts == parts && e.gen == g_ikCfgGen && e.perfGen == g_perfGen)
			return e;
		e.parts = parts;
		e.gen = g_ikCfgGen;
		e.perfGen = g_perfGen;
		e.m[0].n = e.m[1].n = 0;
		if (!parts || !FastReadable(parts, kPartsNotifyCount + 1))
			return e;
		const char *name = *reinterpret_cast<const char *const *>(parts);
		ReadCache rc;
		const size_t span = name ? rc.Span(name) : 0;
		const bool nameOk = span && strnlen(name, span) < span;
		bool fromCfg = false;
		if (nameOk)
			for (int i = 0; i < g_ikNotesCfgCount; i++)
				if (_stricmp(g_ikNotesCfg[i].xanim, name) == 0)
				{
					e.m[g_ikNotesCfg[i].hand] = g_ikNotesCfg[i].m;
					fromCfg = true;
				}
		int found = 0, count = 0;
		bool notesOk = false;
		if (!fromCfg)
		{
			static uint32_t s_ids[2][4];
			static char s_names[2][4][32];
			if (!s_ids[0][0])
			{
				static const char *const kPre[4] = {"ik_in_start_", "ik_in_end_", "ik_out_start_", "ik_out_end_"};
				for (int h = 0; h < 2; h++)
					for (int k = 0; k < 4; k++)
					{
						snprintf(s_names[h][k], sizeof(s_names[h][k]), "%s%s", kPre[k], h ? "right_hand" : "left_hand");
						s_ids[h][k] = IkScr(s_names[h][k]);
					}
			}
			// The engine's viewmodel notetrack handler (0x1402FE520) takes the note's name from +8 ("end" / "loop_end"
			// are compared there); +0 and +0xC are the other scr strings of the entry. All three are matched, by id and,
			// failing that, by text (an id made with another case would differ).
			count = parts[kPartsNotifyCount];
			const uint8_t *notes = *reinterpret_cast<const uint8_t *const *>(parts + kPartsNotify);
			notesOk = count && notes && FastReadable(notes, 16 * static_cast<size_t>(count));
			if (notesOk)
				for (int i = 0; i < count; i++)
				{
					const float t = *reinterpret_cast<const float *>(notes + 16 * i + 4);
					for (size_t field : {size_t(8), size_t(0), size_t(12)})
					{
						const uint32_t id = *reinterpret_cast<const uint32_t *>(notes + 16 * i + field);
						int hit = -1;
						for (int hk = 0; hk < 8 && hit < 0; hk++)
							if (id && id == s_ids[hk >> 2][hk & 3])
								hit = hk;
						if (hit < 0 && id)
							if (const char *txt = IkScrText(id))
								for (int hk = 0; hk < 8 && hit < 0; hk++)
									if (_stricmp(txt, s_names[hk >> 2][hk & 3]) == 0)
										hit = hk;
						if (hit < 0)
							continue;
						IkMarkers &m = e.m[hit >> 2];
						if (m.n < 8 && t >= 0 && t <= 1)
						{
							m.type[m.n] = static_cast<uint8_t>(hit & 3);
							m.t[m.n++] = t;
							found++;
						}
						break;
					}
				}
			IkSortMarkers(e.m[0]);
			IkSortMarkers(e.m[1]);
		}
		static int s_logs;
		if ((g_ikDebug || found || fromCfg) && s_logs < 200)
		{
			s_logs++;
			char txt[2][160];
			static const char *const kT[4] = {"is", "ie", "os", "oe"};
			for (int h = 0; h < 2; h++)
			{
				int n = 0;
				txt[h][0] = 0;
				for (int i = 0; i < e.m[h].n; i++)
					n += snprintf(txt[h] + n, sizeof(txt[h]) - n, "%s%s:%.3f", i ? "," : "", kT[e.m[h].type[i]], e.m[h].t[i]);
				if (!e.m[h].n)
					strcpy_s(txt[h], "-");
			}
			Log("ik: notes %s (%s): left %s | right %s", nameOk ? name : "?", fromCfg ? "ik_notes= line" : found ? "notetracks" : "none",
			    txt[0], txt[1]);
			// No IK notes found: what the anim does carry (ik_debug=1), so a stripped list can be told from a reader miss.
			static int s_dumps;
			if (!fromCfg && !found && g_ikDebug && s_dumps < 24)
			{
				s_dumps++;
				const uint8_t *notes = *reinterpret_cast<const uint8_t *const *>(parts + kPartsNotify);
				Log("ik:   %s: %d notify entr%s at %p%s", nameOk ? name : "?", count, count == 1 ? "y" : "ies", notes,
				    count && !notesOk ? " (not readable)" : "");
				for (int i = 0; notesOk && i < count && i < 16; i++)
				{
					const uint8_t *n = notes + 16 * i;
					auto id = [&](size_t o) { return *reinterpret_cast<const uint32_t *>(n + o); };
					const char *a = IkScrText(id(0)), *b = IkScrText(id(8)), *c = IkScrText(id(12));
					Log("ik:     [%d] t %.3f  +0 %u '%s'  +8 %u '%s'  +C %u '%s'", i, *reinterpret_cast<const float *>(n + 4), id(0),
					    a ? a : "", id(8), b ? b : "", id(12), c ? c : "");
				}
			}
		}
		return e;
	}

	// The IK weight per hand, IW8 style: every weighted leaf of the base (non-additive) part of the tree, its notes'
	// weight at its time, averaged by the leaf's weight.
	void IkAccumulate(void *tree, uint32_t idx, float w, int depth, int &visits, const IkWeapon &cfg, float sum[2], float wsum[2])
	{
		if (!idx || depth > 12 || ++visits > 256)
			return;
		const uint8_t *info = At<uint8_t>(kXAnimInfo + 0x58 * static_cast<size_t>(idx));
		const float wt = w * *reinterpret_cast<const float *>(info + kInfoWeight);
		if (!(wt > 1e-4f))
			return;
		if (*reinterpret_cast<const uint32_t *>(info + kInfoLeaf))  // leaf
		{
			const IkNoteEntry &e = IkNotesFor(*reinterpret_cast<const uint8_t *const *>(info + kInfoParts));
			const float t = *reinterpret_cast<const float *>(info + kInfoTime);
			for (int h = 0; h < 2; h++)
			{
				sum[h] += wt * IkNoteWeight(e.m[h], t, cfg.notelessWeight);
				wsum[h] += wt;
			}
			return;
		}
		for (uint32_t c = *reinterpret_cast<const uint32_t *>(info + kInfoChild); c;
		     c = *reinterpret_cast<const uint32_t *>(At<uint8_t>(kXAnimInfo + 0x58 * static_cast<size_t>(c)) + kInfoNext))
		{
			const uint8_t *ci = At<uint8_t>(kXAnimInfo + 0x58 * static_cast<size_t>(c));
			if (!*reinterpret_cast<const uint32_t *>(ci + kInfoLeaf) && (ci[kInfoParts] & 0x10))
				continue;  // an additive blend: its anims don't carry IK notes
			IkAccumulate(tree, c, wt, depth + 1, visits, cfg, sum, wsum);
			if (visits > 256)
				break;
		}
	}

	const IkWeapon *IkHeldWeapon(int variant)
	{
		static int s_variant = -1;
		static unsigned s_gen, s_perfGen;
		static const IkWeapon *s_held;
		static const uint8_t *s_ptr;
		const uint8_t *p = At<uint8_t *>(kWeaponVariants)[variant & 0x1FF];
		if (variant == s_variant && p == s_ptr && s_gen == g_ikCfgGen && s_perfGen == g_perfGen)
			return s_held;
		s_variant = variant;
		s_ptr = p;
		s_gen = g_ikCfgGen;
		s_perfGen = g_perfGen;
		s_held = nullptr;
		const char *name = p && FastReadable(p, 8) ? *reinterpret_cast<const char *const *>(p) : nullptr;
		ReadCache rc;
		const size_t span = name ? rc.Span(name) : 0;
		if (!span || strnlen(name, span) >= span)
			return nullptr;
		const char *look = name;
		for (int pass = 0; pass < 2 && look; pass++)
		{
			for (int i = 0; i < g_ikWeaponCount; i++)
				if (_stricmp(g_ikWeapons[i].weapon, look) == 0)
					s_held = &g_ikWeapons[i];
			if (s_held)
				break;
			const char *src = nullptr;
			for (int i = 0; i < g_ikAliasCount; i++)
				if (_stricmp(g_ikAliases[i].weapon, look) == 0)
					src = g_ikAliases[i].source;
			look = src;
		}
		Log("ik: holding %s: %s", name, s_held ? (s_held->on ? "IK on" : "IK line says 0") : "no ik= line");
		return s_held;
	}

	// The game thread, once per frame (the anim hook, after CG_UpdateViewWeaponAnim): what the skel hook needs.
	void IkOff()
	{
		g_ikDObj.store(nullptr, std::memory_order_relaxed);
		for (IkHandState &s : g_ikHand)
			s.weight = 0;
	}

	void IkFrame(uint8_t *ps, uint8_t *vm)
	{
		void *dobj = *reinterpret_cast<void **>(vm + kVmDObj);
		const IkWeapon *w = g_ikHooked && (g_ikEnable || g_ikDebug) && dobj ? IkHeldWeapon(static_cast<int>(*reinterpret_cast<uint64_t *>(ps + kPsWeapon) & 0x1FF)) : nullptr;
		void *tree = dobj ? *reinterpret_cast<void **>(dobj) : nullptr;
		if (!w || !w->on || !tree)
		{
			g_ikDObj.store(nullptr, std::memory_order_relaxed);
			for (IkHandState &s : g_ikHand)
				s.weight = 0;
			return;
		}
		IkRig *rig = IkRigFor(static_cast<uint8_t *>(dobj));
		const double now = GameNow();
		static double s_last = -1;
		const float dt = s_last < 0 ? 0.0f : static_cast<float>(fmin(now - s_last, 0.1));
		s_last = now;

		float sum[2] = {}, wsum[2] = {};
		int visits = 0;
		IkAccumulate(tree, *reinterpret_cast<const uint32_t *>(static_cast<uint8_t *>(tree) + 8), 1.0f, 0, visits, *w, sum, wsum);
		for (int h = 0; h < 2; h++)
		{
			float target = !w->hand[h] || !rig->hand[h].ok ? 0.0f : wsum[h] > 0 ? sum[h] / wsum[h] : w->notelessWeight;
			IkHandState &s = g_ikHand[h];
			if (g_ikBlend > 0 && dt > 0)
			{
				const float step = dt / g_ikBlend;
				s.weight = target > s.weight ? fminf(target, s.weight + step) : fmaxf(target, s.weight - step);
			}
			else
				s.weight = target;
		}
		auto getInfo = &XGetInfo;
		float add = 0;
		for (uint32_t node : kIkAdditiveRoots)
			if (uint32_t i = getInfo(tree, node))
				add = fmaxf(add, *At<float>(kXAnimInfo + 0x58 * static_cast<size_t>(i) + kInfoWeight));
		g_ikAddW = add;
		g_ikOrient = w->orient;
		g_ikApply = g_ikEnable;
		g_ikDObj.store(dobj, std::memory_order_release);

		static double s_trace;
		if (g_ikDebug && now > s_trace)
		{
			s_trace = now + 0.5;
			Log("ik: %s%s weight L %.2f R %.2f, additive %.2f | pre-IK slip L %.2f R %.2f | calibrated L %d (at add %.2f) R %d (at add %.2f)",
			    w->weapon, g_ikEnable ? "" : " (measuring only, ik_enable 0)", g_ikHand[0].weight, g_ikHand[1].weight, add, g_ikHand[0].slip, g_ikHand[1].slip, g_ikHand[0].calibrated,
			    g_ikHand[0].calAdd, g_ikHand[1].calibrated, g_ikHand[1].calAdd);
		}
	}

	// ---- the skel hook ------------------------------------------------------------------------------------------------------------
	inline bool IkCalculated(const uint8_t *dobj, int b)
	{
		return (reinterpret_cast<const uint32_t *>(dobj + kDObjSkelBits)[b >> 5] & (0x80000000u >> (b & 31))) != 0;
	}

	// After DObjCalcSkel on the IK'd viewmodel (DObj lock held by the caller). The arm is solved once all four chain bones
	// exist; bones under it already built are carried along, the rest are built later from the moved parents. A skeleton
	// rebuilt in the same frame (the camera tag pass invalidates it) is found by the wrist's mat changing.
	void IkAfterSkel(uint8_t *obj, const float *origin)
	{
		IkRig *rig = g_ikRig.load(std::memory_order_acquire);
		if (!rig || rig->dobj != obj || rig->models != *reinterpret_cast<uint64_t *>(obj + kDObjModels))
			return;
		const bool dbl = (*reinterpret_cast<uint16_t *>(obj + kDObjFlags) & kDObjFlagDoubleSkel) != 0;
		float *pub = *reinterpret_cast<float **>(obj + kDObjSkelMats);
		float *mats = dbl ? *reinterpret_cast<float **>(obj + kDObjSkelWork) : pub;  // the buffer the hierarchy builds in
		if (!mats || !pub)
			return;
		// One-time sanity of the inferred DObj layout (audit C-3): the root bone of a calculated skeleton has a unit
		// quaternion and a finite position. A miss turns IK off for the process (the skeleton is left as the engine built it).
		static std::atomic<int> s_layoutOk{-1};
		if (s_layoutOk.load(std::memory_order_relaxed) < 0 && IkCalculated(obj, 0))
		{
			const size_t bytes = 32 * static_cast<size_t>(rig->numBones > 0 ? rig->numBones : 1);
			const bool readable = FastReadable(mats, bytes) && FastReadable(pub, bytes);
			const float q2 = readable ? pub[0] * pub[0] + pub[1] * pub[1] + pub[2] * pub[2] + pub[3] * pub[3] : 0.0f;
			const bool ok = readable && q2 > 0.5f && q2 < 2.0f && std::isfinite(pub[4]) && std::isfinite(pub[5]) && std::isfinite(pub[6]);
			int expected = -1;
			if (s_layoutOk.compare_exchange_strong(expected, ok ? 1 : 0))
				Log("ik: DObj layout check %s (root |q|^2 %.3f, %d bones%s)", ok ? "passed" : "FAILED: IK off", q2,
				    static_cast<int>(rig->numBones), readable ? "" : ", skeleton buffers not readable");
		}
		if (s_layoutOk.load(std::memory_order_relaxed) != 1)
			return;
		static int s_mode = -1;
		if (s_mode != static_cast<int>(dbl))
		{
			s_mode = dbl;
			Log("ik: viewmodel skeleton %s", dbl ? "double buffered (0x400): IK on the work buffer +0x60, moved bones published to +0x58 + origin"
			                                    : "single buffer (+0x58)");
		}
		const int stamp = *reinterpret_cast<int32_t *>(obj + kDObjSkelStamp);
		const float add = g_ikAddW;
		for (int h = 0; h < 2; h++)
		{
			const IkHandRig &r = rig->hand[h];
			IkHandState &st = g_ikHand[h];
			if (!r.ok || !IkCalculated(obj, r.s) || !IkCalculated(obj, r.e) || !IkCalculated(obj, r.w) || !IkCalculated(obj, r.t))
				continue;
			float *mw = mats + 8 * r.w;
			if (st.appliedObj == obj && st.appliedStamp == stamp && memcmp(st.appliedWrist, mw, sizeof(st.appliedWrist)) == 0)
				continue;  // done for this skeleton
			const float *ms = mats + 8 * r.s, *me = mats + 8 * r.e, *ml = mats + 8 * r.t;
			const IkV3 S{ms[4], ms[5], ms[6]}, E{me[4], me[5], me[6]}, W{mw[4], mw[5], mw[6]}, L{ml[4], ml[5], ml[6]};
			const IkQ qW = IkNorm({mw[0], mw[1], mw[2], mw[3]}), qL = IkNorm({ml[0], ml[1], ml[2], ml[3]});
			const float weight = st.weight;
			// Calibration: the anim's own wrist-to-locator relation, from a frame without additives (the first one may have a
			// little: an additive fading in from its reference frame).
			if (weight >= 0.999f && (add <= 1e-3f || (!st.calibrated && add <= 0.05f)))
			{
				{
					st.offPos = IkRot(IkConj(qL), W - L);
					st.offRot = IkNorm(IkMul(IkConj(qL), qW));
					if (!st.calibrated && g_ikDebug)
						Log("ik: %s hand calibrated at additive %.2f: wrist %.3f from tag_ik_loc", h ? "right" : "left", add, IkLen(W - L));
					st.calibrated = true;
					st.calAdd = add;
				}
			}
			st.appliedObj = obj;
			st.appliedStamp = stamp;
			memcpy(st.appliedWrist, mw, sizeof(st.appliedWrist));
			if (!st.calibrated)
			{
				st.slip = -1;  // the trace shows -1 until a clean frame (ADS, no walk / idle_active / recoil) calibrates
				continue;
			}
			const IkV3 T = L + IkRot(qL, st.offPos);
			const IkQ qT = IkNorm(IkMul(qL, st.offRot));
			st.slip = IkLen(T - W);  // measured even when IK doesn't apply (ik_enable=0 with ik_debug=1: the A/B)
			if (!g_ikApply || weight <= 1e-3f || (add <= 1e-3f && !g_ikAlways))
				continue;
			const IkV3 Tb = W + (T - W) * weight;
			const IkQ qTb = IkNlerp(qW, qT, weight);
			IkDeltas k;
			if (!IkSolve2Bone(S, E, W, qW, Tb, qTb, g_ikOrient, st.hint, k))
				continue;
			st.hint = k.hinge;
			for (int i = 0; i < r.n; i++)
			{
				const int b = r.bone[i];
				if (!IkCalculated(obj, b))
					continue;  // built later, from its moved parent
				float *m = mats + 8 * b;
				IkV3 p{m[4], m[5], m[6]};
				IkQ q{m[0], m[1], m[2], m[3]};
				IkApplyDeltas(k, r.group[i], p, q);
				m[0] = q.x, m[1] = q.y, m[2] = q.z, m[3] = q.w;
				m[4] = p.x, m[5] = p.y, m[6] = p.z;
				if (dbl)  // publish as DObjCalcSkel does
				{
					float *o = pub + 8 * b;
					memcpy(o, m, 32);
					if (origin)
						o[4] += origin[0], o[5] += origin[1], o[6] += origin[2];
				}
			}
			memcpy(st.appliedWrist, mw, sizeof(st.appliedWrist));
		}
	}

	using DObjCalcSkelFn = uint64_t (*)(void *obj, uint32_t *partBits, float *origin);
	DObjCalcSkelFn g_ikCalcSkel;
	uint64_t IkCalcSkelHook(void *obj, uint32_t *partBits, float *origin)
	{
		const uint64_t r = g_ikCalcSkel(obj, partBits, origin);
		if (obj == g_ikDObj.load(std::memory_order_relaxed))
		{
			PerfScope t(kPtIkSkel);
			IkAfterSkel(static_cast<uint8_t *>(obj), origin);
		}
		if (obj == g_rgDObj.load(std::memory_order_acquire))
			RgAfterSkel(static_cast<uint8_t *>(obj), origin);  // FLOATROUND (bo3_roundguard.h)
		return r;
	}

	// Called from InstallAdditives once the anim hook is in. Only with ik= lines at start (the hooks cost one compare per
	// skeleton build, but stay out entirely otherwise).
	void InstallIk()
	{
		if (WtDebugSkip("ik"))
			return;
		g_ikConfiguredAtStart = g_ikWeaponCount > 0;
		if ((!g_ikConfiguredAtStart && !RgWantsSkelHook()) || g_ikHooked)  // FLOATROUND: the round guard uses it too
			return;
		// The DObj offsets IK writes through (audit C-3) must be the ones DObjCalcSkel itself uses (r13 = DObj there;
		// IDA 2026-10-01). +0xF8 (the skeleton stamp) isn't used by DObjCalcSkel; IK only reads and compares it.
		struct Anchor
		{
			uintptr_t rva;
			size_t n;
			uint8_t bytes[8];
			const char *what;
		};
		static const Anchor anchorsRetail[] = {
		    // retail DObjCalcSkel 0x1422C0480 (rdi = DObj, r14 = DObj+0x58); the 0x124 flag test is a helper called with 0x400
		    {0x22C04AD, 4, {0x4C, 0x8D, 0x71, 0x58}, "+0x58 skeleton mats (lea r14, [rcx+58h])"},
		    {0x22C04E3, 5, {0x43, 0x8B, 0x4C, 0x0B, 0x70}, "+0xC8 calculated bits (r14+70h)"},
		    {0x22C0637, 4, {0x4D, 0x8B, 0x46, 0x08}, "+0x60 work buffer (mov r8, [r14+8])"},
		    {0x22BE030, 7, {0x66, 0x85, 0x91, 0x24, 0x01, 0x00, 0x00}, "+0x124 flags (test [rcx+124h], dx: the flag helper)"},
		    {0x22C061A, 5, {0xBA, 0x00, 0x04, 0x00, 0x00}, "flag 0x400 (mov edx, 400h)"},
		    {0x22C0522, 6, {0x8B, 0x8F, 0x04, 0x01, 0x00, 0x00}, "+0x104 dup name (mov ecx, [rdi+104h])"},
		    {0x22C05BE, 7, {0x0F, 0xB6, 0x87, 0x22, 0x01, 0x00, 0x00}, "+0x122 model count (movzx eax, [rdi+122h])"},
		};
		static const Anchor anchorsEnh[] = {
		    {0x23F8611, 7, {0x41, 0x0B, 0x8D, 0xC8, 0x00, 0x00, 0x00}, "+0xC8 calculated bits (or ecx, [r13+0C8h])"},
		    {0x23F8907, 4, {0x4D, 0x8D, 0x7D, 0x58}, "+0x58 skeleton mats (lea r15, [r13+58h])"},
		    {0x23F89C5, 4, {0x49, 0x8B, 0x55, 0x60}, "+0x60 work buffer (mov rdx, [r13+60h])"},
		    {0x23F89BB, 8, {0x66, 0x45, 0x85, 0x9D, 0x24, 0x01, 0x00, 0x00}, "+0x124 flags (test [r13+124h], r11w)"},
		    {0x23F879E, 7, {0x41, 0x8B, 0x85, 0x04, 0x01, 0x00, 0x00}, "+0x104 dup name (mov eax, [r13+104h])"},
		    {0x23F8942, 8, {0x45, 0x0F, 0xB6, 0x85, 0x22, 0x01, 0x00, 0x00}, "+0x122 model count (movzx r8d, [r13+122h])"},
		};
		const Anchor *anchors = IsRetailExe() ? anchorsRetail : anchorsEnh;
		const size_t anchorCount = IsRetailExe() ? std::size(anchorsRetail) : std::size(anchorsEnh);
		for (size_t k = 0; k < anchorCount; k++)
			if (const Anchor &a = anchors[k]; !CodeMatches(a.rva, a.bytes, a.n))
			{
				Log("ik: DObjCalcSkel doesn't use the DObj layout IK assumes (%s at +%zx); not hooked", a.what, a.rva);
				return;
			}
		Log("ik: DObjCalcSkel uses the assumed DObj layout (+0x58 / +0x60 / +0xC8 / +0x104 / +0x122 / +0x124)");
		void *orig1 = nullptr, *orig2 = nullptr;
		bool a = RedirectCall(kCallSkelCalcBone, kDObjCalcSkel, reinterpret_cast<void *>(&IkCalcSkelHook), &orig1, "ik: skeleton (bone calc)");
		bool b = RedirectCall(kCallSkelCalcPose, kDObjCalcSkel, reinterpret_cast<void *>(&IkCalcSkelHook), &orig2, "ik: skeleton (pose calc)");
		g_ikCalcSkel = reinterpret_cast<DObjCalcSkelFn>(a ? orig1 : orig2);
		g_ikHooked = a && b;
		Log("ik: %s; ik_enable %d, %d weapon line(s), %d ik_notes line(s)", g_ikHooked ? "hooked" : a || b ? "only one call site hooked: IK OFF" : "not hooked",
		    g_ikEnable, g_ikWeaponCount, g_ikNotesCfgCount);
		// One site in without the other would IK only some skeleton builds: keep the hook (it is the original call) but
		// never activate.
	}
}
#endif
