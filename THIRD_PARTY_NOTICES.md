# Third-party notices

FF7 Rebirth Vegetation and Hair Fix is MIT licensed (see `LICENSE`). It includes or redistributes the following
third-party components.

## ReShade add-on headers

`third_party/reshade/` — Copyright (C) 2014 Patrick Mours. BSD 3-Clause License, see
`third_party/reshade/LICENSE.md`. <https://github.com/crosire/reshade>

ReShade itself is not redistributed; users install it separately.

## Dear ImGui header

`third_party/imgui/` — Copyright (c) 2014-2026 Omar Cornut. MIT License, see
`third_party/imgui/LICENSE.txt`. <https://github.com/ocornut/imgui>

## DirectX Shader Compiler

- `third_party/dxc/DxilHash.{h,cpp}` (DXIL container hash) — Copyright (C) Microsoft Corporation.
  University of Illinois/NCSA Open Source License, see `third_party/dxc/LICENSE.TXT`.
- Release packages include `dxcompiler.dll` from the official
  [DirectX Shader Compiler v1.9.2609 release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609),
  distributed under the LLVM Release License (University of Illinois/NCSA) and the MIT License,
  included in the package as `FF7RebirthVegetationAndHairFix/licenses/dxcompiler-LICENSE-LLVM.txt` and
  `dxcompiler-LICENSE-MIT.txt`.

The DXIL validator (`dxil.dll`) is **not** redistributed. If a user places it next to
`dxcompiler.dll` themselves, the add-on uses it as an additional validation step.
