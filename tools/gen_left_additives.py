"""Left-gun copies of right-gun additive xanims for BO3 dual-wield (akimbo) viewmodels (weapon_tech side:left lines).

The akimbo rig keeps the left gun as its own bones in the same skeleton (j_slide1, tag_pistol_offset1, tag_weapon_le ..),
listed in the left idle in the same order as the right idle lists the right gun's. Exports are world space (rows of the
X/Y/Z block are the bone's axes, checked on the decho akimbo fire anims: the slide's motion in tag_pistol_offset space is
identical on both sides). Each right part becomes its left twin; frame k = left idle pose (frame 0) * the right bone's
own-frame delta (right frame 0 ^-1 * right frame k). Frame 0 stays the reference (zero delta), as the linker's
type=additive expects.

Pose anims (2 frames: reference + pose) come out at 3 frames (the pose held one more frame): the BO3 linker drops a frame
of a 2-frame additive, which then loads with frequency 0 and never binds on a purpose slot (2026-10-02). The right-gun
sources listed in PAD3 are rewritten the same way in place (backup *.pre_akimboside.bak).

Usage: python gen_left_additives.py [--bo3 <game dir>] [--anim-dir <subdir>] [--out <dir>] [--install]
                                    [--job RIGHT_ADDITIVE RIGHT_IDLE LEFT_IDLE LEFT_OUT]...
  Anim names are relative to <game dir>/xanim_export/<anim-dir> (default anim-dir "tbd"), without extension, e.g.
  iw9/pi_decho/vm_p25_pi_decho_akimbo_r_idle. Needs the .xanim_bin files there. Each --job builds one left additive
  (repeat the flag); without --job the built-in example list JOBS below runs.
  --bo3      Black Ops III folder (default: the BO3_DIR environment variable)
  --out      where the generated xanim_export/xanim_bin files are written (default: ./work next to the cwd)
  --install  also copy the results into xanim_export, first backing up replaced files as *.pre_akimboside.bak
Requires numpy and PyCoD (set PYCOD_PATH to the folder containing the PyCoD package if it is not importable).
"""
import argparse, os, sys, shutil
import numpy as np
if os.environ.get("PYCOD_PATH"):
    sys.path.insert(0, os.environ["PYCOD_PATH"])
try:
    from PyCoD import xanim
except ImportError:  # --help still works; main() reports it
    xanim = None

XA = None      # <game dir>/xanim_export/<anim-dir>, set in main()
OUTDIR = os.path.join(os.getcwd(), "work")
JOBS = [
    # (right additive, right idle, left idle, out name)
    (r"iw9\pi_decho\vm_p25_pi_decho_akimbo_r_empty_additive", r"iw9\pi_decho\vm_p25_pi_decho_akimbo_r_idle",
     r"iw9\pi_decho\vm_p25_pi_decho_akimbo_l_idle", r"iw9\pi_decho\vm_p25_pi_decho_akimbo_l_empty_additive"),
    (r"iw9\pi_decho\vm_p25_pi_decho_bullet_additive", r"iw9\pi_decho\vm_p25_pi_decho_akimbo_r_idle",
     r"iw9\pi_decho\vm_p25_pi_decho_akimbo_l_idle", r"iw9\pi_decho\vm_p25_pi_decho_akimbo_l_bullet_additive"),
    (r"iw8\pistol_mike\vm_pi_mike_empty_additive", r"iw8\pistol_mike\vm_pi_mike_r_idle",
     r"iw8\pistol_mike\vm_pi_mike_l_idle", r"iw8\pistol_mike\vm_pi_mike_l_empty_additive"),
    # mike9 base / PaP are dual wield (pistol_mike9_kar_zm dualWield 1, _le_zm is the left gun), 2026-10-02
    (r"iw8\pistol_mike9\vm_pi_mike9_empty_additive", r"iw8\pistol_mike9\vm_pi_mike9_r_idle",
     r"iw8\pistol_mike9\vm_pi_mike9_l_idle", r"iw8\pistol_mike9\vm_pi_mike9_l_empty_additive"),
]


