# GEO-HUD

Windower 4 addon for Geomancer Luopan / Indi / Entrust tracking and ground rings for enmity/tagging.

![GEO-HUD 01](https://i.ibb.co/PzM7JXtk/GEO-HUD-01.jpg)

![GEO-HUD 02](https://i.ibb.co/7x1dj6Cs/GEO-HUD-02.jpg)

![GEO-HUD 03](https://i.ibb.co/V4DGVRS/GEO-HUD-03.jpg)


## Install

Copy the **GEO-HUD** folder into `Windower4/addons/` so you have:

```
Windower4/addons/GEO-HUD/GEO-HUD.lua
Windower4/addons/GEO-HUD/libs/_GEORings25.dll
Windower4/addons/GEO-HUD/assets/...
```

Then in game:

```
//lua load GEO-HUD
```

Ground rings are drawn by `libs/_GEORings25.dll`, loaded from the addon folder. Nothing goes in `Windower4/plugins/`.

If you previously used GEO-HUD 1.4, this version unloads `BCRings` and deletes `Windower4/plugins/BCRings.dll` on load so the old plugin cannot double-draw. You can also remove `load BCRings` from `init.txt` if it is still there.

GEO-HUD and TargetRing can run together. GEO-HUD finds `draw_scene` even after TargetRing has jumped it, becomes the outer hook, then calls TargetRing and draws. If TargetRing is unloaded, GEO-HUD keeps the original scene trampoline so the game does not crash.

## Special Thanks

Special thanks to Broguypal for sharing his [TargetRing](https://github.com/Broguypal/Addons/tree/main/TargetRing) code.



<<<<<<< HEAD
### v1.9.1
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
