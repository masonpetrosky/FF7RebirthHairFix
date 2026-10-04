// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hairfix {

// Standard CRC-32 (zlib polynomial), the same hash RenoDX uses to name shaders,
// so hashes printed by this add-on can be cross-referenced with RenoDX dumps.
inline uint32_t Crc32(const void* data, size_t size) {
  static const std::array<uint32_t, 256> kTable = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return table;
  }();

  uint32_t crc = 0xFFFFFFFFu;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) crc = kTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace hairfix
