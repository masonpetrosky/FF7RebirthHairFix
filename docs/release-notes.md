Removes the grainy, screen-door dithering on trees, grass and other foliage in FINAL FANTASY VII REBIRTH when using DLSS, DLAA or FSR. Hair can be cleaned up as well.

**New in 0.2.0:**
- New name: **FF7 Rebirth Vegetation Fix**, formerly FF7 Rebirth Hair Fix.
- Characters (hair, fur, clothing) and scenery (trees, plants, grass and everything else) have separate settings. By default scenery gets the clean alpha test and characters keep the game's soft dithering, which DLSS preset L resolved well on hair in our tests. If hair looks grainy (with FSR or other DLSS presets), set Characters to 0.33 / 0.00.
- Fixed: on a PC with two GPUs, a Direct3D device that another mod opens and closes on the other GPU (Shader Injector does this at startup) no longer resets the fix.

**Upgrading from Hair Fix 0.1.0:** extract the zip, delete `FF7RebirthHairFix.addon64` and the `FF7RebirthHairFix` folder, and start the game once. Your settings carry over from `FF7RebirthHairFix.ini` to both groups, so nothing changes until you change it. After that you can delete `FF7RebirthHairFix.ini` too.

**Requires ReShade 6.x with add-on support.** Extract the zip into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`. See the README for details and settings.
