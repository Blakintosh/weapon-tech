"""Derive the spent-round hide for every bullet layer from its bullets anim, as weapon_tech.cfg lines.

weapon_tech hides spent rounds itself (bo3_additive.h, ApplyAutoAmmoHide: DObj hide bits, as HidePart). Its built-in
order (nearest the follower / bolt first) is only a guess. This script reads each bullet line's xanim_export and writes:

  ammohide_spend=<weapon>,<joint>:<clip>,...   the anim brings rounds to their final (spent) pose at different frames
                                               (belts: sierrax, lmg_light; the P90's stack, which the anim itself
                                               compresses toward the helix): each round is hidden from the clip at which
                                               it reaches that pose and stays; rounds it never moves stay shown.
  ammohide_order=<weapon>,<joint>,...          stack mags whose anim moves every round to its final pose on the same
                                               (last) frame, or none at all (AK, M4): the round nearest the follower / pusher at
                                               rest is spent first (the stack moves up, so that slot empties first);
                                               the last <clip> of the list stay shown.

Spend clip: export frame f (frame 0 is the reference, source frame f-1) plays at clip mag-(f-1) (weapon_tech: frame
k = mag - clip, time (k+1)/(mag+1)), so a round parked from frame f on is hidden while clip <= mag + 1 - f.

Usage: python ammohide_order.py <weapon_tech.cfg> [--bo3 <game dir>] [--write]
  --bo3 <dir>  Black Ops III folder holding xanim_export/ (default: the BO3_DIR environment variable)
  --write      replace the section between the BEGIN/END markers in the cfg (default: print the section);
               the cfg is copied to <cfg>.pre_hideorder.bak once first
The bullet anims named in the cfg's additive=<weapon>,bullet,... lines are looked up by name under <dir>/xanim_export.
Guns with their own ammohide= line are skipped (it wins in the DLL). Long lists continue on '<key>=<weapon>,+,...'
lines (the DLL reads 255 chars a line).
"""
import glob
import os
import re
import shutil
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xanimlib as X  # noqa: E402

BO3 = None  # game folder, set in main() from --bo3 / BO3_DIR
BEGIN = '# ==== BEGIN ammohide order (tools/ammohide_order.py) ===='
OLD_BEGINS = ['# ==== BEGIN ammohide order (bullet_empty_additives\\ammohide_order.py) ====']  # still replaced in place
END = '# ==== END ammohide order ===='
PARK_EPS = 0.05     # inches: two rounds share a parking spot
SETTLE_EPS = 0.02   # inches: a round has reached its final pose


def round_name(n):
    low = n.lower()
    if any(s in low for s in ('follower', 'pusher', 'linkempty', 'mag')):
        return False
    if any(s in low for s in ('bullet', 'round', 'shell')):
        return True
    if low.startswith('j_ammo'):  # BO7 / IW9 rounds: j_ammo_01.., left gun j_ammo_01_le / j_ammo_011 (tag_ammo_* are tags)
        return True
    return re.match(r'j_b_\d', low) is not None


def follower_name(n):
    low = n.lower()
    return 'follower' in low or 'pusher' in low


_exports = None


def find_export(anim):
    global _exports
    if _exports is None:
        _exports = {}
        for p in glob.glob(os.path.join(BO3, 'xanim_export', '**', '*.xanim_export'), recursive=True):
            _exports.setdefault(os.path.splitext(os.path.basename(p))[0].lower(), p)
    return _exports.get(anim.lower())


