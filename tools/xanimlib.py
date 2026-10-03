"""XANIM_EXPORT / XMODEL_EXPORT text helpers for the recoil-additive batch.

xanim_export parts are world-space (OFFSET + X/Y/Z axis rows), one block per part per frame.
"""
import math

import numpy as np


def read_xanim(path):
    """-> dict(parts=[names], fps, frames=np.array[F, P, 4, 4] world matrices (row axes, translation in col 3))."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = [l.strip() for l in f]
    parts, fps, nframes = [], 30.0, 0
    i = 0
    while i < len(lines):
        t = lines[i].split()
        if not t:
            i += 1
            continue
        if t[0] == 'PART' and len(t) >= 3:
            parts.append(' '.join(t[2:]).strip('"'))
        elif t[0] == 'FRAMERATE':
            fps = float(t[1])
        elif t[0] == 'NUMFRAMES':
            nframes = int(t[1])
            i += 1
            break
        i += 1
    P = len(parts)
    frames = np.zeros((nframes, P, 4, 4))
    f = -1
    p = -1
    while i < len(lines):
        t = lines[i].split()
        if not t:
            i += 1
            continue
        if t[0] == 'FRAME':
            f = int(t[1])
        elif t[0] == 'PART':
            p = int(t[1])
        elif t[0] == 'OFFSET':
            m = frames[f, p]
            m[3, 3] = 1.0
            m[:3, 3] = [float(x) for x in t[1:4]]
        elif t[0] in ('X', 'Y', 'Z'):
            row = 'XYZ'.index(t[0])
            # store axes as COLUMNS so R @ v maps local -> world
            frames[f, p][:3, row] = [float(x) for x in t[1:4]]
        i += 1
    return {'parts': parts, 'fps': fps, 'frames': frames, 'path': path}


def read_xmodel_hierarchy(path):
    """-> {bone: parent or None} from an XMODEL_EXPORT."""
    names, parents = [], []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            t = line.split()
            if len(t) >= 4 and t[0] == 'BONE':
                names.append(' '.join(t[3:]).strip('"'))
                parents.append(int(t[2]))
            elif t and t[0] in ('VERT', 'NUMVERTS', 'NUMFACES'):
                break
    return {n: (names[p] if p >= 0 else None) for n, p in zip(names, parents)}


def rot_deg(Ra, Rb):
    # atan2 form: acos of the trace loses ~0.1 deg to rounding near zero
    R = Ra.T @ Rb
    c = (np.trace(R) - 1) / 2
    s = 0.5 * math.sqrt((R[2, 1] - R[1, 2]) ** 2 + (R[0, 2] - R[2, 0]) ** 2 + (R[1, 0] - R[0, 1]) ** 2)
    return math.degrees(math.atan2(s, c))


def local(frames, pi, qi):
    """local transform of part pi under part qi, per frame -> [F,4,4]."""
    W = frames[:, pi]
    Q = frames[:, qi]
    return np.linalg.inv(Q) @ W


def motion_world(anim):
    """per part: (max translation, max rotation deg, peak frame) vs frame 0, world space."""
    fr = anim['frames']
    out = {}
    for pi, name in enumerate(anim['parts']):
        best = (0.0, 0.0, 0)
        for f in range(fr.shape[0]):
            dt = float(np.linalg.norm(fr[f, pi, :3, 3] - fr[0, pi, :3, 3]))
            dr = rot_deg(fr[0, pi, :3, :3], fr[f, pi, :3, :3])
            if dt + dr > best[0] + best[1]:
                best = (dt, dr, f)
        out[name] = best
    return out


def motion_local(anim, hierarchy):
    """per part: max local change vs frame 0 (translation, deg). Parent = nearest skeleton ancestor in the export
    (None -> world)."""
    fr = anim['frames']
    idx = {n: i for i, n in enumerate(anim['parts'])}
    out = {}
    for name, pi in idx.items():
        q = hierarchy.get(name)
        while q is not None and q not in idx:
            q = hierarchy.get(q)
        if q is None:
            L = fr[:, pi]
        else:
            L = local(fr, pi, idx[q])
        dt = max(float(np.linalg.norm(L[f, :3, 3] - L[0, :3, 3])) for f in range(fr.shape[0]))
        dr = max(rot_deg(L[0, :3, :3], L[f, :3, :3]) for f in range(fr.shape[0]))
        out[name] = (dt, dr, q)
    return out


def ancestors(name, hierarchy):
    q = hierarchy.get(name)
    while q is not None:
        yield q
        q = hierarchy.get(q)


def write_subset(src_path, dst_path, keep):
    """Rewrite an xanim_export keeping only the named parts (renumbered, export order kept)."""
    with open(src_path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    # header part table
    names = []
    for l in lines:
        t = l.split()
        if len(t) >= 3 and t[0] == 'PART':
            names.append(' '.join(t[2:]).strip('"'))
        if t and t[0] == 'FRAMERATE':
            break
    keep_idx = [i for i, n in enumerate(names) if n in keep]
    remap = {old: new for new, old in enumerate(keep_idx)}
    out = []
    i = 0
    header_done = False
    while i < len(lines):
        l = lines[i]
        t = l.split()
        if not header_done:
            if t and t[0] == 'NUMPARTS':
                out.append(f'NUMPARTS {len(keep_idx)}')
                i += 1
                continue
            if len(t) >= 3 and t[0] == 'PART':
                old = int(t[1])
                if old in remap:
                    out.append(f'PART {remap[old]} {" ".join(t[2:])}')
                i += 1
                continue
            if t and t[0] == 'FRAME':
                header_done = True
                continue
            out.append(l)
            i += 1
            continue
        if t and t[0] == 'FRAME':
            out.append(l)
            i += 1
            continue
        if len(t) == 2 and t[0] == 'PART':
            old = int(t[1])
            # block = PART line + following non-empty lines up to the blank line
            j = i + 1
            while j < len(lines) and lines[j].strip() and lines[j].split()[0] not in ('PART', 'FRAME'):
                j += 1
            if old in remap:
                out.append(f'PART {remap[old]}')
                out.extend(lines[i + 1:j])
                # keep one blank separator
                out.append('')
            while j < len(lines) and not lines[j].strip():
                j += 1
            i = j
            continue
        if not l.strip():
            i += 1
            continue
        out.append(l)
        i += 1
    with open(dst_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    return [names[i] for i in keep_idx]
