// SPDX-License-Identifier: MIT
//
// FINAL FANTASY VII REBIRTH implements masked materials (hair cards, foliage, fur...) with a
// dithered opacity test. Every such pixel shader reads a 128x128x64 blue-noise volume
//
//   noise = DitherVolume.Load(int3(SvPosition.x & 127, SvPosition.y & 127, (FrameIndex & 7 + Offset) & 63))
//
// (materials without a per-object offset, e.g. bindless foliage, use just "FrameIndex & 7")
//
// and compares against thresholds built from it:
//
//   fade    > noise.w * 255/256 + 1/512   (LOD / camera dither fade, left alone)
//   opacity > noise.x * 255/256 + 1/512   (opacity mask, rewritten)
//
// The game's TAA was tuned to average that noise away; DLSS/FSR mostly keep it, which is the
// screen-door look on hair. In DXIL this is always the instruction pair
//
//   %m = fmul fast float %noise_x, 0x3FEFE00000000000
//   %t = fadd fast float %m, 1.953125e-03
//
// so the patch only rewrites those two constants:
//
//   %m = fmul fast float %noise_x, (255/256 * strength)
//   %t = fadd fast float %m, (strength/512 + threshold - strength/2)
//
// which turns the opacity test into "opacity > threshold + (noise - 0.5) * strength".

#include "dxil_patcher.hpp"

#define NOMINMAX
#include <windows.h>  // not WIN32_LEAN_AND_MEAN: dxcapi.h needs the COM/OLE declarations

#include <dxcapi.h>
#include <wrl/client.h>

#include "DxilHash.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace hairfix {

namespace {

constexpr std::string_view kNoiseScale = "0x3FEFE00000000000";  // 255/256
constexpr std::string_view kNoiseBias = "1.953125e-03";         // 1/512
constexpr int kOpacityChannel = 0;
constexpr int kFadeChannel = 3;

struct Instruction {
  size_t line;
  std::string name;
  std::string rhs;  // right-hand side without the trailing comment
};

bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '$'; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> SplitArgs(std::string_view s) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t comma = s.find(", ", start);
    out.push_back(Trim(s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
    if (comma == std::string_view::npos) break;
    start = comma + 2;
  }
  return out;
}

std::string_view LastWord(std::string_view s) {
  s = Trim(s);
  const size_t space = s.rfind(' ');
  return space == std::string_view::npos ? s : s.substr(space + 1);
}

// Parses "<opcode> [flags...] <type> <a>, <b>" (binary ops and fcmp, where the predicate counts as a flag).
bool ParseBinary(std::string_view rhs, std::string_view* opcode, std::string_view* a, std::string_view* b) {
  static constexpr std::string_view kTypes[] = {"float", "half", "double", "i32", "i16", "i64", "i1"};
  const size_t first_space = rhs.find(' ');
  if (first_space == std::string_view::npos) return false;
  *opcode = rhs.substr(0, first_space);
  std::string_view rest = rhs.substr(first_space + 1);
  while (!rest.empty()) {
    const size_t space = rest.find(' ');
    if (space == std::string_view::npos) return false;
    const std::string_view word = rest.substr(0, space);
    rest = rest.substr(space + 1);
    bool is_type = false;
    for (auto type : kTypes) is_type |= (word == type);
    if (is_type) break;
  }
  const auto args = SplitArgs(rest);
  if (args.size() < 2) return false;
  *a = args[0];
  *b = args[1];
  return true;
}

// Calls fn(name) for every %value referenced in an instruction's right-hand side.
template <typename Fn>
void ForEachValueRef(std::string_view rhs, Fn&& fn) {
  for (size_t i = 0; i < rhs.size(); ++i) {
    if (rhs[i] != '%') continue;
    size_t end = i + 1;
    while (end < rhs.size() && IsIdentChar(rhs[end])) ++end;
    const std::string_view name = rhs.substr(i, end - i);
    // Skip type names such as %dx.types.Handle.
    if (name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1]))) fn(name);
    i = end - 1;
  }
}

// Replaces the first occurrence of a whole constant token in a line.
bool ReplaceConstant(std::string& line, std::string_view from, const std::string& to) {
  size_t pos = 0;
  while ((pos = line.find(from, pos)) != std::string::npos) {
    const bool start_ok = pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == ',';
    const size_t end = pos + from.size();
    const bool end_ok = end == line.size() || line[end] == ',' || line[end] == ' ';
    if (start_ok && end_ok) {
      line.replace(pos, from.size(), to);
      return true;
    }
    pos = end;
  }
  return false;
}

