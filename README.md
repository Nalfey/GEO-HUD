# GEO-HUD

Windower 4 addon for Geomancer Luopan / Indi / Entrust tracking and ground rings for enmity/tagging.

![GEO-HUD 01](https://i.ibb.co/PzM7JXtk/GEO-HUD-01.jpg)

![GEO-HUD 02](https://i.ibb.co/7x1dj6Cs/GEO-HUD-02.jpg)

![GEO-HUD 03](https://i.ibb.co/V4DGVRS/GEO-HUD-03.jpg)


## Install

Copy the **GEO-HUD** folder into `Windower4/addons/` so you have:

```
Windower4/addons/GEO-HUD/GEO-HUD.lua
Windower4/addons/GEO-HUD/libs/_GEORings46.dll
Windower4/addons/GEO-HUD/assets/...
```

Then in game:

```
//lua load GEO-HUD
```

Ground rings are drawn by `libs/_GEORings46.dll`, loaded from the addon folder. Nothing goes in `Windower4/plugins/`.

If you previously used GEO-HUD 1.4, this version unloads `BCRings` and deletes `Windower4/plugins/BCRings.dll` on load so the old plugin cannot double-draw. You can also remove `load BCRings` from `init.txt` if it is still there.

GEO-HUD and TargetRing can run together, in any load order, and either one can be unloaded or reloaded at any time. Neither addon hooks the other any more: both draw through a shared scene hook that owns the single `draw_scene` patch for the whole process. Use TargetRing 3.0.0 or later with this build. See [SceneHook/SceneHook.md](SceneHook/SceneHook.md) if you want the details, or if you are writing an addon that needs to draw in the 3D scene yourself.

If you still have GEO-HUD 1.9.x or TargetRing 2.x loaded in this client session, restart FFXI once. Those builds patch `draw_scene` themselves, and SceneHook will not find the original prologue until the process is fresh.

## Entrust duration

The ENTRUST line on **your** HUD uses a timer for Indi spells you cast on someone else. That is what hides the bubble on a Trust: party buff packets do not reliably drop Colure Active on NPCs.

This is **not** something other people in the party configure. They cannot see your midcast set. If they run GEO-HUD while watching you, they still use Colure Active on players and will not get a reliable Trust timer.

The formula matches [BG Wiki Indicolure](https://www.bg-wiki.com/ffxi/Category:Geomancy#Indicolure) and the Compendium example (~346s with Pants +3, Gaiters +3, 40s JP, Gada 8% + Lifestream 20%):

```
(180 + flat seconds from worn gear + job points) × (1 + percent from worn augments / 100)
```

Job points are **+2s each, 20 cap = 40s**, added *before* the percent multiply.

GEO-HUD does **not** talk to GearSwap. It follows outgoing equip packets (single-slot **0x050** and GearSwap’s bulk **0x051**), looks those items up in your bags, and keeps the **best** duration it sees. The countdown starts when the spell **lands**. It does **not** trust Windower’s “currently equipped” table at land time — that is often still Fast Cast or aftercast idle.

The only manual value is Indicolure Duration job points in seconds (default **40**). Lower it only if yours is not maxed:

```
//geohud entrustjp 40
//geohud entrustdur          — show last gear read and current JP
```

If no duration gear is seen during the cast (very short Fast Cast, inventory not ready), the timer is **180 + JP** so the bubble still goes away instead of hanging. That fallback is about **80s too short** vs a typical midcast set — if the HUD dies early, run `//geohud entrustdur` right after the cast; `0 flat, 0%` means the snapshot missed midcast.

What it looks for while you cast:

| Piece | Slot | How it counts |
| --- | --- | --- |
| Bagua Pants / +1 / +2 / +3 / +4 | Legs | +12 / +15 / +18 / +21 seconds (+4 same as +3) |
| Azimuth Gaiters / +1 / +2 / +3 | Feet | +15 / +20 / +25 / +30 seconds |
| Nantosuelta's Cape | Back | +20 seconds |
| Solstice | Club | +15 seconds |
| Lifestream Cape (augment) | Back | +10–20% from the cape's actual augment |
| Gada (augment) | Club | +1–11% from the club's actual augment |

Back is one cape. Club is Solstice or Gada, not both — whichever is actually on. Pants left in storage or a locked weapon that never becomes Gada are not counted.

## Special Thanks

Special thanks to Broguypal for sharing his [TargetRing](https://github.com/Broguypal/Addons/tree/main/TargetRing) code.



### v2.0.2
* Range rings: `//geohud rangerings on` draws a thin element-coloured circle on the floor at the edge of each luopan, Indi, and Entrust bubble. Separate from the green/red tag rings.

* Debuff Geo- on a mob now tags that target. Casting the bubble is enough enmity for potency on that mob; other enemies in range still need a separate hate action.

* In Maquette Abdhaljs-Legion B (Ambuscade, zone 287) every enemy counts as tagged, because the party is already on the hate list.

### v2.0.1
* Entrust on Trusts expires from a duration taken from the gear you wear while casting. Other party members still use Colure Active. `//geohud entrustjp` sets job points only (default 40). Midcast is sampled on equip swaps so Fast Cast no longer falls back to 180+JP. See [Entrust duration](#entrust-duration).

* Colorblind mode brightens the Cardinal Chant N/S/E/W letters and draws a dark outline so they read more clearly.

* Ground rings hide when a mob leaves the luopan, and all rings clear when the luopan is gone. Tagged hate is still remembered for the next bubble.

### v2.0.0
* Rewritten hook handling. GEO-HUD and TargetRing no longer patch or chain into each other; both register with a shared scene hook. Load order, unload order and reloading no longer matter, and neither addon can crash the other.

* The ring DLL is now `_GEORings36.dll`. There is deliberately no fallback to `_GEORings35.dll`, which patches `draw_scene` on its own and would fight the bus. Update TargetRing to 3.0.0 at the same time if you use it.

### v1.9.5
* Do not free the draw_scene trampoline if another addon chained on top of GEO-HUD, so unloading GEO-HUD after TargetRing no longer crashes.

### v1.9.4
* Accept D3D devices from wrappers such as dgVoodoo: if GetDevice returns an object outside d3d8.dll, keep it when Release/EndScene/GetViewport/DrawPrimitiveUP look like real code. Renderer scan skips unreadable slots and looks 0x8000 bytes instead of 0x2000.

### v1.9.3
* Cardinal Chant ring shows N/S/E/W letters at the world compass points.

* Performance: ring list and Cardinal Chant updates throttled to 10 Hz in `prerender`; trampoline page freed on DLL unload; D3D device COM ref released on scan failure.

### v1.9.1
* Cardinal Chant is main GEO only, off by default on other jobs, and the circle hides in towns.

### v1.9.0
* Cardinal Chant circle: `//geohud cardinalchant on` draws a ring under you that follows the target's direction. North = blue magic crit, east = red magic attack, south = green magic accuracy, west = yellow magic burst. In-between bearings blend the two nearest colours.

* Per-character and per-job settings files so HUD, orbs, rings, and the mob list can differ by character and job.

* Colorblind mode: `//geohud colorblind on` draws six high-contrast O marks on green rings and six X marks on red rings.

* Buff Geo- bubbles draw green rings on party members in range. Debuff Geo- bubbles still ring enemies.

* Coexist with TargetRing: chain `draw_scene` hooks instead of overwriting each other.

### v1.5.2
* Increased fluidity and performance of drawn rings. 

### v1.5.0
* Fix for the ground rings after Windower LuaCore 2.6.8.4 update.

### v1.4.0
* Added commands for HUD customisation.

### v1.2.0
* First release.
