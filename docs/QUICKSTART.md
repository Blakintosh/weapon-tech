# weapon_tech quickstart

How to switch on each feature for one gun. Every key is in the [config reference](CONFIG_REFERENCE.md); this page is the
shortest path to a working gun.

## Install in 2 minutes

1. Unzip the release. In a command prompt in that folder run:

   ```
   kit\install.bat <mapname>
   ```

   `<mapname>` is your map's folder under `usermaps\`. The installer finds Black Ops III through Steam; if it can't, add
   `-GameDir "C:\...\Call of Duty Black Ops III"`. Add `-Bake` to also bake the cfg into the map (see
   [Shipping your map](../README.md#shipping-your-map)). Running it again changes nothing, and it never overwrites your
   `zone\weapon_tech.cfg` (it writes `weapon_tech.cfg.template` next to it instead).
2. It copies `weapon_tech.dll` and a starter `weapon_tech.cfg` into `usermaps\<map>\zone\`, copies the loader to
   `usermaps\<map>\ui\t7\utility\weapon_tech_loader.lua` and adds `rawfile,ui/t7/utility/weapon_tech_loader.lua` to
   `zone_source\<map>.zone`.
3. **Add one line by hand** at the top of `main()` in your map's client script (`scripts\zm\<map>.csc`):

   ```
   LuiLoad("ui.t7.utility.weapon_tech_loader");
   ```

4. Relink, run the map, and read `weapon_tech.log` next to `BlackOps3.exe`. The first line is
   `weapon_tech 0.1.1-alpha loaded from ...`. Later lines say which cfg it read and what it installed. Lines below marked
   **Log** are what to look for.

Without the installer, do the same four things by hand; the README's [Quick start](../README.md#quick-start) has the
steps.

## How the cfg works

The starter cfg has a `[features]` list. A feature that is **off installs nothing**, even with its lines in the file, so
a gun's lines only do something when its feature is on. Each feature has its own section. A gun does nothing until you
add it by its full variant name (for example `smg_charlie9_kar_zm`, case-sensitive). Comments go on their own line.

Saving the cfg while the game runs applies simple values live; anything that sets up hooks or animation slots needs a
game restart (the reference marks which). `[features]` is read at start.

After a typo, the log says `unknown key '...' (ignored; typo?)` or `bad line`. The `weapon_tech: features (...)` line is a
quick summary of what each feature ended up with, e.g. `additives(1 guns) inspect(1)`.

The gun names and anim names below are examples. Use your own.

---

## Additive layers (recoil, bullets, empty)

Plays an additive animation on top of the gun's normal animation: the empty-gun pose, a round-count animation, or a
recoil kick.

**You need**
- An additive xanim in your zone (`xanim,<name>` in the .zone file), one per layer.
- Pose animations (empty) need **3 frames**: frame 0 is the reference, then the pose twice. A 2-frame pose never shows.
- Bullets animations: one frame per round, frame 0 = full mag.
- The mag size for a `bullet` line.
- A gun has two classic roots, 193 and 195. Recoil must use 195; empty and bullet take whichever is left. For a third
  layer use the `slot:` form (see akimbo below).

**Minimal cfg**

```ini
[features]
additives = on

[additives]
additive=smg_charlie9_kar_zm,empty,193,vm_sm_charlie9_empty_additive,1.0
additive=smg_charlie9_kar_zm,recoil,195,vm_sm_charlie9_recoil_additive,1.0,1.2
```

The last number of a `recoil` line is its rate; for a `bullet` line it is the mag size
(`additive=<gun>,bullet,195,<xanim>,1.0,30`).

**Confirm** (Log)
- `additive: hooked after CG_UpdateViewWeaponAnim (2 layer(s))`
- `additive: smg_charlie9_kar_zm is variant 123`. If you see `no weapon variant named '...'` the name is wrong.

**Common mistakes**
- Weapon name that isn't the full variant name.
- A 2-frame pose: the log tells you; make it 3 frames.
- A comment at the end of an `additive=` line. Put comments on their own line.
- Editing the xanim path and expecting a live change: new or changed anims need a restart.

## Akimbo per-hand layers

Each dual-wield gun shows its own empty and bullet state.

**You need**
- A normal additive for the right gun, and a left twin that keys the **left** gun's bones (`j_slide1`, `j_bolt1`,
  `tag_weapon_le`, ...). `tools/gen_left_additives.py` makes one from the right gun's.
- The `slot:` form of `additive=`, so each side has its own layer.

**Minimal cfg**

```ini
[features]
additives = on

[additives]
additive=pistol_mike9_kar_zm,empty,slot:empty,vm_pi_mike9_empty_additive,1.0
additive=pistol_mike9_kar_zm,empty,slot:empty,vm_pi_mike9_l_empty_additive,1.0,side:left
```

For bullets use `bullet,slot:bullets,<xanim>,1.0,<mag size>` and add `,side:left` for the left gun. One line per gun,
purpose and side.

**Confirm** (Log)
- `slots: <gun>: empty bound: root ...` then `slots: <gun> slot:empty (<anim>) ON`.
- `slots: 2 slot line(s); purposes: ...`

**Common mistakes**
- A left anim that uses the right gun's bone names: nothing moves.
- `side:left` on a gun that isn't dual wield: stays off (logged).
- `side:both`: refused. Give each hand its own line.
- There is no left recoil layer.

## Ammo hide

Spent rounds disappear from see-through mags and belts as you fire, and come back on reload. On by default for any gun
with a `bullet` line.

**You need**
- A `bullet` additive line (above). Round joints are recognised by name: `bullet`, `round`, `shell`, `j_b_<n>`, `j_ammo*`.
- Optional: Gramien-style `gramien_hide_full_magazine` / `gramien_show_full_magazine` / `gramien_watch_ammo` notetracks on
  the reload anim for exact reload timing.

**Minimal cfg**

```ini
[features]
additives = on
ammo_hide = on

[additives]
additive=smg_charlie9_kar_zm,bullet,195,vm_sm_charlie9_bullets_additive,1.0,30
```

Nothing else is needed. For stack mags and belts where the order isn't obvious, run `tools/ammohide_order.py` on the cfg:
it writes the hide order into a generated block.

**Confirm** (Log)
- `additive: ammohide auto smg_charlie9_kar_zm: 30 round joint(s) (...)`
- `rg: viewmodel ...: N round joint(s) guarded` (the round guard that hides rounds the anim throws far away).

**Common mistakes**
- Joint names that don't match the patterns: nothing hides and the log says why.
- A mag material that is still a Gramien `_dyn` one: the feed motion doubles. Give it a normal material or skip it in
  the Gramien script.
- To switch it off for one gun: `auto_off = <gun>` in `[ammo_hide]`.

## IW8 kick and recoil patterns

MW2019 weapon-offset patterns (`wop_*`), view kick and kick springs.

**You need**
- The gun's data as `wop_*` lines. Don't write them by hand: `tools/iw8_wop_cfg.py <iw8 weapon json> <gun>` prints them.
- The fire time in ms.

**Minimal cfg** (example values from one gun)

```ini
[features]
kick = on

[kick]
wop_weapon=smg_charlie9_kar_zm,63
wop_kick=smg_charlie9_kar_zm,0,0,0,0,30,44,44,1
wop=smg_charlie9_kar_zm,1,0,2,1,0,-0.7,-0.04,0.05,2,0,1,1,0,-1
```

A PaP variant takes its base's block: `wop_alias=smg_charlie9_kar_upgraded_zm,smg_charlie9_kar_zm` (the source line has to
come first).

**Confirm** (Log)
- `additive: weapon offsets <gun> is variant N`
- `additive: weapon offsets: per-shot kick hooked`
- Warning `... fires every N ms but wop_weapon says M ms`: your fire time and the GDT disagree; the kick data was authored
  for `wop_weapon`.

**Common mistakes**
- Wrong `wop_weapon` fire time, so the kick rhythm is off.
- A comment at the end of any `wop*` line.
- `[features] kick = off`: no `wop*` block is read at all.

## Kick return

IW8's view-kick return: the view settles back after you stop firing.

**You need**
- At least one `wop_kick` line for the gun (the section above).

**Minimal cfg**

```ini
[features]
kick        = on
kick_return = on
```

Per gun, `wop_kickreturn=<gun>,0` in `[kick]` switches it off for that gun; put it before the gun's `wop_alias` lines.

**Confirm** (Log)
- `additive: kick return: hooked (clientActive caught at ...)`
- While firing and releasing: `additive: kick return <gun>: kick ... at release ...`

**Common mistakes**
- No `wop_kick` line: the log says `kick return off`.
- Turning it on in `[kick]` without `kick_return = on` in `[features]`.

## Camera shake and free camera

Scales the IW8 camera shake, and lets camera animations move only the camera.

**You need**
- A gun with WOP data (`[kick]`), since both only act while a WOP weapon is held.

**Minimal cfg**

```ini
[features]
camera = on

[camera]
shake = 1,1,1
cam_shake=smg_charlie9_kar_zm,0.5,0.35,0.5,1
free = 0
```

`shake` is the global pitch/yaw, roll and origin scale. A `cam_shake=` line sets one gun; `free = 1` is the IW8 camera
anim mode. Both are live.

**Confirm** (Log)
- No line of its own. The `weapon_tech: features (...)` line shows `camera(1)`, and edits show as `perf: cfg changed:
  applying live tuning`.

**Common mistakes**
- A global `cam_shake=` whose first value starts with a letter is read as a weapon name.
- `camera_free` on a gun with no WOP data: nothing happens.

## idle_active, locomotion and sway

MW2019's looping hip idle, walk and jog loops, and advanced sway.

**You need**
- An idle_active additive anim for the gun (leaf 194), and/or a walk loop from the MW2019 data.
- Walk loops cover several strides: say how many.
- For sway, the `sway_*` data lines; `tools/iw8_wop_cfg.py <json> <gun> --sway` prints them (they are long).

**Minimal cfg**

```ini
[features]
locomotion = on

[locomotion]
locomotion=smg_charlie9_kar_zm,walk,bob,4
idle_active=smg_charlie9_kar_zm,vm_sm_charlie9_idle_active_additive,194
```

**Confirm** (Log)
- `locomotion: 1 weapon(s): 1 walk, 0 jog, 1 idle_active; ...`
- `locomotion: holding <gun> (variant N): walk ..., idle_active ...` when you pick the gun up.
- With sway lines: `sway: 1 weapon(s): ...` and `sway: holding <gun> ...`.

**Common mistakes**
- Wrong stride count: the feet slide against the bob.
- `idle_active` on a root the gun's `additive=` line already uses: refused, the log says so. Use 194 or 196.
- A leaf that holds no anim of its own: `jog off` / `idle_active leaf ... holds no anim`.

## Inspect and empty inspect

Press a key to inspect the gun. With an empty clip it plays a separate empty inspect.

**You need**
- The normal inspect anim in the GDT's `lowReadyLoopAnim`, optionally `lowReadyLoopTime`.
- For the empty inspect, an anim in `lowReadyInAnim` (and `lowReadyInTime`).
- No script calling `SetLowReady` on the same gun.
- Optional: akimbo guns work (the inspect anim animates both arms).

**Minimal cfg**

```ini
[features]
inspect = on

[inspect]
guns = smg_charlie9_kar_zm
```

Optional keys in `[inspect]`: `key = I` (a letter, F1..F24, MOUSE3..5), `hidehud = 1`, `empty = in | out | off`.
`gun:4.8` sets the length in seconds; `gun:off` disables it.

**Confirm** (Log)
- `inspect: hooked before CG_UpdateViewWeaponAnim; 1 of 1 weapon line(s) on, key 0x49, ...`
- `inspect: <gun> (variant N): lowReadyLoopAnim '<anim>' ...`. If it ends `NOT USED: the slot is blank or the idle
  anim`, the GDT slot is empty.

**Common mistakes**
- `lowReadyLoopAnim` blank, or the same as the idle anim.
- Expecting live changes: inspect is read once at start.
- No `[features] inspect = on`.

## Last shot

Decides when the empty pose comes in after the last round.

**You need**
- An `empty` additive layer (see Additive layers). Nothing if you leave it on `auto`.

**Minimal cfg**

```ini
[features]
last_shot = auto

[last_shot]
guns = smg_psierra41_kar_zm:hold
```

`auto` plays a real fire-last animation then locks empty with no pop, and locks empty straight away on a gun without
one. Per gun, `hold` forces the first behaviour and `iw` the second (MW2019).

**Confirm** (Log)
- `additive: empty_lastshot: default auto, 1 weapon line(s)`
- `empty_lastshot: variant N node ... : hold` (or `iw`) once per gun.

**Common mistakes**
- Forcing `hold` on a gun whose last-shot anim is just its fire anim: the empty pose pops in late. Use `iw`.
- Expecting it without an `empty` layer.

## Empty melee

An empty gun keeps its own melee and normal raise and drop, and (optionally) rapid melee works on an empty gun.

**You need**
- Nothing: it is a code patch, global to the map.

**Minimal cfg**

```ini
[features]
empty_melee = on

[empty_melee]
interrupt_empty_melee = 1
```

**Confirm** (Log)
- `additive: empty melee: gun keeps its own melee patched` (plus the empty raise and drop lines).
- `interrupt: interrupt_empty_melee=1: the empty gun's post-melee raise (state 32) stays interruptible by melee`.

**Common mistakes**
- Expecting a per-gun switch: these patches apply to every gun.
- `empty_melee = off` in `[features]` drops both the empty-melee fix and `interrupt_empty_melee`.

## Interrupts *(experimental)*

Raises, reloads and rechambers can end early when the player fires, aims, sprints, melees, reloads or switches.

**You need**
- The cut-off point: an `interruptible` or `state_timer_end` notetrack on the anim, or a time or fraction.

**Minimal cfg**

```ini
[features]
interrupts = on

[interrupts]
interrupt=smg_charlie9_kar_zm,raises+reload,notetrack|0.85
```

States are joined with `+`, the source chain with `|` (first that resolves wins). An optional fourth field limits the
actions, e.g. `...,fire+ads`.

**Confirm** (Log)
- `interrupt: hooked both PM_Weapon calls; 1 weapon line(s), ...`
- `interrupt: <gun> (variant N) <state>: node ..., ... ms -> ...`. Add `interrupt_debug = 1` to log every cut.

**Common mistakes**
- Commas inside the states field: use `+`.
- Expecting it live: interrupts are read once at start.
- Little in-game testing so far: keep it off unless you want to try it.

## Segmented reloads

Empty variants for shell-by-shell reloads, MW2019 (rechamber at the end) or MWII (empty start).

**You need**
- A segmented-reload gun with `reloadEmptyAnim` and `reloadEmptyTime` set in the GDT. For `start`, also
  `reloadEmptyAddTime`.
- `end` (MW2019): `reloadEndAnim` is the plain end, `reloadEmptyAnim` the rechamber end.

**Minimal cfg**

```ini
[features]
segreload = on

[segreload]
guns = shotgun_romeo870_kar_zm:end
```

`start` is the MWII behaviour, `off` stops a PaP inheriting its base's mode.

**Confirm** (Log)
- `segreload: hooked 8 call sites (...); 1 weapon line(s), ...`
- `segreload: <gun> (variant N) mode end | reloadAnim '...' ...`

**Common mistakes**
- No `reloadEmptyAnim` / `reloadEmptyTime` in the GDT.
- A PaP that should keep stock behaviour: give it `:off`.
- Not restarting after a change.

## MW slide

MW2019 slide movement with the slide gesture animation. **It applies to every player on the whole map.**

**You need**
- Nothing for the movement. For the gesture, three animations (in, loop, out; frame 0 = the gun's idle as the additive
  reference) in the juke slots under additive root 184, by default leaves 187, 188 and 191. A gun without them just
  has no gesture.

**Minimal cfg**

```ini
[features]
slide = on
```

Tuning goes in `[slide]` (`suit`, `gesture`, `camera`, `view`, ...); most of it is live.

**Confirm** (Log)
- `slide: MW2019 slide installed: N call sites hooked; ...`

**Common mistakes**
- Forgetting it is map-wide.
- Gesture anims in the wrong leaves: set `gesture_nodes = in,loop,out` to match.
- Turning it on live: `slide` is read at start.

## Viewmodel FOV pin

Keeps the gun the same size on screen whatever the player's FOV. **Map-wide.**

**You need**
- Nothing.

**Minimal cfg**

```ini
[features]
vmfov = mw
```

`mw` is MW2019's default; a number such as `vmfov = 65` pins the gun at that FOV. Tune in `[vmfov]`
(`ads = fade | hold`, `depth`, `max`). Those are live; the hook itself installs only if `vmfov` is on at start.

**Confirm** (Log)
- `vmfov: hooked (...); ...`. With `debug = 1` in `[vmfov]` a once-a-second line shows the world FOV and shift.

**Common mistakes**
- Switching it on live in a game that started with it off: the log says `restart the game`.
- Expecting it to change your guns per weapon: it is map-wide.

## Hand IK *(experimental)*

Keeps the hands on the gun while non-rigid additives move it. It has seen little in-game testing.

**You need**
- IK notetracks on the anims: `ik_in_start_<hand>` / `ik_in_end_<hand>` / `ik_out_start_<hand>` / `ik_out_end_<hand>`
  (`<hand>` is `l` or `r`), or an `ik_notes=` line that supplies them.

**Minimal cfg**

```ini
[features]
ik = on

[ik]
guns = ar_valpha_kar_zm:lr
```

The hands are `l`, `r` or `lr`.

**Confirm** (Log)
- `ik: hooked; ik_enable 1, 1 weapon line(s), 0 ik_notes line(s)`. With `debug = 1` in `[ik]` it logs the measurements.

**Common mistakes**
- Leaving `ik = off`: the lines do nothing.
- An anim with no IK notetracks and no `ik_notes=` line.
- `only one call site hooked: IK OFF`: the exe didn't match; check the exe build in the log.
