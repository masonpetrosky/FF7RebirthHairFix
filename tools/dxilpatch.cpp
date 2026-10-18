// SPDX-License-Identifier: MIT
// Offline test tool for the DXIL patcher.
//
//   dxilpatch <dll dir> <input dir> <output dir> [threshold] [dither strength]
//
// Patches every *.cso in <input dir> and writes patched shaders (and their disassembly) to <output dir>.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dxil_patcher.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: dxilpatch <dll dir> <input dir> <output dir> [threshold] [dither strength]\n");
    return 2;
  }
  vegfix::PatchOptions options;
  if (argc > 4) options.threshold = static_cast<float>(std::atof(argv[4]));
  if (argc > 5) options.dither_strength = static_cast<float>(std::atof(argv[5]));

  vegfix::DxilPatcher patcher;
  std::string error;
  if (!patcher.Initialize(argv[1], &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  const fs::path output_dir = argv[3];
  fs::create_directories(output_dir);
  int patched = 0, skipped = 0, failed = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& entry : fs::directory_iterator(argv[2])) {
    if (entry.path().extension() != ".cso") continue;
    std::ifstream file(entry.path(), std::ios::binary);
    const std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    vegfix::PatchStats stats;
    std::vector<uint8_t> out;
    const auto result = patcher.Patch(code.data(), code.size(), options, &out, &stats, &error);
    const std::string name = entry.path().stem().stem().string();
    switch (result) {
      case vegfix::PatchResult::kPatched: {
        ++patched;
        std::ofstream(output_dir / (name + ".cso"), std::ios::binary).write(reinterpret_cast<const char*>(out.data()), out.size());
        std::string text;
        if (patcher.Disassemble(out.data(), out.size(), &text, &error)) std::ofstream(output_dir / (name + ".ll")) << text;
        std::printf("patched  %s opacity=%d fade=%d other=%d\n", name.c_str(), stats.opacity_tests, stats.fade_tests, stats.other_tests);
        break;
      }
      case vegfix::PatchResult::kNotApplicable:
        ++skipped;
        if (stats.fade_tests || stats.other_tests)
          std::printf("skipped  %s fade=%d other=%d\n", name.c_str(), stats.fade_tests, stats.other_tests);
        break;
      case vegfix::PatchResult::kFailed:
        ++failed;
        std::printf("FAILED   %s: %s\n", name.c_str(), error.c_str());
        break;
    }
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  std::printf("%d patched, %d not applicable, %d failed in %lld ms\n", patched, skipped, failed, static_cast<long long>(ms));
  return failed ? 1 : 0;
}