# right-gun pose additives used by slot:empty lines on the akimbo PaPs: rewritten at 3 frames (--install)
PAD3 = [r"iw9\pi_decho\vm_p25_pi_decho_akimbo_r_empty_additive", r"iw8\pistol_mike\vm_pi_mike_empty_additive",
        r"iw8\pistol_mike9\vm_pi_mike9_empty_additive"]


def pad3(anim):
    """2 frames (reference + pose) -> 3 (reference, pose, pose); anything longer is left alone"""
    if len(anim.frames) == 2:
        f = xanim.Frame(2)
        f.parts = [xanim.FramePart(tuple(p.offset), [tuple(r) for r in p.matrix], tuple(p.scale)) for p in anim.frames[1].parts]
        anim.frames.append(f)
    return anim


def load(rel):
    a = xanim.Anim()
    a.LoadFile_Bin(os.path.join(XA, rel + ".xanim_bin"))
    return a


def mat(fp):
    T = np.eye(4)
    T[:3, :3] = np.array(fp.matrix, float).T  # columns = axes
    T[:3, 3] = fp.offset
    return T


def part(T):
    R = T[:3, :3].T
    return xanim.FramePart(tuple(float(x) for x in T[:3, 3]), [tuple(float(x) for x in r) for r in R])


def twin_ok(r, l):
    """the left name the rig gives a right bone: <name>1, _ri -> _le, tag_weapon -> tag_weapon_le, .._right -> .._left"""
    cands = {r + "1", r.replace("_ri", "_le"), r.replace("_right", "_left"), r + "_le"}
    if r.startswith("tag_") and "_" in r:
        cands.add(r + "_le")
        cands.add(r.replace("tag_brass", "tag_brass_le").replace("tag_flash", "tag_flash_le"))
    return l in cands or r == l == "tag_torso" or (r == l and not r.endswith(("_ri", "_le")) and r in (
        "tag_cambone", "tag_camera", "tag_fill_light", "tag_flashlight", "tag_gasmask", "tag_torso"))


def build(job):
    rsrc, ridle, lidle, out = job
    a, ri, li = load(rsrc), load(ridle), load(lidle)
    rn, ln = [p.name for p in ri.parts], [p.name for p in li.parts]
    if len(rn) == len(ln):
        twin = dict(zip(rn, ln))
    else:
        # the left idle lacks some right-only tags (mike9: tag_ik_loc_le_foregrip, tag_ik_loc_ri): pair by name instead
        twin = {}
        for r in rn:
            hits = [l for l in ln if twin_ok(r, l)]
            if len(hits) > 1:
                hits = [h for h in hits if h != r]  # a renamed twin beats a shared name
            if len(hits) == 1:
                twin[r] = hits[0]
        print(f"{os.path.basename(ridle)} / {os.path.basename(lidle)}: {len(rn)} vs {len(ln)} parts, paired by name ({len(twin)})")
    lpose = {p.name: li.frames[0].parts[i] for i, p in enumerate(li.parts)}
    names = [p.name for p in a.parts]
    lnames = []
    for n in names:
        if n not in twin:
            raise SystemExit(f"{rsrc}: {n} is not in {ridle}")
        if not twin_ok(n, twin[n]):
            raise SystemExit(f"{rsrc}: {n} -> {twin[n]} doesn't look like its left twin")
        lnames.append(twin[n])
    o = xanim.Anim()
    o.framerate = a.framerate
    o.version = 3
    o.parts = [xanim.PartInfo(n) for n in lnames]
    o.notes = []
    moved = []
    for fi, f in enumerate(a.frames):
        nf = xanim.Frame(f.frame)
        nf.parts = []
        for pi, n in enumerate(names):
            R0, Rk = mat(a.frames[0].parts[pi]), mat(f.parts[pi])
            Dk = np.linalg.inv(R0) @ Rk
            L0 = mat(lpose[lnames[pi]])
            nf.parts.append(part(L0 @ Dk))
            if fi == len(a.frames) - 1:
                d = np.linalg.norm(Dk[:3, 3])
                ang = np.degrees(np.arccos(np.clip((np.trace(Dk[:3, :3]) - 1) / 2, -1, 1)))
                if d > 1e-3 or ang > 1e-2:
                    moved.append(f"{n} -> {lnames[pi]} {d:.3f} / {ang:.2f} deg")
        o.frames.append(nf)
    pad3(o)
    wd = OUTDIR
    os.makedirs(wd, exist_ok=True)
    base = os.path.join(wd, os.path.basename(out))
    o.WriteFile_Raw(base + ".xanim_export", header_message="// gen_left_additives.py: left twin of %s\n" % os.path.basename(rsrc))
    o.WriteFile_Bin(base + ".xanim_bin")
    chk = xanim.Anim()
    chk.LoadFile_Bin(base + ".xanim_bin")
    assert [p.name for p in chk.parts] == lnames and len(chk.frames) == len(o.frames)
    print(f"{os.path.basename(out)}: {len(lnames)} parts, {len(o.frames)} frames; moved at the last frame: {'; '.join(moved)}")
    return base, out


