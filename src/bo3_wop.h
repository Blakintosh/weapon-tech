// IW8 (MW2019) weapon offset patterns ("WOP"), evaluated per frame for BO3 weapons.
// Ported from the decompiled IW8 bg_weapon_offsets.cpp (OpenIW8): WeaponOffsetPattern::Update,
// CalculatePatternWeight, WeaponOffsetCurve::GetCurrentCurveFraction / CalculateBlendCurrentFractionDelta /
// GetCurrentFullAutoScale. Research notes: %TEMP%\iw8rec\REPORT_recoil.md.
//
// Each pattern = shape (keyframe 1,1,1 / noisy sine / random spline / sine) * magnitude * strength, where
// strength = curve fraction * lerp(hipScale, 1, ads). Curves: hold-fire slow/fast (retargetable eased blend
// while firing), kick / snap-decay (per shot, scaled by a full-auto ramp), ADS-in transient, always on.
// Outputs, summed per target: view origin, view angles, weapon origin, weapon angles (IW axes: origin
// forward/left/up... as authored; angles pitch/yaw/roll in degrees). Weapon angle patterns with a
// rotationOffset pivot about a point that far forward, which adds P - R*P to the weapon origin.
//
// Config (bo3_additive.cfg; generate with iw8_wop_cfg.py from an IW8 weapon def dump):
//   wop_weapon=<weapon>,<fire time ms>
//   wop_curve=<weapon>,<curve 0-5>,<blend>,<decay>,<shotDecayFireTimeFrac>,<hold>,<adsBegin>,<adsEnd>,<interpIn>,<interpOut>
//   wop=<weapon>,<curve>,<pattern>,<target>,<frequency>,<blendTime>,<mx>,<my>,<mz>,<hipScale>,<rotationOffset>,
//       <fullAutoScale>,<fullAutoBullets>,<fullAutoDecay>,<kickOrSnapDecayIndex>
//   wop_alias=<weapon>,<source weapon>[,<fire time ms>]   (a PaP or other variant: a copy of an earlier block)
//   wop_kickreturn=<weapon>,<0|1>[,<viewKickMaintainFraction>[,<disableInputDrivenViewReturnDampening 0|1>]]
//       (IW8 view-kick return, needs the global wop_kickreturn=1; put it before the weapon's wop_alias lines)
#pragma once
#include <cmath>
#include "bo3_iw8kick.h"

namespace
{
	enum WopCurveId { WOBC_HOLD_SLOW, WOBC_HOLD_FAST, WOBC_KICK, WOBC_SNAP_DECAY, WOBC_ADS, WOBC_ALWAYS_ON };
	enum WopPatternId { WOP_KEYFRAME, WOP_NOISY_SINE, WOP_RANDOM_SQUARE, WOP_SINE };
	enum WopTarget { WOTT_VIEW_ORIGIN, WOTT_VIEW_ANGLES, WOTT_WEAPON_ORIGIN, WOTT_WEAPON_ANGLES };