// LLVM IR spells float constants as the hex of the equivalent double.
std::string FloatLiteral(float value) {
  if (value == 0.0f) return "0.000000e+00";
  const double d = static_cast<double>(value);
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof(bits));
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(bits));
  return buf;
}

std::string BlobToString(IDxcBlob* blob) {
  if (blob == nullptr || blob->GetBufferSize() == 0) return {};
  const char* data = static_cast<const char*>(blob->GetBufferPointer());
  size_t size = blob->GetBufferSize();
  while (size > 0 && data[size - 1] == '\0') --size;
  return std::string(data, size);
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

// DXIL container header: "DXBC", a 16-byte digest, then the hashed remainder of the container.
constexpr size_t kDigestOffset = 4;
constexpr size_t kHashedOffset = 20;

void SignContainer(std::vector<uint8_t>& container) {
  ComputeHashRetail(container.data() + kHashedOffset, static_cast<UINT32>(container.size() - kHashedOffset),
                    container.data() + kDigestOffset);
}

std::string ResultErrors(IDxcOperationResult* result) {
  ComPtr<IDxcBlobEncoding> errors;
  if (FAILED(result->GetErrorBuffer(&errors))) return "unknown error";
  return BlobToString(errors.Get());
}

}  // namespace

bool PatchDisassembly(std::string& text, const PatchOptions& options, PatchStats* stats) {
  PatchStats local_stats;
  if (stats == nullptr) stats = &local_stats;
  if (text.find(kNoiseScale) == std::string::npos || text.find("@dx.op.discard") == std::string::npos) return false;

  std::vector<std::string> lines;
  for (size_t start = 0; start < text.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    lines.emplace_back(text, start, end - start);
    start = end + 1;
  }

  // Collect the instructions of the entry function.
  std::vector<Instruction> instructions;
  std::unordered_map<std::string, size_t> defs;  // value name -> index into instructions
  std::unordered_map<std::string, int> use_counts;
  bool in_function = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string_view line = lines[i];
    if (!in_function) {
      in_function = line.rfind("define ", 0) == 0;
      continue;
    }
    if (line.rfind("}", 0) == 0) break;
    std::string_view code = line.substr(0, line.find(';'));
    code = Trim(code);
    std::string_view rhs = code;
    std::string name;
    const size_t eq = code.find(" = ");
    if (!code.empty() && code[0] == '%' && eq != std::string_view::npos) {
      name = std::string(code.substr(0, eq));
      rhs = code.substr(eq + 3);
    }
    ForEachValueRef(rhs, [&](std::string_view ref) { ++use_counts[std::string(ref)]; });
    if (!name.empty()) {
      defs[name] = instructions.size();
      instructions.push_back({i, std::move(name), std::string(rhs)});
    } else if (!rhs.empty()) {
      instructions.push_back({i, {}, std::string(rhs)});
    }
  }

  auto def_rhs = [&](std::string_view value) -> std::string_view {
    auto it = defs.find(std::string(value));
    return it == defs.end() ? std::string_view() : std::string_view(instructions[it->second].rhs);
  };
  auto is_masked_coord = [&](std::string_view value, std::string_view mask) {
    const std::string_view rhs = def_rhs(value);
    return rhs.rfind("and i32 ", 0) == 0 && rhs.size() > mask.size() && rhs.substr(rhs.size() - mask.size()) == mask;
  };

  // 1. Loads from the dither volume: Load(int3(x & 127, y & 127, z & 63 or z & 7)).
  std::unordered_map<std::string, bool> noise_loads;
  constexpr std::string_view kLoadPrefix = "call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(";
  for (const auto& inst : instructions) {
    if (inst.name.empty() || inst.rhs.rfind(kLoadPrefix, 0) != 0) continue;
    std::string_view args_text = inst.rhs;
    args_text.remove_prefix(kLoadPrefix.size());
    args_text = args_text.substr(0, args_text.rfind(')'));
    const auto args = SplitArgs(args_text);
    if (args.size() < 6) continue;
    if (is_masked_coord(LastWord(args[3]), ", 127") && is_masked_coord(LastWord(args[4]), ", 127") &&
        (is_masked_coord(LastWord(args[5]), ", 63") || is_masked_coord(LastWord(args[5]), ", 7"))) {
      noise_loads[inst.name] = true;
    }
  }
  if (noise_loads.empty()) return false;

  // 2. Channels extracted from those loads.
  std::unordered_map<std::string, int> noise_channels;
  constexpr std::string_view kExtractPrefix = "extractvalue %dx.types.ResRet.f32 ";
  for (const auto& inst : instructions) {
    if (inst.name.empty() || inst.rhs.rfind(kExtractPrefix, 0) != 0) continue;
    const auto args = SplitArgs(std::string_view(inst.rhs).substr(kExtractPrefix.size()));
    if (args.size() == 2 && noise_loads.count(std::string(args[0]))) noise_channels[inst.name] = std::atoi(std::string(args[1]).c_str());
  }

  // 3. Thresholds: %m = fmul %noise, 255/256 ; %t = fadd %m, 1/512.
  struct Scaled {
    size_t index;
    int channel;
  };
  std::unordered_map<std::string, Scaled> scaled;
  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& inst = instructions[i];
    std::string_view opcode, a, b;
    if (inst.name.empty() || !ParseBinary(inst.rhs, &opcode, &a, &b) || opcode != "fmul") continue;
    if (b == kNoiseScale && noise_channels.count(std::string(a))) scaled[inst.name] = {i, noise_channels[std::string(a)]};
    if (a == kNoiseScale && noise_channels.count(std::string(b))) scaled[inst.name] = {i, noise_channels[std::string(b)]};
  }

  const float strength = options.dither_strength;
  const std::string scale_literal = FloatLiteral(255.0f / 256.0f * strength);
  const std::string bias_literal = FloatLiteral(strength / 512.0f + options.threshold - strength * 0.5f);

  bool patched = false;
  for (const auto& inst : instructions) {
    std::string_view opcode, a, b;
    if (inst.name.empty() || !ParseBinary(inst.rhs, &opcode, &a, &b) || opcode != "fadd") continue;
    std::string_view m_name;
    if (b == kNoiseBias && scaled.count(std::string(a))) m_name = a;
    if (a == kNoiseBias && scaled.count(std::string(b))) m_name = b;
    if (m_name.empty()) continue;
    const Scaled& m = scaled[std::string(m_name)];

    if (m.channel == kFadeChannel) {
      ++stats->fade_tests;
      continue;
    }
    if (m.channel != kOpacityChannel) {
      ++stats->other_tests;
      continue;
    }

    // Only rewrite when both values are used exclusively for comparisons against the threshold.
    bool only_compares = use_counts[std::string(m_name)] == 1 && use_counts[inst.name] > 0;
    for (const auto& user : instructions) {
      if (!only_compares) break;
      bool uses = false;
      ForEachValueRef(user.rhs, [&](std::string_view ref) { uses |= (ref == inst.name); });
      if (uses && user.rhs.rfind("fcmp ", 0) != 0) only_compares = false;
    }
    if (!only_compares) {
      ++stats->other_tests;
      continue;
    }

    std::string& m_line = lines[instructions[m.index].line];
    std::string& t_line = lines[inst.line];
    if (ReplaceConstant(m_line, kNoiseScale, scale_literal) && ReplaceConstant(t_line, kNoiseBias, bias_literal)) {
      ++stats->opacity_tests;
      patched = true;
    }
  }

  if (!patched) return false;
  std::string out;
  out.reserve(text.size() + 64);
  for (const auto& line : lines) {
    out += line;
    out += '\n';
  }
  text = std::move(out);
  return true;
}

