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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace vegfix {

namespace {

constexpr std::string_view kNoiseScale = "0x3FEFE00000000000";  // 255/256
constexpr std::string_view kNoiseBias = "1.953125e-03";         // 1/512
constexpr int kOpacityChannel = 0;
constexpr int kFadeChannel = 3;

struct Instruction {
  size_t line;
  std::string name;
  std::string rhs;  // right-hand side without the trailing comment
  size_t block = 0;
};

// A basic block of the entry function: "%0" for the entry block, "%148" for "; <label>:148".
struct Block {
  std::string name;
  size_t label_line;  // the label comment, or the "define" line for the entry block
  std::vector<std::string> preds;
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

bool ContainsName(const void* code, size_t size, std::string_view name) {
  // LLVM bitcode writes a string's characters as 8-bit fields starting at any bit, least significant bit
  // first. Starting at bit `shift` of byte q, a name of n characters fills the high bits of byte q, all of
  // bytes q+1 .. q+n-1 and, for a non-zero shift, the low bits of byte q+n.
  const auto* data = static_cast<const uint8_t*>(code);
  const auto* text = reinterpret_cast<const uint8_t*>(name.data());
  const size_t n = name.size();
  if (n < 2 || size <= n) return false;
  std::vector<uint8_t> middle(n - 1);
  for (int shift = 0; shift < 8; ++shift) {
    for (size_t k = 1; k < n; ++k)
      middle[k - 1] = static_cast<uint8_t>(text[k] << shift | (shift != 0 ? text[k - 1] >> (8 - shift) : 0));
    const int first = text[0] & (0xFF >> shift);
    const int last = shift != 0 ? text[n - 1] >> (8 - shift) : 0;
    const uint8_t* end = data + size - (shift != 0 ? 1 : 0);
    for (const uint8_t* p = data + 1; (p = std::search(p, end, middle.begin(), middle.end())) != end; ++p) {
      if ((p[-1] >> shift) == first && (shift == 0 || (p[n - 1] & ((1 << shift) - 1)) == last)) return true;
    }
  }
  return false;
}

namespace {

// The entry function of a disassembled shader: its instructions, value definitions and basic blocks.
struct ParsedFunction {
  std::vector<std::string> lines;
  std::vector<Instruction> instructions;
  std::unordered_map<std::string, size_t> defs;  // value name -> index into instructions
  std::unordered_map<std::string, int> use_counts;
  std::vector<Block> blocks;
  size_t end_line = 0;  // the closing "}"

  std::string_view DefRhs(std::string_view value) const {
    auto it = defs.find(std::string(value));
    return it == defs.end() ? std::string_view() : std::string_view(instructions[it->second].rhs);
  }
  int UseCount(std::string_view value) const {
    auto it = use_counts.find(std::string(value));
    return it == use_counts.end() ? 0 : it->second;
  }
  size_t BlockIndex(std::string_view name) const {
    for (size_t i = 0; i < blocks.size(); ++i)
      if (blocks[i].name == name) return i;
    return SIZE_MAX;
  }
};

ParsedFunction ParseFunction(const std::string& text) {
  ParsedFunction f;
  for (size_t start = 0; start < text.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    f.lines.emplace_back(text, start, end - start);
    start = end + 1;
  }

  bool in_function = false;
  for (size_t i = 0; i < f.lines.size(); ++i) {
    std::string_view line = f.lines[i];
    if (!in_function) {
      in_function = line.rfind("define ", 0) == 0;
      if (in_function) f.blocks.push_back({"%0", i, {}});
      continue;
    }
    if (line.rfind("}", 0) == 0) {
      f.end_line = i;
      break;
    }
    // "; <label>:148                                     ; preds = %141, %0"
    if (line.rfind("; <label>:", 0) == 0) {
      Block block;
      block.name = "%" + std::string(Trim(line.substr(10, line.find(' ', 10) - 10)));
      block.label_line = i;
      const size_t preds = line.find("; preds = ");
      if (preds != std::string_view::npos) {
        for (auto pred : SplitArgs(line.substr(preds + 10))) block.preds.emplace_back(pred);
      }
      f.blocks.push_back(std::move(block));
      continue;
    }
    std::string_view code = line.substr(0, line.find(';'));
    code = Trim(code);
    std::string_view rhs = code;
    std::string name;
    const size_t eq = code.find(" = ");
    if (!code.empty() && code[0] == '%' && eq != std::string_view::npos) {
      name = std::string(code.substr(0, eq));
      rhs = code.substr(eq + 3);
    }
    ForEachValueRef(rhs, [&](std::string_view ref) { ++f.use_counts[std::string(ref)]; });
    if (!name.empty()) {
      f.defs[name] = f.instructions.size();
      f.instructions.push_back({i, std::move(name), std::string(rhs), f.blocks.size() - 1});
    } else if (!rhs.empty()) {
      f.instructions.push_back({i, {}, std::string(rhs), f.blocks.size() - 1});
    }
  }
  return f;
}

// dominators[b][a]: block a dominates block b. Iterative data-flow over the block list.
std::vector<std::vector<bool>> Dominators(const ParsedFunction& f) {
  const size_t n = f.blocks.size();
  std::vector<std::vector<bool>> dom(n, std::vector<bool>(n, true));
  if (n == 0) return dom;
  dom[0].assign(n, false);
  dom[0][0] = true;
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t b = 1; b < n; ++b) {
      std::vector<bool> next(n, true);
      bool any_pred = false;
      for (const auto& pred : f.blocks[b].preds) {
        const size_t p = f.BlockIndex(pred);
        if (p == SIZE_MAX) continue;
        any_pred = true;
        for (size_t k = 0; k < n; ++k) next[k] = next[k] && dom[p][k];
      }
      if (!any_pred) next.assign(n, false);
      next[b] = true;
      if (next != dom[b]) {
        dom[b] = std::move(next);
        changed = true;
      }
    }
  }
  return dom;
}

// A dithered comparison: %m = fmul %noise_channel, 255/256 ; %t = fadd %m, 1/512 ; fcmp ... %t.
struct DitherTest {
  size_t m_index;  // into instructions
  size_t t_index;
  int channel;
  bool only_compares;  // %m and %t feed nothing but comparisons
};

std::vector<DitherTest> FindDitherTests(const ParsedFunction& f) {
  const auto is_masked_coord = [&](std::string_view value, std::string_view mask) {
    const std::string_view rhs = f.DefRhs(value);
    return rhs.rfind("and i32 ", 0) == 0 && rhs.size() > mask.size() && rhs.substr(rhs.size() - mask.size()) == mask;
  };

  // 1. Loads from the dither volume: Load(int3(x & 127, y & 127, z & 63 or z & 7)).
  std::unordered_map<std::string, bool> noise_loads;
  constexpr std::string_view kLoadPrefix = "call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(";
  for (const auto& inst : f.instructions) {
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
  if (noise_loads.empty()) return {};

  // 2. Channels extracted from those loads.
  std::unordered_map<std::string, int> noise_channels;
  constexpr std::string_view kExtractPrefix = "extractvalue %dx.types.ResRet.f32 ";
  for (const auto& inst : f.instructions) {
    if (inst.name.empty() || inst.rhs.rfind(kExtractPrefix, 0) != 0) continue;
    const auto args = SplitArgs(std::string_view(inst.rhs).substr(kExtractPrefix.size()));
    if (args.size() == 2 && noise_loads.count(std::string(args[0]))) noise_channels[inst.name] = std::atoi(std::string(args[1]).c_str());
  }

  // 3. Thresholds: %m = fmul %noise, 255/256 ; %t = fadd %m, 1/512.
  std::unordered_map<std::string, DitherTest> scaled;
  for (size_t i = 0; i < f.instructions.size(); ++i) {
    const auto& inst = f.instructions[i];
    std::string_view opcode, a, b;
    if (inst.name.empty() || !ParseBinary(inst.rhs, &opcode, &a, &b) || opcode != "fmul") continue;
    if (b == kNoiseScale && noise_channels.count(std::string(a))) scaled[inst.name] = {i, 0, noise_channels[std::string(a)], false};
    if (a == kNoiseScale && noise_channels.count(std::string(b))) scaled[inst.name] = {i, 0, noise_channels[std::string(b)], false};
  }

  std::vector<DitherTest> tests;
  for (size_t i = 0; i < f.instructions.size(); ++i) {
    const auto& inst = f.instructions[i];
    std::string_view opcode, a, b;
    if (inst.name.empty() || !ParseBinary(inst.rhs, &opcode, &a, &b) || opcode != "fadd") continue;
    std::string_view m_name;
    if (b == kNoiseBias && scaled.count(std::string(a))) m_name = a;
    if (a == kNoiseBias && scaled.count(std::string(b))) m_name = b;
    if (m_name.empty()) continue;
    DitherTest test = scaled[std::string(m_name)];
    test.t_index = i;
    test.only_compares = f.UseCount(m_name) == 1 && f.UseCount(inst.name) > 0;
    for (const auto& user : f.instructions) {
      if (!test.only_compares) break;
      bool uses = false;
      ForEachValueRef(user.rhs, [&](std::string_view ref) { uses |= (ref == inst.name); });
      if (uses && user.rhs.rfind("fcmp ", 0) != 0) test.only_compares = false;
    }
    tests.push_back(test);
  }
  return tests;
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) {
    out += line;
    out += '\n';
  }
  return out;
}

}  // namespace

bool PatchDisassembly(std::string& text, const PatchOptions& options, PatchStats* stats) {
  PatchStats local_stats;
  if (stats == nullptr) stats = &local_stats;
  if (text.find(kNoiseScale) == std::string::npos || text.find("@dx.op.discard") == std::string::npos) return false;

  ParsedFunction f = ParseFunction(text);
  const float strength = options.dither_strength;
  const std::string scale_literal = FloatLiteral(255.0f / 256.0f * strength);
  const std::string bias_literal = FloatLiteral(strength / 512.0f + options.threshold - strength * 0.5f);

  bool patched = false;
  for (const DitherTest& test : FindDitherTests(f)) {
    if (test.channel == kFadeChannel) {
      ++stats->fade_tests;
      continue;
    }
    // Only rewrite when both values are used exclusively for comparisons against the threshold.
    if (test.channel != kOpacityChannel || !test.only_compares) {
      ++stats->other_tests;
      continue;
    }
    std::string& m_line = f.lines[f.instructions[test.m_index].line];
    std::string& t_line = f.lines[f.instructions[test.t_index].line];
    if (ReplaceConstant(m_line, kNoiseScale, scale_literal) && ReplaceConstant(t_line, kNoiseBias, bias_literal)) {
      ++stats->opacity_tests;
      patched = true;
    }
  }

  if (!patched) return false;
  text = JoinLines(f.lines);
  return true;
}

namespace {

// Splits an instruction's right-hand side into words ("fcmp fast ogt float %a, %b" -> fcmp, fast, ogt, float, %a, %b).
std::vector<std::string_view> Words(std::string_view s) {
  std::vector<std::string_view> words;
  while (!s.empty()) {
    s = Trim(s);
    size_t end = s.find_first_of(" ,");
    if (end == std::string_view::npos) end = s.size();
    if (end > 0) words.push_back(s.substr(0, end));
    s.remove_prefix(std::min(end + 1, s.size()));
  }
  return words;
}

// The metadata node number of a "!123 = ..." line, or -1.
int MetadataId(std::string_view line) {
  if (line.size() < 2 || line[0] != '!' || !std::isdigit(static_cast<unsigned char>(line[1]))) return -1;
  return std::atoi(std::string(line.substr(1, line.find(' '))).c_str());
}

// Makes sure the signature element of a pixel shader input marks `component` as read: DXIL keeps the read
// components in the element's extended properties, {i32 3 (usage mask), i32 mask}. Returns false if the
// element's metadata has an unexpected shape.
bool MarkInputComponentUsed(std::vector<std::string>& lines, size_t element_line, int component) {
  std::string& element = lines[element_line];
  const size_t close = element.rfind('}');
  const size_t last_comma = element.rfind(", ", close);
  if (close == std::string::npos || last_comma == std::string::npos) return false;
  const std::string props = std::string(Trim(std::string_view(element).substr(last_comma + 2, close - last_comma - 2)));

  int mask = 0;
  if (props != "null") {
    if (props.size() < 2 || props[0] != '!') return false;
    const int id = std::atoi(props.c_str() + 1);
    bool found = false;
    for (const auto& line : lines) {
      if (MetadataId(line) != id) continue;
      // "!26 = !{i32 3, i32 7}"
      const size_t open = line.find("!{");
      if (open == std::string::npos) return false;
      const auto fields = SplitArgs(std::string_view(line).substr(open + 2, line.rfind('}') - open - 2));
      if (fields.size() != 2 || fields[0] != "i32 3" || fields[1].rfind("i32 ", 0) != 0) return false;
      mask = std::atoi(std::string(fields[1].substr(4)).c_str());
      found = true;
      break;
    }
    if (!found) return false;
  }
  if (mask & (1 << component)) return true;

  int max_id = 0;
  for (const auto& line : lines) max_id = std::max(max_id, MetadataId(line));
  const std::string node = "!" + std::to_string(max_id + 1);
  lines.push_back(node + " = !{i32 3, i32 " + std::to_string(mask | (1 << component)) + "}");
  element.replace(last_comma + 2, close - last_comma - 2, node);
  return true;
}

}  // namespace

bool MakeFringeDisassembly(std::string& text, const FringeOptions& options, std::string* reason) {
  std::string local_reason;
  if (reason == nullptr) reason = &local_reason;
  const auto fail = [reason](const char* why) {
    *reason = why;
    return false;
  };
  if (text.find(kNoiseScale) == std::string::npos || text.find("@dx.op.discard") == std::string::npos)
    return fail("no dithered discard");

  ParsedFunction f = ParseFunction(text);
  if (f.end_line == 0 || f.blocks.empty()) return fail("no entry function");

  // The one dithered opacity test, and the opacity it compares.
  const DitherTest* opacity = nullptr;
  const auto tests = FindDitherTests(f);
  for (const DitherTest& test : tests) {
    if (test.channel != kOpacityChannel) continue;
    if (opacity != nullptr || !test.only_compares) return fail("more than one opacity test, or an unusual one");
    opacity = &test;
  }
  if (opacity == nullptr) return fail("no opacity test");
  const std::string& t_name = f.instructions[opacity->t_index].name;
  std::string alpha;
  int compares = 0;
  for (const auto& inst : f.instructions) {
    if (inst.rhs.rfind("fcmp ", 0) != 0) continue;
    const auto words = Words(inst.rhs);
    if (words.size() < 4 || std::find(words.begin(), words.end(), std::string_view(t_name)) == words.end()) continue;
    ++compares;
    const std::string_view a = words[words.size() - 2], b = words[words.size() - 1];
    const std::string_view predicate = words[words.size() - 4];
    const bool greater = predicate == "ogt" || predicate == "oge" || predicate == "ugt" || predicate == "uge";
    const bool less = predicate == "olt" || predicate == "ole" || predicate == "ult" || predicate == "ule";
    if (b == t_name && greater) alpha = std::string(a);  // keep if opacity > threshold
    if (a == t_name && less) alpha = std::string(b);     // keep if threshold < opacity
  }
  if (compares == 1 && !alpha.empty() && alpha[0] != '%') return fail("constant opacity");  // opaque, nothing to blend
  if (compares != 1 || alpha.empty() || !f.defs.count(alpha)) return fail("unexpected opacity comparison");

  // The fringe keeps every fragment more opaque than fade_from (but no fully transparent ones).
  const float min_alpha = std::max(std::min(options.fade_from, options.core_threshold - 0.01f), 1.0f / 255.0f);
  if (!ReplaceConstant(f.lines[f.instructions[opacity->m_index].line], kNoiseScale, FloatLiteral(0.0f)) ||
      !ReplaceConstant(f.lines[f.instructions[opacity->t_index].line], kNoiseBias, FloatLiteral(min_alpha)))
    return fail("could not rewrite the threshold");

  // The discard, and the opacity as a value of its block.
  const Instruction* discard = nullptr;
  for (const auto& inst : f.instructions) {
    if (inst.rhs.rfind("call void @dx.op.discard(", 0) != 0) continue;
    if (discard != nullptr) return fail("more than one discard");
    discard = &inst;
  }
  if (discard == nullptr) return fail("no discard");
  const auto dom = Dominators(f);
  const size_t discard_block = discard->block;
  const size_t alpha_block = f.instructions[f.defs[alpha]].block;
  std::vector<std::string> inserted_phi;
  std::string alpha_here = alpha;
  if (!dom[discard_block][alpha_block]) {
    // The opacity is only computed on some paths into the discard's block (the others discard anyway).
    const Block& block = f.blocks[discard_block];
    std::string phi = "  %vf.alpha = phi float ";
    bool any = false;
    for (size_t i = 0; i < block.preds.size(); ++i) {
      const size_t p = f.BlockIndex(block.preds[i]);
      if (p == SIZE_MAX) return fail("unknown predecessor block");
      const bool has_alpha = dom[p][alpha_block];
      any |= has_alpha;
      phi += (i ? ", [ " : "[ ") + (has_alpha ? alpha : FloatLiteral(0.0f)) + ", " + block.preds[i] + " ]";
    }
    if (!any) return fail("opacity unavailable at the discard");
    inserted_phi.push_back(phi);
    alpha_here = "%vf.alpha";
  }

  // SV_Position is an input of every pixel shader; its z is the fragment's depth.
  size_t position_line = SIZE_MAX;
  int position_id = -1;
  for (size_t i = f.end_line; i < f.lines.size(); ++i) {
    const std::string& line = f.lines[i];
    const size_t at = line.find(", !\"SV_Position\", ");
    if (MetadataId(line) < 0 || at == std::string::npos) continue;
    const size_t open = line.find("!{i32 ");
    if (open == std::string::npos || open > at) continue;
    position_id = std::atoi(line.c_str() + open + 6);
    position_line = i;
  }
  if (position_line == SIZE_MAX || !MarkInputComponentUsed(f.lines, position_line, 2)) return fail("no usable SV_Position input");

  // SV_Target0..3 are the first outputs (scene colour, GBufferA, GBufferB, GBufferC = base colour): check the
  // signature table in the header comment.
  int targets = 0;
  for (size_t i = 0; i + 4 < f.lines.size() && i < f.blocks[0].label_line; ++i) {
    if (f.lines[i] != "; Output signature:") continue;
    for (int t = 0; t < 4 && i + 4 + t < f.lines.size(); ++t) {
      const std::string_view row = f.lines[i + 4 + t];  // after the blank, header and separator lines
      const auto words = Words(row.substr(std::min<size_t>(1, row.size())));
      if (words.size() >= 3 && words[0] == "SV_Target" && words[1] == std::to_string(t) && words[2] == "xyzw") ++targets;
    }
    break;
  }
  if (targets != 4) return fail("outputs are not SV_Target0..3");

  // The stores to SV_Target0..3. The fringe pass reads SV_Target3's base colour and writes four targets:
  //   SV_Target0  x: 1 for the solid core      y: depth of a fringe fragment (MAX-blended: the nearest)
  //               z: depth of a core fragment  w: fringe opacity (MAX-blended)
  //   SV_Target1  the core's base colour, w: 1 for the core (blended by w, so fringe fragments leave it)
  //   SV_Target2  fringe base colour times fringe opacity, w: fringe opacity (summed)
  //   SV_Target3  1 for a fringe fragment (the add-on blends its draw's ID into the target by it)
  struct Store {
    size_t line = SIZE_MAX;
    std::string value;
  };
  Store stores[4][4];
  constexpr std::string_view kStoreCall = "call void @dx.op.storeOutput.f32(";
  for (const auto& inst : f.instructions) {
    if (inst.rhs.rfind(kStoreCall, 0) != 0) continue;
    // i32 5, i32 <output>, i32 <row>, i8 <component>, float <value>
    const std::string_view inner = std::string_view(inst.rhs).substr(kStoreCall.size(), inst.rhs.rfind(')') - kStoreCall.size());
    const auto args = SplitArgs(inner);
    if (args.size() != 5 || args[1].rfind("i32 ", 0) != 0 || args[3].rfind("i8 ", 0) != 0 || args[4].rfind("float ", 0) != 0) continue;
    const int output = std::atoi(std::string(args[1].substr(4)).c_str());
    const int component = std::atoi(std::string(args[3].substr(3)).c_str());
    if (output < 0 || output > 3) continue;
    if (args[2] != "i32 0" || component < 0 || component > 3 || stores[output][component].line != SIZE_MAX)
      return fail("unexpected SV_Target store");
    if (!dom[inst.block][discard_block] || (inst.block == discard_block && inst.line < discard->line))
      return fail("SV_Target stored before the discard");
    stores[output][component] = {inst.line, std::string(args[4].substr(6))};
  }
  size_t first_store = SIZE_MAX;
  for (const auto& output : stores) {
    for (const auto& store : output) {
      if (store.line == SIZE_MAX) return fail("SV_Target0..3 not fully stored");
      first_store = std::min(first_store, store.line);
    }
  }
  // The base colour must be computed before the first of these stores, in a block that dominates it.
  size_t first_store_block = 0;
  for (const auto& inst : f.instructions)
    if (inst.line == first_store) first_store_block = inst.block;
  for (int c = 0; c < 3; ++c) {
    const std::string& value = stores[3][c].value;
    if (value.empty() || value[0] != '%') continue;  // a constant
    auto it = f.defs.find(value);
    if (it == f.defs.end()) return fail("unknown base colour value");
    const Instruction& def = f.instructions[it->second];
    if (def.line > first_store || !dom[first_store_block][def.block]) return fail("base colour computed after the stores");
  }
  const std::string albedo[3] = {stores[3][0].value, stores[3][1].value, stores[3][2].value};
  // An unlit material writes no base colour (its colour is emissive, in SV_Target0): its fringe would come out black.
  if (albedo[0] == albedo[1] && albedo[1] == albedo[2] && albedo[0] == FloatLiteral(0.0f)) return fail("unlit material");
  const std::string values[4][4] = {
      {"%vf.r", "%vf.g", "%vf.b", "%vf.w"},
      {albedo[0], albedo[1], albedo[2], "%vf.r"},
      {"%vf.ar", "%vf.ag", "%vf.ab", "%vf.w"},
      {"%vf.ff", "%vf.ff", "%vf.ff", "%vf.ff"},
  };
  for (int output = 0; output < 4; ++output) {
    for (int c = 0; c < 4; ++c) {
      std::string& line = f.lines[stores[output][c].line];
      const size_t value = line.find(", float ", line.find(kStoreCall));
      const size_t close = line.find(')', value);
      if (value == std::string::npos || close == std::string::npos) return fail("unexpected SV_Target store");
      line.replace(value + 8, close - value - 8, values[output][c]);
    }
  }

  // Fringe opacity: the texture's own opacity (what the game's dithering averages to), faded in over 0.1 from
  // `fade` so the faintest parts drop out, plus a steep knee (1 - core) * (opacity / core)^16 that only reaches the
  // core's edge, where it makes the fringe meet the solid core at full opacity.
  const float core = options.core_threshold;
  const float fade = std::min(options.fade_from, core - 0.01f);
  constexpr float kFadeWidth = 0.1f;
  const std::string a = alpha_here;
  std::vector<std::string> computed = {
      "  %vf.z = call float @dx.op.loadInput.f32(i32 4, i32 " + std::to_string(position_id) + ", i32 0, i8 2, i32 undef)",
      "  %vf.core = fcmp fast oge float " + a + ", " + FloatLiteral(core),
      "  %vf.r = select i1 %vf.core, float " + FloatLiteral(1.0f) + ", float " + FloatLiteral(0.0f),
      "  %vf.g = select i1 %vf.core, float " + FloatLiteral(0.0f) + ", float %vf.z",
      "  %vf.b = select i1 %vf.core, float %vf.z, float " + FloatLiteral(0.0f),
      "  %vf.m = fmul fast float " + a + ", " + FloatLiteral(1.0f / core),
      "  %vf.m2 = fmul fast float %vf.m, %vf.m",
      "  %vf.m4 = fmul fast float %vf.m2, %vf.m2",
      "  %vf.m8 = fmul fast float %vf.m4, %vf.m4",
      "  %vf.m16 = fmul fast float %vf.m8, %vf.m8",
      "  %vf.knee = fmul fast float %vf.m16, " + FloatLiteral(1.0f - core),
      "  %vf.base = fadd fast float " + a + ", %vf.knee",
      "  %vf.d = fadd fast float " + a + ", " + FloatLiteral(-fade),
      "  %vf.rm = fmul fast float %vf.d, " + FloatLiteral(1.0f / kFadeWidth),
      "  %vf.rover = fcmp fast ogt float %vf.rm, " + FloatLiteral(1.0f),
      "  %vf.r1 = select i1 %vf.rover, float " + FloatLiteral(1.0f) + ", float %vf.rm",
      "  %vf.runder = fcmp fast olt float %vf.r1, " + FloatLiteral(0.0f),
      "  %vf.ramp = select i1 %vf.runder, float " + FloatLiteral(0.0f) + ", float %vf.r1",
      "  %vf.c0 = fmul fast float %vf.base, %vf.ramp",
      "  %vf.over = fcmp fast ogt float %vf.c0, " + FloatLiteral(1.0f),
      "  %vf.c = select i1 %vf.over, float " + FloatLiteral(1.0f) + ", float %vf.c0",
      "  %vf.w = select i1 %vf.core, float " + FloatLiteral(0.0f) + ", float %vf.c",
      "  %vf.isf = fcmp fast ogt float %vf.w, " + FloatLiteral(0.0f),
      "  %vf.ff = select i1 %vf.isf, float " + FloatLiteral(1.0f) + ", float " + FloatLiteral(0.0f),
  };
  const std::vector<std::string> weighted = {
      "  %vf.ar = fmul fast float %vf.w, " + albedo[0],
      "  %vf.ag = fmul fast float %vf.w, " + albedo[1],
      "  %vf.ab = fmul fast float %vf.w, " + albedo[2],
  };

  // Insert: the phi after the block's own phis, the computation right after the discard, the weighted base colour
  // right before the first store.
  std::vector<std::string> out;
  out.reserve(f.lines.size() + computed.size() + weighted.size() + 2);
  const size_t phi_after = [&] {
    size_t line = f.blocks[discard_block].label_line;
    while (line + 1 < f.lines.size() && f.lines[line + 1].find(" = phi ") != std::string::npos) ++line;
    return line;
  }();
  for (size_t i = 0; i < f.lines.size(); ++i) {
    if (i == first_store) out.insert(out.end(), weighted.begin(), weighted.end());
    out.push_back(std::move(f.lines[i]));
    if (i == phi_after) out.insert(out.end(), inserted_phi.begin(), inserted_phi.end());
    if (i == discard->line) out.insert(out.end(), computed.begin(), computed.end());
  }
  text = JoinLines(out);
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

PatchResult DxilPatcher::MakeFringe(const void* code, size_t size, const FringeOptions& options, std::vector<uint8_t>* fringe,
                                    std::string* error) const {
  std::string text;
  if (!Disassemble(code, size, &text, error)) return PatchResult::kFailed;
  if (!MakeFringeDisassembly(text, options, error)) return PatchResult::kNotApplicable;
  if (!Assemble(text, fringe, error)) return PatchResult::kFailed;
  return PatchResult::kPatched;
}

}  // namespace vegfix
