// Spent-round guard (FLOATROUND, 2026-10-03). Included at the end of bo3_additive.h, after bo3_ik.h (it shares the IK
// DObjCalcSkel hook: IkCalcSkelHook calls RgAfterSkel for the held viewmodel).
//
// Why: the IW / BO7 bullets anims move a spent round far out of the magazine to get rid of it (MW19 AN-94: every round
// ~226 units on its last frame; CW RPD belt: ~2000 units from 14 rounds left; BO7 minigun: the fed belt slides up to 66
// units out of the box). Their engines hide those rounds too; BO3 draws whatever isn't hidden, so a parked round that
// weapon_tech's spent-round hide (BELTHIDE / ammohide=) doesn't cover at that moment floats in front of the camera:
// rounds with names the auto hide doesn't know (j_ammo_*), a hide order that spends rounds in another order than the anim
// parks them, or a frame where the hide bits aren't ours (a weapon change, the engine re-copying its own hide bits).
//
// What: after each viewmodel skeleton build, every round-like bone (bullet / round / shell / ammo / j_b_<n>, not tag_*)
// is measured in the frame of its nearest non-round ancestor (the mag / belt root) against the XModel's base pose. A round
// more than ammohide_park units (default 12) from where the model puts it is parked: it and every bone below it are hidden
// through the DObj hide bits (the same bits HidePart / BELTHIDE use). The hide bits written here are rebuilt every time
// from the engine's own (vm+0x338), the anim hook's (BELTHIDE / ammohide=) and the parked set, so nothing sticks.
// Material-independent: nothing is drawn for a hidden bone whatever its material.
//
// Config: ammohide_guard=0 turns it off; ammohide_park=<units> the distance; ammohide_debug=1 logs the parked set when it
// changes.
#pragma once
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace
{
	bool RgWantsSkelHook() { return g_rgEnable || g_rgDebug != 0; }

	const char *RgBoneName(uint8_t *dobj, int bone)
	{
		const int numModels = dobj[kDObjNumModels];
		auto *const *models = *reinterpret_cast<uint8_t *const *const *>(dobj + kDObjModels);
		const uint8_t *sl = *At<uint8_t *>(kSLTable);
		if (!models || !sl || !FastReadable(models, numModels * 8))
			return "?";
		int start = 0;
		for (int m = 0; m < numModels; m++)
		{
			const uint8_t *xm = models[m];
			if (!xm || !FastReadable(xm, 0x40))
				return "?";
			const int nb = xm[kXModelNumBones];
			if (bone < start + nb)
			{
				const uint32_t *names = *reinterpret_cast<const uint32_t *const *>(xm + 0x10);
				if (!names || !FastReadable(names + (bone - start), 4))
					return "?";
				const uint32_t id = names[bone - start];
				if (!id || id > 0x100000)
					return "?";
				const char *t = reinterpret_cast<const char *>(sl + 28 * static_cast<size_t>(id) + 4);
				return FastReadable(t, 32) ? t : "?";
			}
			start += nb;
		}
		return "?";
	}

	bool RgRoundName(const char *name)
	{
		char low[64];
		size_t j = 0;
		for (; name[j] && j < 63; j++)
			low[j] = static_cast<char>(tolower(static_cast<unsigned char>(name[j])));
		low[j] = 0;
		if (strncmp(low, "tag_", 4) == 0 || strstr(low, "follower") || strstr(low, "pusher") || strstr(low, "mag"))
			return false;
		return strstr(low, "bullet") || strstr(low, "round") || strstr(low, "shell") || strstr(low, "ammo") ||
		       (strncmp(low, "j_b_", 4) == 0 && isdigit(static_cast<unsigned char>(low[4])));
	}

	// v rotated into the frame of q (x y z w, any length): conj(q) v q
	inline void RgInvRotate(const float *q, const float *v, float *out)
	{
		const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
		const float s = n2 > 1e-12f ? 1.0f / sqrtf(n2) : 0.0f;
		const float x = -q[0] * s, y = -q[1] * s, z = -q[2] * s, w = q[3] * s;
		const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
		out[0] = v[0] + w * tx + (y * tz - z * ty);
		out[1] = v[1] + w * ty + (z * tx - x * tz);
		out[2] = v[2] + w * tz + (x * ty - y * tx);
	}

	struct RgRig
	{
		void *dobj = nullptr;
		uint64_t models = 0;
		int numBones = 0;
		bool ok = false;
		int16_t parent[384];
		int count = 0;            // round candidates
		int16_t round[128];
		int16_t anchor[128];      // nearest non-round ancestor in the same model
		float rest[128][3];       // the round in its anchor's frame, base pose
		uint32_t below[128][12];  // the round and every bone under it
		uint32_t parked[12];      // the current parked set (MSB-first, like the hide bits)
	};
	RgRig g_rgRig;

	// The XModel base pose (model space, DObjAnimMat layout: quat, trans, weight) at +0x38, checked: unit quaternions and
	// finite positions, or nullptr.
	const float *RgBaseMats(const uint8_t *xm, int nb)
	{
		const float *base = *reinterpret_cast<const float *const *>(xm + 0x38);
		if (!base || !FastReadable(base, 32u * nb))
			return nullptr;
		for (int i = 0; i < nb; i++)
		{
			const float *m = base + 8 * i;
			const float q2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2] + m[3] * m[3];
			if (!(q2 > 0.9f && q2 < 1.1f) || !std::isfinite(m[4]) || !std::isfinite(m[5]) || !std::isfinite(m[6]))
				return nullptr;
		}
		return base;
	}

	bool RgBuild(RgRig &r, uint8_t *obj, int numBones, uint64_t models)
	{
		r = RgRig{};
		r.dobj = obj, r.models = models, r.numBones = numBones;
		if (!IkBuildParents(obj, static_cast<int16_t>(numBones), r.parent))
			return false;
		// Each model's base mats are in its own space, so a round is measured from an anchor in its own model.
		const int numModels = obj[kDObjNumModels];
		auto *const *ms = *reinterpret_cast<uint8_t *const *const *>(obj + kDObjModels);
		if (!ms || !FastReadable(ms, numModels * 8))
			return false;
		static const float *base[384];
		static int8_t modelOf[384];
		static bool isRound[384];
		int start = 0, badModels = 0;
		for (int m = 0; m < numModels; m++)
		{
			const uint8_t *xm = ms[m];
			if (!xm || !FastReadable(xm, 0x40))
				return false;
			const int nb = xm[kXModelNumBones];
			if (start + nb > numBones)
				return false;
			const float *b = RgBaseMats(xm, nb);
			badModels += !b;
			for (int i = 0; i < nb; i++)
				base[start + i] = b ? b + 8 * i : nullptr, modelOf[start + i] = static_cast<int8_t>(m);
			start += nb;
		}
		for (int b = 0; b < numBones; b++)
			isRound[b] = RgRoundName(RgBoneName(obj, b));
		for (int b = 0; b < numBones && r.count < 128; b++)
		{
			if (!isRound[b] || !base[b])
				continue;
			int a = r.parent[b];
			while (a >= 0 && isRound[a] && modelOf[a] == modelOf[b])
				a = r.parent[a];
			if (a < 0 || modelOf[a] != modelOf[b] || !base[a])
				continue;
			const int k = r.count++;
			r.round[k] = static_cast<int16_t>(b), r.anchor[k] = static_cast<int16_t>(a);
			const float d[3] = {base[b][4] - base[a][4], base[b][5] - base[a][5], base[b][6] - base[a][6]};
			RgInvRotate(base[a], d, r.rest[k]);
			// the round and every bone under it (a bone's parent comes before it)
			r.below[k][b >> 5] |= 0x80000000u >> (b & 31);
			for (int c = b + 1; c < numBones; c++)
			{
				const int p = r.parent[c];
				if (p >= 0 && (r.below[k][p >> 5] & (0x80000000u >> (p & 31))))
					r.below[k][c >> 5] |= 0x80000000u >> (c & 31);
			}
		}
		r.ok = true;
		Log("rg: viewmodel %p (%d bones, %d model(s), %d without a readable base pose): %d round joint(s) guarded", obj, numBones,
		    numModels, badModels, r.count);
		return true;
	}

	// After DObjCalcSkel on the held viewmodel (the caller holds the DObj lock).
	void RgAfterSkel(uint8_t *obj, const float *origin)
	{
		(void)origin;
		if (!g_rgEnable)
			return;
		const int16_t numBones = *reinterpret_cast<int16_t *>(obj + kDObjNumBones);
		const float *pub = *reinterpret_cast<float *const *>(obj + kDObjSkelMats);
		if (numBones <= 0 || numBones > 384 || !pub || !FastReadable(pub, 32u * numBones))
			return;
		RgRig &r = g_rgRig;
		const uint64_t models = *reinterpret_cast<uint64_t *>(obj + kDObjModels);
		if (r.dobj != obj || r.models != models || r.numBones != numBones)
			if (!RgBuild(r, obj, numBones, models))
				return;
		if (!r.ok || !r.count)
			return;
		const uint32_t *calc = reinterpret_cast<const uint32_t *>(obj + kDObjSkelBits);
		auto calculated = [&](int b) { return (calc[b >> 5] & (0x80000000u >> (b & 31))) != 0; };
		bool changed = false;
		for (int k = 0; k < r.count; k++)
		{
			const int b = r.round[k], a = r.anchor[k];
			if (!calculated(b) || !calculated(a))
				continue;  // not built in this call: keeps its state
			const float *mb = pub + 8 * b, *ma = pub + 8 * a;
			const float d[3] = {mb[4] - ma[4], mb[5] - ma[5], mb[6] - ma[6]};
			float local[3];
			RgInvRotate(ma, d, local);
			const float dx = local[0] - r.rest[k][0], dy = local[1] - r.rest[k][1], dz = local[2] - r.rest[k][2];
			const bool parked = dx * dx + dy * dy + dz * dz > g_rgPark * g_rgPark;
			const bool was = (r.parked[b >> 5] & (0x80000000u >> (b & 31))) != 0;
			if (parked != was)
			{
				r.parked[b >> 5] ^= 0x80000000u >> (b & 31);
				changed = true;
			}
		}
		// The hide bits: the engine's | the anim hook's | everything under a parked round. Rebuilt from those every time,
		// so a round that comes back (a reload) shows again.
		uint32_t bits[12];
		const uint32_t *vmBits = g_rgVmBits;
		if (g_rgAnimSet && vmBits && FastReadable(vmBits, 48))
			for (int i = 0; i < 12; i++)
				bits[i] = vmBits[i] | g_rgAnimBits[i];
		else
			memcpy(bits, obj + kDObjHideBits, sizeof(bits));
		int parkedCount = 0;
		for (int k = 0; k < r.count; k++)
		{
			const int b = r.round[k];
			if (!(r.parked[b >> 5] & (0x80000000u >> (b & 31))))
				continue;
			parkedCount++;
			for (int i = 0; i < 12; i++)
				bits[i] |= r.below[k][i];
		}
		memcpy(obj + kDObjHideBits, bits, sizeof(bits));
		if (changed && g_rgDebug)
		{
			char list[512];
			size_t n = 0;
			list[0] = 0;
			for (int k = 0; k < r.count && n < sizeof(list) - 40; k++)
				if (r.parked[r.round[k] >> 5] & (0x80000000u >> (r.round[k] & 31)))
					n += sprintf_s(list + n, sizeof(list) - n, " %s", RgBoneName(obj, r.round[k]));
			Log("rg: %d of %d round(s) parked (> %.0f units from their base pose):%s", parkedCount, r.count, g_rgPark, list);
		}
	}
}
