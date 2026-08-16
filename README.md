# GEO-HUD

Windower 4 addon for Geomancer Luopan / Indi / Entrust tracking and ground rings for enmity/tagging.

![GEO-HUD 01](https://i.ibb.co/PzM7JXtk/GEO-HUD-01.jpg)

![GEO-HUD 02](https://i.ibb.co/7x1dj6Cs/GEO-HUD-02.jpg)

![GEO-HUD 03](https://i.ibb.co/V4DGVRS/GEO-HUD-03.jpg)


## Install

Copy the **GEO-HUD** folder into `Windower4/addons/` so you have:

```
Windower4/addons/GEO-HUD/GEO-HUD.lua
Windower4/addons/GEO-HUD/plugin/BCRings.dll
Windower4/addons/GEO-HUD/assets/...
```

That is the whole install. Do **not** put anything in `Windower4/plugins/` yourself.

Then in game:

```
//lua load GEO-HUD
```

Ground rings need a native Windower plugin. Windower can only load those from `plugins/`, so GEO-HUD copies `plugin/BCRings.dll` there the first time you load the addon, then runs `load BCRings`. Unloading GEO-HUD unloads the plugin too.

## Special Thanks

Special thanks to Broguypal for sharing his [TargetRing](https://github.com/Broguypal/Addons/tree/main/TargetRing) code.