	struct WopCurve
	{
		float blend, decay, shotDecayFrac, hold, adsBegin, adsEnd;
		int interpIn, interpOut;
	};
	struct WopPattern
	{
		int curve, pattern, target;
		float frequency, blendTime, mag[3], hipScale, rotationOffset, fullAutoScale;
		int fullAutoBullets;
		float fullAutoDecay;
		int kickIndex;
		// random-square spline cache (IW8 BSplineRelaxedCBezier: 15 cubic Bezier curves)
		int splinePeriod = INT32_MIN;
		float bez[15][4][3];
		float bezLen[15], bezTotal;
	};
	struct WopBlend  // a retargetable eased blend (hold-fire and ADS curves)
	{
		double t0 = -1e9;
		float start = 0;
		bool in = false;
	};
	struct WopKick  // an IW8 angular kick set: from shot `bullet` of a burst on
	{
		int ads, gun, bullet;
		float dir, dev, strengthMin, strengthMax, pitchScale;
	};
	struct WopWeapon
	{
		char weapon[64];
		char source[64] = {};  // wop_alias source (per-weapon camera lines fall back to it)
		float fireTime = 0.1f;  // seconds
		WopCurve curves[6] = {};
		WopPattern patterns[16];
		int count = 0;
		WopKick kicks[24];
		int kickCount = 0;
		// kick percent: start -> end over shots [kickStartBullets, kickEndBullets]; [hip/ads][gun/view]
		int kickStartBullets = 0, kickEndBullets = 1;
		float kickStart[2][2] = {{1, 1}, {1, 1}}, kickEnd[2][2] = {{1, 1}, {1, 1}};
		int kickShot = 0;
		double lastKick = -1e9;
		uint32_t kickSeed = 0x1234567;
		// IW8 kick springs (bo3_iw8kick.h), replacing BO3's integrators when present: [0 view|1 gun][0 hip|1 ads]
		Iw8KickParams springs[2][2] = {};
		bool hasSpring[2] = {};
		Iw8KickState viewKick, gunKick;
		Iw8ViewKickReturn viewReturn;  // CgViewSystem::UpdateViewKickState (bo3_iw8kick.h, KickReturn in bo3_additive.h)
		bool kickReturn = true;        // wop_kickreturn=<weapon>,<0|1>,..
		float kickMaintain = 0.0f;     // viewKickMaintainFraction
		bool kickNoDampening = false;  // disableInputDrivenViewReturnDampening
		double shotIntervalSum = 0;    // measured shot cadence, to catch a wop_weapon fire time that doesn't match the GDT
		int shotIntervals = 0;
		bool cadenceWarned = false;
		float tilt[2][4] = {};  // [hip|ads] pitch, yaw, roll factor, pivot offset (wop_tilt=)
		double lastSpringTime = -1;
		int variant = -1;
		// state
		int lastClip = -1;
		double lastShot = -1e9, sustainStart = 0, sustainStop = 0, burstSeed = 0, adsStart = 0;
		bool sustained = false;
		float fullAuto[8] = {};  // full-auto fraction carried into the next burst (IW packs this in 4 bits)
		WopBlend hold[2], ads;
		float lastAds = 0;
		bool adsLatched = false;
		// outputs
		float viewOrigin[3], viewAngles[3], weaponOrigin[3], weaponAngles[3];
	};
	constexpr int kMaxWops = 128;  // every MW19 gun in Karelia plus its PaP and variants (~60) with headroom
	WopWeapon g_wops[kMaxWops];
	int g_wopCount;

	WopWeapon *FindWop(const char *weapon, bool create)
	{
		for (int i = 0; i < g_wopCount; i++)
			if (strcmp(g_wops[i].weapon, weapon) == 0)
				return &g_wops[i];
		if (!create || g_wopCount >= kMaxWops)
			return nullptr;
		WopWeapon &w = g_wops[g_wopCount++];
		strcpy_s(w.weapon, weapon);
		return &w;
	}

