# weapon_tech tools

Python helpers for authoring `weapon_tech.dll` content: they generate cfg lines and additive anim files.
All paths come from arguments or environment variables; nothing is hard-coded.

| Script | What it does | Inputs | Outputs | Needs |
| --- | --- | --- | --- | --- |
| `ammohide_order.py` | Derives `ammohide_order=` / `ammohide_spend=` lines (which spent round hides when) from each bullet anim. Recognises round joints named `bullet`, `round`, `shell`, `j_b_N` and BO7/IW9 `j_ammo_*`. | A `weapon_tech.cfg` with `additive=<weapon>,bullet,...` lines; the anims' `.xanim_export` files under `<BO3_DIR>/xanim_export` | Prints the section, or `--write` replaces the block between the BEGIN/END markers in the cfg (backup `<cfg>.pre_hideorder.bak`) | numpy, `xanimlib.py` |
| `cfg_migrate.py` | Converts a flat `weapon_tech.cfg` to the sectioned layout (format v2): `[features]` from the old switches, every line in its feature's section with its comments, simple per-gun lines collapsed into guns lists, generator blocks kept whole. `--check` proves the DLL parses old and new to the same state. | A flat cfg | `<cfg>.v2`, or `--in-place` (backup `<cfg>.pre_cfgv2.bak`, only when the check passes) | Python only; `wt_cfgdump.exe` for `--check` (`build.ps1` builds it; or `--dump-tool` / `WT_CFGDUMP`) |
| `gen_left_additives.py` | Makes left-gun twins of right-gun additive anims (bullet, empty) for akimbo / dual-wield viewmodels (`side:left` slots). Also rewrites 2-frame pose anims to 3 frames, since the BO3 linker drops a frame of a 2-frame additive. | `.xanim_bin` files under `<BO3_DIR>/xanim_export/<anim-dir>`: right additive, right idle, left idle. Jobs via `--job`, or edit `JOBS` | `.xanim_export` + `.xanim_bin` in `./work` (`--out`); `--install` copies them into xanim_export (backup `*.pre_akimboside.bak`) | numpy, PyCoD |
| `iw8_wop_cfg.py` | Converts a MW2019 (IW8) weapon def dump into `wop_*` cfg lines (weapon offset patterns, kick), `sway_*` lines (`--sway`) and phase-1 GDT key/values (`--gdt`). | An `iw8_*.json` weapon dump, the BO3 weapon name, optional fire time ms, `--att` attachment dumps, `--idle` advanced-idle json (or `$IW8_ADVIDLE`, or `iw8_advidle.json` beside the script) | Text on stdout to paste into the cfg / GDT | Python only |
| `xanimlib.py` | Library: reads and writes `xanim_export` text (world-space parts, one block per part per frame). Used by `ammohide_order.py`. | | | numpy |

## Requirements

- Python 3.9 or newer.
- numpy: `pip install numpy`.
- PyCoD (only for `gen_left_additives.py`): the Python CoD xanim/xmodel library used by the Maya/Blender CoD tools.
  Install it from its GitHub repository, or point `PYCOD_PATH` at the folder that contains the `PyCoD` package.
- `BO3_DIR` (or `--bo3 <dir>`): your Black Ops III folder, e.g. `set BO3_DIR=C:\Games\Call of Duty Black Ops III`.

## Examples

    python ammohide_order.py path\to\weapon_tech.cfg --bo3 "%BO3_DIR%"
    python ammohide_order.py path\to\weapon_tech.cfg --write
    python cfg_migrate.py path\to\weapon_tech.cfg --in-place --check --dump-tool ..\build\wt_cfgdump.exe
    python gen_left_additives.py --job iw9/pi_x/r_empty_additive iw9/pi_x/r_idle iw9/pi_x/l_idle iw9/pi_x/l_empty_additive
    python iw8_wop_cfg.py iw8_ar_mcharlie.json ar_mcharlie_zm --sway

## Typical workflow

Convert the source weapon anims (MW2019, MW2022, BO7 and so on) to `.xanim_export` / `.xanim_bin` in your BO3
`xanim_export` folder, with the bullet, empty and idle anims kept as additives of the same skeleton the gun uses.
Run `iw8_wop_cfg.py` for the weapon's offset/sway lines and `gen_left_additives.py` for any left-gun twins, then
add the matching `additive=` lines to `weapon_tech.cfg`. Once the `additive=<weapon>,bullet,...` lines exist, run
`ammohide_order.py` (print first, then `--write`) so each gun's spent rounds hide in the right order; rerun it
whenever bullet lines change. Paste the other output into the cfg by hand, link the map so the `.xanim_bin` files
are packed (new anims need a rebuild of the zone), and test in game.