DxilPatcher::~DxilPatcher() {
  if (validator_module_ != nullptr) FreeLibrary(static_cast<HMODULE>(validator_module_));
  if (compiler_module_ != nullptr) FreeLibrary(static_cast<HMODULE>(compiler_module_));
}

bool DxilPatcher::Initialize(const std::filesystem::path& dll_directory, std::string* error) {
  const auto compiler_path = dll_directory / L"dxcompiler.dll";
  const auto validator_path = dll_directory / L"dxil.dll";
  compiler_module_ = LoadLibraryExW(compiler_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (compiler_module_ == nullptr) {
    if (error) *error = "could not load " + compiler_path.string();
    return false;
  }
  create_compiler_ = reinterpret_cast<CreateInstanceProc>(GetProcAddress(static_cast<HMODULE>(compiler_module_), "DxcCreateInstance"));
  if (create_compiler_ == nullptr) {
    if (error) *error = "DxcCreateInstance export not found in dxcompiler.dll";
    return false;
  }
  // The validator is optional: without it, containers are signed with the open-source DXIL hash.
  if (FileExists(validator_path)) {
    validator_module_ = LoadLibraryExW(validator_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (validator_module_ != nullptr)
      create_validator_ = reinterpret_cast<CreateInstanceProc>(GetProcAddress(static_cast<HMODULE>(validator_module_), "DxcCreateInstance"));
  }
  return true;
}

bool DxilPatcher::Disassemble(const void* code, size_t size, std::string* text, std::string* error) const {
  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcCompiler> compiler;
  if (FAILED(create_compiler_(&CLSID_DxcUtils, &__uuidof(IDxcUtils), &utils)) ||
      FAILED(create_compiler_(&CLSID_DxcCompiler, &__uuidof(IDxcCompiler), &compiler))) {
    if (error) *error = "DxcCreateInstance failed";
    return false;
  }
  ComPtr<IDxcBlobEncoding> source;
  ComPtr<IDxcBlobEncoding> disassembly;
  if (FAILED(utils->CreateBlobFromPinned(code, static_cast<UINT32>(size), DXC_CP_ACP, &source)) ||
      FAILED(compiler->Disassemble(source.Get(), &disassembly))) {
    if (error) *error = "disassembly failed";
    return false;
  }
  *text = BlobToString(disassembly.Get());
  return true;
}

bool DxilPatcher::Assemble(const std::string& text, std::vector<uint8_t>* code, std::string* error) const {
  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcAssembler> assembler;
  ComPtr<IDxcValidator> validator;
  if (FAILED(create_compiler_(&CLSID_DxcUtils, &__uuidof(IDxcUtils), &utils)) ||
      FAILED(create_compiler_(&CLSID_DxcAssembler, &__uuidof(IDxcAssembler), &assembler)) ||
      (create_validator_ != nullptr && FAILED(create_validator_(&CLSID_DxcValidator, &__uuidof(IDxcValidator), &validator)))) {
    if (error) *error = "DxcCreateInstance failed";
    return false;
  }

  ComPtr<IDxcBlobEncoding> source;
  if (FAILED(utils->CreateBlobFromPinned(text.data(), static_cast<UINT32>(text.size()), DXC_CP_UTF8, &source))) {
    if (error) *error = "could not create source blob";
    return false;
  }
  ComPtr<IDxcOperationResult> assembled;
  HRESULT status = E_FAIL;
  if (FAILED(assembler->AssembleToContainer(source.Get(), &assembled)) || FAILED(assembled->GetStatus(&status)) || FAILED(status)) {
    if (error) *error = "assembly failed: " + (assembled ? ResultErrors(assembled.Get()) : std::string());
    return false;
  }
  ComPtr<IDxcBlob> container;
  if (FAILED(assembled->GetResult(&container)) || container == nullptr) {
    if (error) *error = "assembler returned no container";
    return false;
  }

  // D3D12 only accepts DXIL whose container digest matches its contents.
  const auto* bytes = static_cast<const uint8_t*>(container->GetBufferPointer());
  code->assign(bytes, bytes + container->GetBufferSize());
  if (code->size() <= kHashedOffset) {
    if (error) *error = "assembler returned a truncated container";
    return false;
  }
  SignContainer(*code);
  if (validator == nullptr) return true;

  // With dxil.dll available, also run full validation and check that it signs identically.
  std::vector<uint8_t> validated_code(code->begin(), code->end());
  std::fill(validated_code.begin() + kDigestOffset, validated_code.begin() + kHashedOffset, uint8_t{0});
  ComPtr<IDxcBlobEncoding> signable;
  ComPtr<IDxcOperationResult> validated;
  if (FAILED(utils->CreateBlobFromPinned(validated_code.data(), static_cast<UINT32>(validated_code.size()), DXC_CP_ACP, &signable)) ||
      FAILED(validator->Validate(signable.Get(), DxcValidatorFlags_InPlaceEdit, &validated)) || FAILED(validated->GetStatus(&status)) ||
      FAILED(status)) {
    if (error) *error = "validation failed: " + (validated ? ResultErrors(validated.Get()) : std::string());
    return false;
  }
  if (validated_code != *code) {
    if (error) *error = "DXIL hash differs from the validator's";
    return false;
  }
  return true;
}

PatchResult DxilPatcher::Patch(const void* code, size_t size, const PatchOptions& options, std::vector<uint8_t>* patched,
                               PatchStats* stats, std::string* error) const {
  std::string text;
  if (!Disassemble(code, size, &text, error)) return PatchResult::kFailed;
  if (!PatchDisassembly(text, options, stats)) return PatchResult::kNotApplicable;
  if (!Assemble(text, patched, error)) return PatchResult::kFailed;
  return PatchResult::kPatched;
}

}  // namespace hairfix
