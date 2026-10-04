# FF7 Rebirth Hair Fix

A [ReShade](https://reshade.me) add-on for **FINAL FANTASY VII REBIRTH** (PC) that removes the
grainy, screen-door dithering on hair (and grass, fur and foliage) when playing with
**DLSS, DLAA or FSR**.

![Sephiroth's hair and nearby grass at 4K with DLSS Quality: original on the left, with the fix on the right](docs/comparison.png)

## Why hair looks grainy

Rebirth draws hair cards, fur and foliage as *dithered* masked materials. Instead of blending
semi-transparent pixels, every pixel is either kept or discarded by comparing its opacity against
an animated blue-noise pattern (UE's `View_SpatiotemporalBlueNoiseVolumeTexture`):

```
keep pixel if  opacity > noise.x * 255/256 + 1/512
```

The game's own TAA averages that noise over several frames into smooth translucency. DLSS and FSR
treat much of it as real detail and keep it, so hair ends up looking like a grainy screen door.

## What the add-on does

As the game creates its pipelines, the add-on looks for that exact comparison in every pixel
shader (disassembling the DXIL with Microsoft's `dxcompiler.dll`) and rewrites only the two
constants that build the dithered threshold:

```
keep pixel if  opacity > threshold + (noise - 0.5) * dither_strength
```

With the default `dither_strength = 0`, it becomes a plain alpha test that DLSS/FSR can
anti-alias cleanly. Everything else stays as the game shipped it, including the separate
dither used for LOD transitions and for fading out characters close to the camera.

Because it matches the pattern rather than specific shader hashes, it covers every hair, fur and
foliage material automatically, including ones a future game patch adds. Work happens on a
background thread and patched shaders are cached on disk, so later launches skip the analysis.

## Installation

1. Install **ReShade 6.x with add-on support** (the "with full add-on support" download from
   [reshade.me](https://reshade.me)). Point it at
   `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64\ff7rebirth_.exe` and choose **DirectX 12**.
   You don't need to pick any effect shaders.
2. Download the latest `FF7RebirthHairFix-v*.zip` from
   [Releases](../../releases) and extract it into the same `Win64` folder, so you end up with:
   ```
   Win64\ff7rebirth_.exe
   Win64\dxgi.dll                      (ReShade)
   Win64\FF7RebirthHairFix.addon64
   Win64\FF7RebirthHairFix.ini
   Win64\FF7RebirthHairFix\dxcompiler.dll
   ```
3. Start the game. Patching begins a few seconds after the title screen appears.

## Settings

Open the ReShade overlay (**Home**), go to the **Add-ons** tab and expand **FF7 Rebirth Hair Fix**.
Changes apply live and are saved to `FF7RebirthHairFix.ini`.

| Setting | Default | What it does |
| --- | --- | --- |
| Remove hair dithering | on | Switch between patched and original shaders instantly, for comparison. |
| Opacity threshold | 0.33 | Hair-card pixels more opaque than this are drawn. Lower = fuller hair with chunkier strand edges, higher = finer strands but thinner, more see-through hair. |
| Dither strength | 0.00 | 0 = clean alpha test. Values around 0.2–0.3 bring back a little softness at strand edges. 1 = the game's original dithering. |

To bind a toggle hotkey, set `ToggleKey` in the ini to a
[virtual-key code](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)
(for example `119` for F8).

## Notes

- It applies to every material that uses this dithering, so grass, foliage and fur are cleaned up too.
- First launch: dithered pipelines are patched in the background over the first seconds of play,
  so hair can look dithered briefly. Patched shaders are cached in
  `Win64\FF7RebirthHairFix\cache`. It's safe to delete that folder.
- Tested with game build 23447986 (Steam, October 2026), ReShade 6.8.0 and DLSS 310.3 on an
  RTX 5070 Ti. Compatibility with other ReShade add-ons such as RenoDX hasn't been tested.

## Uninstall

Delete `FF7RebirthHairFix.addon64`, `FF7RebirthHairFix.ini` and the `FF7RebirthHairFix` folder
from `Win64`. To remove ReShade too, delete `dxgi.dll`, `ReShade.ini` and `ReShade.log`.

## Building

Requires Visual Studio 2022 or newer with the C++ x64 tools (CMake and Ninja are included).

```powershell
.\build.ps1      # build\FF7RebirthHairFix.addon64 and build\dxilpatch.exe
.\package.ps1    # dist\FF7RebirthHairFix-v<version>.zip (downloads the pinned DXC release)
```

`dxilpatch.exe <dxc dir> <input dir> <output dir> [threshold] [dither strength]` runs the shader
patch over a folder of dumped `.cso` files. That's useful for checking the pattern still matches after a
game update. Shaders can be dumped from the add-on's **Developer** section.

## License

MIT; see [LICENSE](LICENSE). Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). FINAL FANTASY VII REBIRTH is a trademark of
Square Enix; this project is not affiliated with or endorsed by Square Enix.
