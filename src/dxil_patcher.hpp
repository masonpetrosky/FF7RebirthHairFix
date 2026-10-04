// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hairfix {

// How the dithered opacity test is rewritten.
//   new_threshold = threshold + (noise - 0.5) * dither_strength
// dither_strength = 0 gives a plain alpha test (no screen-door noise at all);
// threshold = 0.5 with dither_strength = 1 reproduces the game's original behaviour.
struct PatchOptions {
  float threshold = 0.5f;
  float dither_strength = 0.0f;
};

struct PatchStats {
  int opacity_tests = 0;  // dithered opacity-mask comparisons that were rewritten
  int fade_tests = 0;     // dithered fade (LOD / camera fade) comparisons, left untouched
  int other_tests = 0;    // comparisons against other noise channels, left untouched
};

enum class PatchResult { kNotApplicable, kPatched, kFailed };

// Rewrites the disassembly of a pixel shader in place. Returns true if anything was patched.
bool PatchDisassembly(std::string& text, const PatchOptions& options, PatchStats* stats);

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

 private:
  using CreateInstanceProc = long(__stdcall*)(const void* clsid, const void* iid, void** object);

  void* compiler_module_ = nullptr;
  void* validator_module_ = nullptr;
  CreateInstanceProc create_compiler_ = nullptr;
  CreateInstanceProc create_validator_ = nullptr;
};

}  // namespace hairfix