def main():
    global XA, OUTDIR, JOBS
    ap = argparse.ArgumentParser(description="Left-gun copies of right-gun additive xanims (see the module docstring).")
    ap.add_argument("--bo3", default=os.environ.get("BO3_DIR"), help="Black Ops III folder (default: BO3_DIR)")
    ap.add_argument("--anim-dir", default="tbd", help="subfolder of xanim_export holding the anims (default: tbd)")
    ap.add_argument("--out", default=OUTDIR, help="output folder (default: ./work)")
    ap.add_argument("--install", action="store_true", help="copy results into xanim_export (backs up *.pre_akimboside.bak)")
    ap.add_argument("--job", nargs=4, action="append", metavar=("RIGHT_ADD", "RIGHT_IDLE", "LEFT_IDLE", "LEFT_OUT"),
                    help="one left additive to build (repeatable); replaces the built-in JOBS list")
    ap.add_argument("--no-pad3", action="store_true", help="skip the in-place 3-frame rewrite of the PAD3 right-gun sources")
    opt = ap.parse_args()
    if not opt.bo3 or not os.path.isdir(opt.bo3):
        raise SystemExit("set the Black Ops III folder with --bo3 <dir> or the BO3_DIR environment variable")
    if xanim is None:
        raise SystemExit("PyCoD is not importable: install the PyCoD package (from its GitHub repository) or set PYCOD_PATH to the folder containing it")
    XA = os.path.join(opt.bo3, "xanim_export", opt.anim_dir)
    OUTDIR = os.path.abspath(opt.out)
    install = opt.install
    if opt.job:
        JOBS = [tuple(j) for j in opt.job]
        PAD3[:] = []
    if opt.no_pad3:
        PAD3[:] = []
    for rel in PAD3:
        a = load(rel)
        if len(a.frames) != 2:
            print(f"{os.path.basename(rel)}: {len(a.frames)} frames, left alone")
            continue
        a.version = 3
        a.notes = getattr(a, "notes", None) or []
        pad3(a)
        wd = OUTDIR
        os.makedirs(wd, exist_ok=True)
        base = os.path.join(wd, os.path.basename(rel))
        a.WriteFile_Raw(base + ".xanim_export", header_message="// gen_left_additives.py: pose held to 3 frames" + chr(10))
        a.WriteFile_Bin(base + ".xanim_bin")
        print(f"{os.path.basename(rel)}: 2 -> 3 frames")
        if install:
            for ext in (".xanim_export", ".xanim_bin"):
                dst = os.path.join(XA, rel + ext)
                if not os.path.exists(dst + ".pre_akimboside.bak"):
                    shutil.copy2(dst, dst + ".pre_akimboside.bak")
                shutil.copy2(base + ext, dst)
            print("  installed", rel)
    for job in JOBS:
        base, out = build(job)
        if install:
            for ext in (".xanim_export", ".xanim_bin"):
                dst = os.path.join(XA, out + ext)
                if os.path.exists(dst) and not os.path.exists(dst + ".pre_akimboside.bak"):
                    shutil.copy2(dst, dst + ".pre_akimboside.bak")
                shutil.copy2(base + ext, dst)
            print("  installed", out)


if __name__ == "__main__":
    main()