def derive(weapon, anim, mag):
    path = find_export(anim)
    if not path:
        return None, f'# {weapon}: {anim}: no xanim_export found'
    a = X.read_xanim(path)
    P, F = a['parts'], a['frames']
    root = P.index('tag_weapon') if 'tag_weapon' in P else 0
    rel = np.einsum('fij,fpjk->fpik', np.linalg.inv(F[:, root]), F)
    pos = rel[:, :, :3, 3]
    rounds = [k for k, p in enumerate(P) if round_name(p)]
    if not rounds:
        return None, f'# {weapon}: {anim}: no round joints in the anim'
    n = F.shape[0]
    final = {k: pos[-1, k] for k in rounds}
    moved = {k: np.linalg.norm(pos[:, k] - pos[0, k], axis=1).max() > SETTLE_EPS for k in rounds}
    parked = [k for k in rounds if moved[k]]  # every round the anim moves reaches its final (spent) pose at some frame
    arrive = {}
    for k in parked:
        f = n - 1
        while f > 1 and np.linalg.norm(pos[f - 1, k] - final[k]) < SETTLE_EPS:
            f -= 1
        arrive[k] = f
    if parked and len(set(arrive.values())) >= 2:
        items = sorted(((max(0, mag + 1 - arrive[k]), P[k]) for k in parked), key=lambda x: (-x[0], x[1]))
        return (f'ammohide_spend={weapon},' + ','.join(f'{j}:{c}' for c, j in items),
                f'# {weapon}: {anim}: {len(parked)} of {len(rounds)} rounds reach their spent pose at different clips (spend clips '
                f'{items[0][0]}..{items[-1][0]}); rounds the anim never moves are never hidden')
    fol = [k for k, p in enumerate(P) if follower_name(p)]
    if not fol:
        return None, (f'# {weapon}: {anim}: {len(rounds)} rounds, all parked together or none; no follower / pusher in '
                      f'the anim: weapon_tech\'s built-in order')
    ref = pos[0, fol[0]]
    order = sorted(rounds, key=lambda k: (np.linalg.norm(pos[0, k] - ref), P[k]))
    why = 'all parked on the last frame' if parked else 'no round parked'
    return (f'ammohide_order={weapon},' + ','.join(P[k] for k in order),
            f'# {weapon}: {anim}: {len(rounds)} rounds, {why}: nearest {P[fol[0]]} at rest first (stack mag)')


def split_line(line, cap=240):
    """weapon_tech reads cfg lines 255 chars at a time: continue a long list as '<key>=<weapon>,+,...' lines."""
    head, rest = line.split('=', 1)
    weapon, items = rest.split(',', 1)
    items = items.split(',')
    out, cur = [], f'{head}={weapon}'
    for it in items:
        if len(cur) + 1 + len(it) > cap:
            out.append(cur)
            cur = f'{head}={weapon},+'
        cur += ',' + it
    out.append(cur)
    return out


def find_begin(text):
    """the BEGIN marker (current or old text) present in text, or None"""
    for b in [BEGIN] + OLD_BEGINS:
        if b in text:
            return b
    return None


def main():
    global BO3
    args = sys.argv[1:]
    bo3 = None
    if '--bo3' in args:
        i = args.index('--bo3')
        if i + 1 >= len(args):
            raise SystemExit('--bo3 needs a directory')
        bo3 = args[i + 1]
        del args[i:i + 2]
    write = '--write' in args
    args = [a for a in args if a != '--write']
    if len(args) != 1 or args[0] in ('-h', '--help'):
        print(__doc__)
        raise SystemExit(0 if args and args[0] in ('-h', '--help') else 2)
    BO3 = bo3 or os.environ.get('BO3_DIR')
    if not BO3 or not os.path.isdir(BO3):
        raise SystemExit('set the Black Ops III folder with --bo3 <dir> or the BO3_DIR environment variable')
    cfg = args[0]
    text = open(cfg, encoding='utf-8', errors='replace').read()
    ob = find_begin(text)
    body = (text.split(ob)[0] if ob else text) + (text.split(END, 1)[1] if ob and END in text else '')
    explicit = {m.group(1).lower() for m in re.finditer(r'^ammohide=([^,\s]+),', body, re.M)}
    out = [BEGIN, '# Spent-round hide order derived from each bullets anim (weapon_tech ApplyAutoAmmoHide). Regenerate after',
           '# adding bullet lines; edit by hand only outside this section (an ammohide= line, ammohide_order/_spend/_reverse).']
    seen = set()
    for m in re.finditer(r'^additive=([^,\s]+),bullet,[^,]+,([^,\s]+),[^,\s]+,(\d+)', body, re.M):
        weapon, anim, mag = m.group(1), m.group(2), int(m.group(3))
        if weapon.lower() in seen:
            continue
        seen.add(weapon.lower())
        if weapon.lower() in explicit:
            out.append(f'# {weapon}: has its own ammohide= line')
            continue
        line, note = derive(weapon, anim, mag)
        out.append(note)
        if line:
            out.extend(split_line(line))
    out.append(END)
    section = '\n'.join(out)
    if not write:
        print(section)
        return
    bak = cfg + '.pre_hideorder.bak'
    if not os.path.exists(bak):
        shutil.copy2(cfg, bak)
    text = open(cfg, encoding='utf-8', errors='replace').read()  # re-read right before writing
    ob = find_begin(text)
    if ob and END in text:
        pre, rest = text.split(ob, 1)
        text = pre + section + rest.split(END, 1)[1]
    else:
        text = text.rstrip('\n') + '\n\n' + section + '\n'
    open(cfg, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'wrote {len(out) - 4} line(s) into {cfg}')


if __name__ == '__main__':
    main()