	// wop_weapon= / wop_curve= / wop= lines (the part after '=').
	bool ParseWop(const char *key, const char *value)
	{
		char weapon[64] = {};
		const char *comma = strchr(value, ',');
		if (!comma || comma - value >= 64)
			return false;
		memcpy(weapon, value, comma - value);
		if (strcmp(key, "wop_alias") == 0)
		{
			// The source's whole config under another weapon name (PaP and other variants are separate
			// weapons); the source must come earlier in the file. Optional fire time for a faster/slower PaP.
			char source[64] = {};
			int ms = 0;
			if (sscanf_s(comma + 1, "%63[^,],%d", source, static_cast<unsigned>(sizeof(source)), &ms) < 1)
				return false;
			const WopWeapon *src = FindWop(source, false);
			if (!src || FindWop(weapon, false))
				return false;
			WopWeapon *w = FindWop(weapon, true);
			if (!w)
				return false;
			*w = *src;
			strcpy_s(w->weapon, weapon);
			strcpy_s(w->source, src->source[0] ? src->source : source);
			if (ms > 0)
				w->fireTime = ms / 1000.0f;
			return true;
		}
		WopWeapon *w = FindWop(weapon, true);
		if (!w)
			return false;
		const char *rest = comma + 1;
		if (strcmp(key, "wop_weapon") == 0)
		{
			int ms = 0;
			if (sscanf_s(rest, "%d", &ms) != 1 || ms <= 0)
				return false;
			w->fireTime = ms / 1000.0f;
			return true;
		}
		if (strcmp(key, "wop_curve") == 0)
		{
			int i;
			WopCurve c{};
			if (sscanf_s(rest, "%d,%f,%f,%f,%f,%f,%f,%d,%d", &i, &c.blend, &c.decay, &c.shotDecayFrac, &c.hold, &c.adsBegin,
			             &c.adsEnd, &c.interpIn, &c.interpOut) != 9 || i < 0 || i > 5)
				return false;
			w->curves[i] = c;
			return true;
		}
		if (strcmp(key, "wop_kickpct") == 0)
		{
			// start bullets, end bullets, hip gun start/end, hip view start/end, ads gun start/end, ads view start/end
			float v[8];
			if (sscanf_s(rest, "%d,%d,%f,%f,%f,%f,%f,%f,%f,%f", &w->kickStartBullets, &w->kickEndBullets, &v[0], &v[1], &v[2],
			             &v[3], &v[4], &v[5], &v[6], &v[7]) != 10)
				return false;
			for (int a = 0; a < 2; a++)
				for (int g = 0; g < 2; g++)  // g: 1 = gun, 0 = view
				{
					int i = a * 4 + (g ? 0 : 2);
					w->kickStart[a][g] = v[i];
					w->kickEnd[a][g] = v[i + 1];
				}
			return true;
		}
		if (strcmp(key, "wop_kickreturn") == 0)
		{
			int on = 1, noDamp = 0;
			float maintain = 0.0f;
			if (sscanf_s(rest, "%d,%f,%d", &on, &maintain, &noDamp) < 1 || maintain < 0.0f || maintain > 1.0f)
				return false;
			w->kickReturn = on != 0;
			w->kickMaintain = maintain;
			w->kickNoDampening = noDamp != 0;
			return true;
		}
		if (strcmp(key, "wop_tilt") == 0)
		{
			float *t = &w->tilt[0][0];
			return sscanf_s(rest, "%f,%f,%f,%f,%f,%f,%f,%f", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 8;
		}
		if (strcmp(key, "wop_spring") == 0)
		{
			int gun, ads;
			Iw8KickParams p{};
			if (sscanf_s(rest, "%d,%d,%f,%f,%f,%f,%f", &gun, &ads, &p.accel, &p.returnAccelScale, &p.returnSpeedCurveScale,
			             &p.maxPitch, &p.maxYaw) != 7 || gun < 0 || gun > 1 || ads < 0 || ads > 1)
				return false;
			w->springs[gun][ads] = p;
			w->hasSpring[gun] = true;
			return true;
		}
		if (strcmp(key, "wop_kick") == 0)
		{
			if (w->kickCount >= 24)
				return false;
			WopKick &k = w->kicks[w->kickCount];
			if (sscanf_s(rest, "%d,%d,%d,%f,%f,%f,%f,%f", &k.ads, &k.gun, &k.bullet, &k.dir, &k.dev, &k.strengthMin,
			             &k.strengthMax, &k.pitchScale) != 8)
				return false;
			w->kickCount++;
			return true;
		}
		if (w->count >= 16)
			return false;
		WopPattern &p = w->patterns[w->count];
		if (sscanf_s(rest, "%d,%d,%d,%f,%f,%f,%f,%f,%f,%f,%f,%d,%f,%d", &p.curve, &p.pattern, &p.target, &p.frequency,
		             &p.blendTime, &p.mag[0], &p.mag[1], &p.mag[2], &p.hipScale, &p.rotationOffset, &p.fullAutoScale,
		             &p.fullAutoBullets, &p.fullAutoDecay, &p.kickIndex) != 14)
			return false;
		if (p.curve < 0 || p.curve > 5 || p.pattern < 0 || p.pattern > 3 || p.target < 0 || p.target > 3 || p.kickIndex > 7)
			return false;
		w->count++;
		return true;
	}

	// WeaponOffsetCurve::CalculateBlendCurrentFractionDelta's easing.
	float WopEase(float u, int type)
	{
		if (u <= 0.0f || u >= 1.0f)
			return u <= 0.0f ? 0.0f : 1.0f;
		switch (type)
		{
		case 1: return u * u * u;
		case 2: return (u - 1) * (u - 1) * (u - 1) + 1;
		case 3: return u * u * u * u;
		case 4: return 1 - (u - 1) * (u - 1) * (u - 1) * (u - 1);
		case 5: return powf(2.0f, (u - 1) * 10.0f);
		case 6: return 1 - powf(2.0f, u * -10.0f);
		default: return u;
		}
	}

	// start + val * ease(u): toward 1 while blending in, toward 0 while blending out, over |val| * time.
	float WopBlendFraction(const WopBlend &b, const WopCurve &c, double now)
	{
		float val = b.in ? 1.0f - b.start : -b.start;
		float T = fabsf(val) * (b.in ? c.blend : c.decay);
		float u = T > 1e-6f ? static_cast<float>((now - b.t0) / T) : 1.0f;
		u = u < 0 ? 0 : u > 1 ? 1 : u;
		float f = b.start + val * WopEase(u, b.in ? c.interpIn : c.interpOut);
		return f < 0 ? 0 : f > 1 ? 1 : f;
	}

	void WopRetarget(WopBlend &b, const WopCurve &c, double now, bool in)
	{
		if (b.in == in)
			return;
		b.start = WopBlendFraction(b, c, now);
		b.t0 = now;
		b.in = in;
	}

	// BG_WeaponOffsets_GetFullAutoScale: 0 -> 1 over fullAutoBullets shots of sustained fire, carried over
	// (and decayed over fullAutoDecay * F seconds) between bursts. The curve uses lerp(1, fullAutoScale, F).
	float WopFullAutoFraction(const WopWeapon &w, const WopPattern &p, double now)
	{
		if (p.kickIndex < 0)
			return 0.0f;
		float span = w.fireTime * (p.fullAutoBullets < 1 ? 1 : p.fullAutoBullets);
		float carried = w.fullAuto[p.kickIndex];
		if (w.sustained)
		{
			float t = static_cast<float>(now - w.sustainStart) + carried * span;
			return t >= span ? 1.0f : t / span;
		}
		float D = carried * p.fullAutoDecay;
		if (D <= 0)
			return 0.0f;
		float F = carried - static_cast<float>(now - w.sustainStop) * carried / D;
		return F < 0 ? 0 : F;
	}

	// IW8 BG_random / BG_flrand / BG_srand, bit-exact (PDB build 0x104FC10..0x104FCD0).
	uint32_t WopRand(uint32_t &seed)
	{
		seed = seed * 0x343FDu + 0x269EC3u;
		return seed >> 17;  // 15 bits
	}
	float WopRandRange(uint32_t &seed, float lo, float hi)
	{
		return static_cast<float>(WopRand(seed)) * (hi - lo) * (1.0f / 32768.0f) + lo;
	}
	void WopSrand(uint32_t &seed) { seed = 0x098520C4u - seed * 0x2200AFAFu; }

	// RANDOM_PATTERNS[0] (PDB build 0x3F0D5D0), {xmin,xmax,ymin,ymax,zmin,zmax} per point: a walk round the four
	// quadrants. Pattern 1 is unreachable (BG_irand(0,1) is always 0).
	constexpr float kWopRandomPattern[4][6] = {{-1, -0.1f, -1, -0.1f, -1, 1}, {0.1f, 1, 0.1f, 1, -1, 1},
	                                           {-1, -0.1f, 0.1f, 1, -1, 1}, {0.1f, 1, -1, -0.1f, -1, 1}};
	inline void WopBezier(const float c[4][3], float u, float o[3])
	{
		const float v = 1 - u, b0 = v * v * v, b1 = 3 * v * v * u, b2 = 3 * v * u * u, b3 = u * u * u;
		for (int j = 0; j < 3; j++)
			o[j] = b0 * c[0][j] + b1 * c[1][j] + b2 * c[2][j] + b3 * c[3][j];
	}
	// BG_BuildRandomPatternSpline + BG_BSpline_RelaxedCBezier_Build(points, 15, passThru 0, looped 1).
	void WopBuildRandomSpline(WopPattern &p, uint32_t seed)
	{
		float P[15][3] = {};
		WopSrand(seed);
		WopRand(seed);  // BG_irand(0, 1): one draw, always pattern 0
		for (int k = 1; k < 15; k++)
			for (int j = 0; j < 3; j++)
				P[k][j] = WopRandRange(seed, kWopRandomPattern[(k - 1) % 4][2 * j], kWopRandomPattern[(k - 1) % 4][2 * j + 1]);
		float prev[3] = {P[0][0], P[0][1], P[0][2]};
		for (int i = 0; i < 15; i++)
		{
			const float *a = P[i], *b = P[(i + 1) % 15], *c = P[(i + 2) % 15];
			for (int j = 0; j < 3; j++)
			{
				p.bez[i][0][j] = prev[j];
				p.bez[i][1][j] = 0.33333334f * b[j] + 0.66666669f * a[j];
				p.bez[i][2][j] = 0.66666669f * b[j] + 0.33333334f * a[j];
				p.bez[i][3][j] = (0.66666669f * b[j] + 0.16666667f * a[j]) + 0.16666667f * c[j];
				prev[j] = p.bez[i][3][j];
			}
		}
		for (int j = 0; j < 3; j++)
			p.bez[0][0][j] = p.bez[14][3][j];  // looped: the first curve starts where the last ends
		p.bezTotal = 0;
		for (int i = 0; i < 15; i++)  // arc length: 64 chords per curve
		{
			float a[3], b[3], len = 0;
			WopBezier(p.bez[i], 0, a);
			for (int k = 1; k <= 64; k++)
			{
				WopBezier(p.bez[i], k * 0.015625f, b);
				len += sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
				a[0] = b[0];
				a[1] = b[1];
				a[2] = b[2];
			}
			p.bezLen[i] = len;
			p.bezTotal += len;
		}
	}

	// WOP_RANDOM_SQUARE: a new IW8 random spline per period, seeded by adsStartTime + K*(period+1) with K =
	// 3907/4057/5857/5443 per target, evaluated by arc length (BG_BSpline_FindBezierCurve). Like IW8, it jumps a
	// little at each period boundary.
	void WopSpline(WopPattern &p, const WopWeapon &w, float phase, int period, float out[3])
	{
		if (p.splinePeriod != period)
		{
			static const uint32_t kP[4] = {3907, 4057, 5857, 5443};
			WopBuildRandomSpline(p, static_cast<uint32_t>(w.adsStart * 1000.0) + kP[p.target] * static_cast<uint32_t>(period + 1));
			p.splinePeriod = period;
		}
		const float t = phase < 0 ? 0 : phase > 1 ? 1 : phase, s = t * p.bezTotal;
		float acc = 0;
		for (int i = 0; i < 15; i++)
		{
			if (s >= acc && s <= p.bezLen[i] + acc)
			{
				float u = p.bezLen[i] > 0 ? (s - acc) / p.bezLen[i] : 0;
				WopBezier(p.bez[i], u < 0 ? 0 : u > 1 ? 1 : u, out);
				return;
			}
			acc += p.bezLen[i];
		}
		for (int j = 0; j < 3; j++)
			out[j] = p.bez[0][0][j];
	}

	// IW AnglesToAxis (pitch, yaw, roll in degrees): forward, left... as the engine's; used for the pivot term.
	void WopAnglesToAxis(const float a[3], float axis[3][3])
	{
		const float d = 3.14159265f / 180.0f;
		float sp = sinf(a[0] * d), cp = cosf(a[0] * d), sy = sinf(a[1] * d), cy = cosf(a[1] * d), sr = sinf(a[2] * d), cr = cosf(a[2] * d);
		axis[0][0] = cp * cy; axis[0][1] = cp * sy; axis[0][2] = -sp;
		axis[1][0] = sr * sp * cy - cr * sy; axis[1][1] = sr * sp * sy + cr * cy; axis[1][2] = sr * cp;
		axis[2][0] = cr * sp * cy + sr * sy; axis[2][1] = cr * sp * sy - sr * cy; axis[2][2] = cr * cp;
	}

	// One shot's IW8 angular kick: the set for this shot of the burst (the last one whose first bullet has
	// been reached), angle dir +- dev/2 degrees from straight up, strength min..max, times the kick percent
	// (start -> end over the kick bullets). Returns false if the weapon has no set for this mode.
	bool WopShotKick(WopWeapon &w, bool ads, bool gun, int shot, float &pitch, float &yaw)
	{
		const WopKick *set = nullptr;
		for (int i = 0; i < w.kickCount; i++)
		{
			const WopKick &k = w.kicks[i];
			if (k.ads == static_cast<int>(ads) && k.gun == static_cast<int>(gun) && shot >= k.bullet &&
			    (!set || k.bullet >= set->bullet))
				set = &k;
		}
		if (!set)
			return false;
		const float d = 3.14159265f / 180.0f;
		float theta = (set->dir + WopRandRange(w.kickSeed, -0.5f, 0.5f) * set->dev) * d;
		float S = WopRandRange(w.kickSeed, set->strengthMin, set->strengthMax);
		// BG_WeaponFireRecoil: the starting percent while the starting-kick timer runs (the first
		// kickStartBullets shots), the ending percent once the ending timer has run out (kickEndBullets shots),
		// full kick in between. A step, not a ramp.
		float pct = shot < w.kickStartBullets ? w.kickStart[ads][gun] : shot >= w.kickEndBullets ? w.kickEnd[ads][gun] : 1.0f;
		// BG_CalculateKickPolar: pitch = -S cos(theta), yaw = -S sin(theta), both as angular velocities (negative
		// pitch = up; negative dir pulls left: Kilo 141 sets are all -10..-30 and it pulls up-left).
		pitch = -S * cosf(theta) * pct;  // IW8 never reads PitchScale for players (PDB build)
		yaw = -S * sinf(theta) * pct;
		return true;
	}

	// Per frame, for the held weapon. clip = rounds in the clip (a drop is a shot), ads = 0..1.
	void UpdateWop(WopWeapon &w, int clip, float ads, double now)
	{
		// Shots and sustained fire. IW8 ends sustained fire when the weapon leaves the firing state; between
		// automatic shots it stays in it, so "within a fire time and a half of the last shot" stands in.
		if (w.lastClip >= 0 && clip >= 0 && clip < w.lastClip)
		{
			if (!w.sustained)
			{
				for (int i = 0; i < w.count; i++)  // carry the decayed full-auto fraction into the new burst
					if (w.patterns[i].kickIndex >= 0)
						w.fullAuto[w.patterns[i].kickIndex] = WopFullAutoFraction(w, w.patterns[i], now);
				w.sustained = true;
				w.sustainStart = now;
				w.burstSeed = now;
			}
			w.lastShot = now;
		}
		w.lastClip = clip;
		if (w.sustained && now - w.lastShot > w.fireTime * 1.5)
		{
			for (int i = 0; i < w.count; i++)
				if (w.patterns[i].kickIndex >= 0)
					w.fullAuto[w.patterns[i].kickIndex] = WopFullAutoFraction(w, w.patterns[i], now);
			w.sustained = false;
			w.sustainStop = now;
		}
		for (int h = 0; h < 2; h++)
		{
			const WopCurve &c = w.curves[h];
			bool in = w.sustained;
			if (in && c.shotDecayFrac > 0 && c.shotDecayFrac < 1)  // releases between shots
				in = now - w.lastShot < c.shotDecayFrac * w.fireTime;
			WopRetarget(w.hold[h], c, now, in);
		}
		// ADS-in transient: starts as the gun passes adsBegin on the way up, holds, decays; resets at the hip.
		const WopCurve &ac = w.curves[WOBC_ADS];
		if (ads > ac.adsBegin && w.lastAds <= ac.adsBegin)
		{
			w.ads = WopBlend{now, 0.0f, true};
			w.adsStart = now;
			w.adsLatched = true;
		}
		if (w.adsLatched && w.ads.in && now - w.ads.t0 >= ac.blend + ac.hold)
			WopRetarget(w.ads, ac, now, false);
		if (ads <= ac.adsBegin)
		{
			w.adsLatched = false;
			w.ads = WopBlend{};
		}
		w.lastAds = ads;

		float *outs[4] = {w.viewOrigin, w.viewAngles, w.weaponOrigin, w.weaponAngles};
		for (float *o : outs)
			o[0] = o[1] = o[2] = 0.0f;
		for (int i = 0; i < w.count; i++)
		{
			WopPattern &p = w.patterns[i];
			const WopCurve &c = w.curves[p.curve];
			float f = 0.0f;
			switch (p.curve)
			{
			case WOBC_HOLD_SLOW:
			case WOBC_HOLD_FAST: f = WopBlendFraction(w.hold[p.curve], c, now); break;
			case WOBC_ADS: f = w.adsLatched ? WopBlendFraction(w.ads, c, now) : 0.0f; break;
			case WOBC_ALWAYS_ON: f = 1.0f; break;
			default:  // kick / snap decay, from the last shot
			{
				float bt = p.blendTime > 0 ? p.blendTime : c.blend;
				if (w.lastShot < -1e8 || bt <= 0)
					break;
				float u = static_cast<float>((now - w.lastShot) / bt);
				u = u < 0 ? 0 : u > 1 ? 1 : u;
				float s = p.curve == WOBC_KICK ? sinf(u * 3.14159265f) : 1.0f - sinf(u * 3.14159265f * 0.5f);
				float F = WopFullAutoFraction(w, p, now);
				f = (s < 0 ? 0 : s > 1 ? 1 : s) * (F * p.fullAutoScale + (1.0f - F));
			}
			}
			if (f < 1e-6f)
				continue;
			float strength = f * (p.curve == WOBC_ADS ? 1.0f : (1.0f - ads) * p.hipScale + ads);

			float shape[3] = {1, 1, 1};
			if (p.pattern == WOP_NOISY_SINE && p.frequency > 0)
			{
				uint32_t seed = static_cast<uint32_t>(w.burstSeed * 1000.0);
				float r1 = WopRandRange(seed, 1.05f, 1.15f), r2 = WopRandRange(seed, 1.2f, 1.25f);
				float a = static_cast<float>(fmod(now, 1000.0)) / p.frequency;
				shape[0] = sinf(a);
				shape[1] = sinf(a * r1);
				shape[2] = sinf(a * r2);
			}
			else if (p.pattern == WOP_SINE && p.frequency > 0)
				shape[0] = shape[1] = shape[2] = sinf(static_cast<float>(fmod(now, 1000.0)) / p.frequency);
			else if (p.pattern == WOP_RANDOM_SQUARE && p.frequency > 0)
			{
				double x = now / p.frequency;
				int period = static_cast<int>(floor(x));
				WopSpline(p, w, static_cast<float>(x - period), period, shape);
			}
			float v[3];
			for (int j = 0; j < 3; j++)
			{
				v[j] = shape[j] * p.mag[j] * strength;
				outs[p.target][j] += v[j];
			}
			// BG_GetNewOriginForRotationWithOffset: rotate about a point rotationOffset forward.
			if (p.target == WOTT_WEAPON_ANGLES && p.rotationOffset != 0.0f)
			{
				float axis[3][3];
				WopAnglesToAxis(v, axis);
				for (int j = 0; j < 3; j++)
					w.weaponOrigin[j] += p.rotationOffset * ((j == 0 ? 1.0f : 0.0f) - axis[0][j]);
			}
		}
	}
}
