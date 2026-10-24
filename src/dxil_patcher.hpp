// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vegfix {

// How the dithered opacity test is rewritten.
//   new_threshold = threshold + (noise - 0.5) * dither_strength
// dither_strength = 0 gives a plain alpha test (no screen-door noise at all);
// threshold = 0.5 with dither_strength = 1 reproduces the game's original behaviour.
struct PatchOptions {
  float threshold = 0.5f;
  float dither_strength = 0.0f;

  bool operator==(const PatchOptions&) const = default;
  // The game's own test, which a patch would leave unchanged.
  bool original() const { return threshold == 0.5f && dither_strength == 1.0f; }
};

// True if a DXIL container holds `name` as a bitcode string, such as a resource name in its reflection
// data (the STAT part).
bool ContainsName(const void* code, size_t size, std::string_view name);

struct PatchStats {
  int opacity_tests = 0;  // dithered opacity-mask comparisons that were rewritten
  int fade_tests = 0;     // dithered fade (LOD / camera fade) comparisons, left untouched
  int other_tests = 0;    // comparisons against other noise channels, left untouched
};

enum class PatchResult { kNotApplicable, kPatched, kFailed };

// Rewrites the disassembly of a pixel shader in place. Returns true if anything was patched.
bool PatchDisassembly(std::string& text, const PatchOptions& options, PatchStats* stats);

// Soft edges: the game's passes draw the solid core of a dithered material as a plain alpha test at
// `core_threshold`, and a fringe pass draws the same geometry once more with a variant of its G-buffer pixel
// shader. That variant keeps every fragment more opaque than `fade_from` (and the game's dithered fade) and writes
// four targets (their blend states are in the add-on):
//   SV_Target0  x = 1 for a core fragment      y = depth of a fringe fragment
//               z = depth of a core fragment   w = fringe opacity: the texture's opacity, faded in from fade_from
//                                                and meeting the core at 1 just below core_threshold
//   SV_Target1  the core's base colour, w = 1 for a core fragment
//   SV_Target2  fringe base colour times fringe opacity, w = fringe opacity
//   SV_Target3  1 for a fringe fragment
struct FringeOptions {
  float core_threshold = 0.5f;
  float fade_from = 0.25f;

  bool operator==(const FringeOptions&) const = default;
};

// Turns the disassembly of a dithered G-buffer pixel shader into its fringe variant. Returns false (with the
// reason) if the shader does not have the expected shape.
bool MakeFringeDisassembly(std::string& text, const FringeOptions& options, std::string* reason);

// Disassembles, patches, reassembles and signs DXIL using dxcompiler.dll. If dxil.dll is present
// next to it, every patched shader is additionally checked by the official validator.
class DxilPatcher {
 public:
  DxilPatcher() = default;
  DxilPatcher(const DxilPatcher&) = delete;
  DxilPatcher& operator=(const DxilPatcher&) = delete;
  ~DxilPatcher();

  // Loads dxcompiler.dll (required) and dxil.dll (optional) from the given directory.
  bool Initialize(const std::filesystem::path& dll_directory, std::string* error);
  bool initialized() const { return create_compiler_ != nullptr; }
  bool validating() const { return create_validator_ != nullptr; }

  bool Disassemble(const void* code, size_t size, std::string* text, std::string* error) const;
  bool Assemble(const std::string& text, std::vector<uint8_t>* code, std::string* error) const;

  PatchResult Patch(const void* code, size_t size, const PatchOptions& options, std::vector<uint8_t>* patched,
                    PatchStats* stats, std::string* error) const;
  // The fringe variant of a dithered G-buffer pixel shader (see FringeOptions).
  PatchResult MakeFringe(const void* code, size_t size, const FringeOptions& options, std::vector<uint8_t>* fringe,
                         std::string* error) const;

 private:
  using CreateInstanceProc = long(__stdcall*)(const void* clsid, const void* iid, void** object);

  void* compiler_module_ = nullptr;
  void* validator_module_ = nullptr;
  CreateInstanceProc create_compiler_ = nullptr;
  CreateInstanceProc create_validator_ = nullptr;
};

}  // namespace vegfix
