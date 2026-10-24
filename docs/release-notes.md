Removes the grainy, screen-door dithering on trees, grass and other foliage in FINAL FANTASY VII REBIRTH when using DLSS, DLAA or FSR, and gives hair and fur soft edges.

**New in 0.3.0:**
- **Soft edges for hair and fur** (on by default). The solid part of the hair is drawn cleanly, and the thin, see-through strand tips are blended over the scene, lit like the nearby solid hair, instead of being dithered or cut away. No grain, no chunky cutout. Overlapping layers cover as much as the strongest of them, like the game's own dithering, so hair keeps its depth. Settings: **Solid above** (the threshold) and **Edge softness**; `SoftEdgesKey` toggles them for comparison.
- New name: **FF7 Rebirth Vegetation and Hair Fix**, formerly FF7 Rebirth Vegetation Fix (and Hair Fix before that). The files are now `FF7RebirthVegetationAndHairFix.*`.

**Upgrading from Vegetation Fix 0.2.0 or Hair Fix 0.1.0:** extract the zip, delete the old `FF7RebirthVegetationFix.addon64` and `FF7RebirthVegetationFix` folder (or the `FF7RebirthHairFix` ones), and start the game once. Your settings carry over from the old ini; after that you can delete it too.

**Requires ReShade 6.x with add-on support.** Extract the zip into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`. See the README for details and settings.
