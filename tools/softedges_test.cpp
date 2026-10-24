// SPDX-License-Identifier: MIT
// Offline test of the soft-edge composite (src/soft_edges.*) with the D3D12 debug layer, on a synthetic frame:
//
//   softedges_test
//
// A dark background, a bright vertical stripe of "core" hair (base colour 0.5, lit 1.0: lighting 2), a fringe to its
// right with base colour 0.25 whose transmittance rises from 0 to 1 (so its colour is 0.5), a fringe of another draw
// at the core's depth (like eyebrow hairs, it has no core of its own) and a fringe far in front of any core (both must
// stay untouched). Prints the debug layer's messages and checks the blended values.

#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "soft_edges.hpp"

namespace {

constexpr uint32_t kWidth = 256, kHeight = 128;
constexpr float kBackground = 0.2f, kCore = 1.0f, kCoreAlbedo = 0.5f, kFringeAlbedo = 0.25f;
constexpr float kFringe = kFringeAlbedo * kCore / kCoreAlbedo;  // the fringe's base colour times the core's lighting
constexpr float kCoreDepth = 0.05f;    // device depth (reversed Z) of the core and of the first fringe
constexpr float kFarFringe = 0.2f;     // a fringe much nearer than any core: no colour to borrow

uint16_t ToHalf(float value) {
  uint32_t f;
  std::memcpy(&f, &value, 4);
  const uint32_t sign = (f >> 16) & 0x8000;
  int exponent = static_cast<int>((f >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = f & 0x7FFFFF;
  if (exponent <= 0) return static_cast<uint16_t>(sign);
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);
  return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

float FromHalf(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1F;
  const uint32_t mantissa = h & 0x3FF;
  uint32_t f = sign;
  if (exponent != 0) f |= ((exponent - 15 + 127) << 23) | (mantissa << 13);
  float value;
  std::memcpy(&value, &f, 4);
  return value;
}

ID3D12Resource* CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = type;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* res = nullptr;
  device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
  return res;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

// Rows of RGBA16F pixels in an upload buffer laid out for CopyTextureRegion.
struct Image {
  std::vector<uint16_t> texels = std::vector<uint16_t>(kWidth * kHeight * 4);
  void Set(uint32_t x, uint32_t y, float r, float g, float b, float a) {
    uint16_t* p = &texels[(y * kWidth + x) * 4];
    p[0] = ToHalf(r), p[1] = ToHalf(g), p[2] = ToHalf(b), p[3] = ToHalf(a);
  }
  float Get(uint32_t x, uint32_t y, int c) const { return FromHalf(texels[(y * kWidth + x) * 4 + c]); }
};

void Upload(ID3D12GraphicsCommandList* cmd, ID3D12Resource* upload, ID3D12Resource* texture, const Image& image) {
  void* mapped = nullptr;
  upload->Map(0, nullptr, &mapped);
  std::memcpy(mapped, image.texels.data(), image.texels.size() * 2);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = upload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, kWidth, kHeight, 1, kWidth * 8};
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = texture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

}  // namespace

int main() {
  ID3D12Debug1* debug = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
    debug->EnableDebugLayer();
    debug->SetEnableGPUBasedValidation(TRUE);  // also checks descriptors and resource states as the GPU runs
  }
  ID3D12Device* device = nullptr;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
    std::printf("no D3D12 device\n");
    return 1;
  }
  ID3D12InfoQueue* info = nullptr;
  device->QueryInterface(IID_PPV_ARGS(&info));

  vegfix::SoftEdges soft;
  std::string error;
  if (!soft.Initialize(device, &error) || !soft.EnsureSize(kWidth, kHeight, 0)) {
    std::printf("soft edges: %s\n", error.c_str());
    return 1;
  }

  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* alloc = nullptr;
  ID3D12GraphicsCommandList* cmd = nullptr;
  ID3D12Fence* fence = nullptr;
  device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd));
  device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  uint64_t fence_value = 0;
  const auto submit = [&] {
    cmd->Close();
    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++fence_value);
    while (fence->GetCompletedValue() < fence_value) Sleep(1);
    alloc->Reset();
    cmd->Reset(alloc, nullptr);
  };

  // The scene colour, like the game's: a float render target that also allows UAV access.
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* scene = nullptr;
  device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&scene));
  ID3D12Resource* upload = CreateBuffer(device, kWidth * kHeight * 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource* readback = CreateBuffer(device, kWidth * kHeight * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

  // Frame 1: the coverage target is new, so the composite only clears it.
  vegfix::SoftEdges::Settings settings;
  soft.Composite(cmd, scene, D3D12_RESOURCE_STATE_RENDER_TARGET, settings);
  submit();

  Image scene_image, coverage, core_albedo, fringe_albedo, fringe_id;
  const float core_id = vegfix::SoftEdges::DrawId(0), other_id = vegfix::SoftEdges::DrawId(1);
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x) {
      const bool core = x >= 100 && x < 120;
      scene_image.Set(x, y, core ? kCore : kBackground, core ? kCore : kBackground, core ? kCore : kBackground, 1.0f);
      float transmittance = 1.0f, fringe_depth = 0.0f;
      if (x >= 120 && x < 140) {  // fringe beside the core
        transmittance = (x - 120 + 0.5f) / 20.0f;
        fringe_depth = kCoreDepth;
      }
      float id = transmittance < 1.0f ? core_id : 0.0f;
      if (x >= 160 && x < 170) {  // fringe of another draw at the core's depth
        transmittance = 0.5f;
        fringe_depth = kCoreDepth;
        id = other_id;
      }
      if (x >= 200 && x < 210) {  // fringe far from any core
        transmittance = 0.5f;
        fringe_depth = kFarFringe;
        id = core_id;
      }
      coverage.Set(x, y, core ? 1.0f : 0.0f, fringe_depth, core ? kCoreDepth : 0.0f, 1.0f - transmittance);
      const float a = core ? 1.0f : 0.0f, opacity = 1.0f - transmittance;
      core_albedo.Set(x, y, kCoreAlbedo * a, kCoreAlbedo * a, kCoreAlbedo * a, core ? core_id : 0.0f);
      fringe_id.Set(x, y, id, id, id, id);
      fringe_albedo.Set(x, y, kFringeAlbedo * opacity, kFringeAlbedo * opacity, kFringeAlbedo * opacity, opacity);
    }

  // Frame 2: write the synthetic targets (one clear per column, the values do not change down a column) and scene,
  // then blend.
  {
    const Image* targets[vegfix::SoftEdges::kTargets] = {&coverage, &core_albedo, &fringe_albedo, &fringe_id};
    for (uint32_t t = 0; t < vegfix::SoftEdges::kTargets; ++t) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        const Image& image = *targets[t];
        const float value[4] = {image.Get(x, 0, 0), image.Get(x, 0, 1), image.Get(x, 0, 2), image.Get(x, 0, 3)};
        const D3D12_RECT rect = {static_cast<LONG>(x), 0, static_cast<LONG>(x + 1), static_cast<LONG>(kHeight)};
        cmd->ClearRenderTargetView(soft.target_rtv(t), value, 1, &rect);
      }
    }
    const D3D12_RESOURCE_BARRIER to_copy = Transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->ResourceBarrier(1, &to_copy);
    Upload(cmd, upload, scene, scene_image);
    const D3D12_RESOURCE_BARRIER to_rt = Transition(scene, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &to_rt);
  }
  soft.Composite(cmd, scene, D3D12_RESOURCE_STATE_RENDER_TARGET, settings);
  {
    const D3D12_RESOURCE_BARRIER to_copy = Transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &to_copy);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = scene;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, kWidth, kHeight, 1, kWidth * 8};
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER back = Transition(scene, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &back);
  }
  submit();

  Image result;
  void* mapped = nullptr;
  readback->Map(0, nullptr, &mapped);
  std::memcpy(result.texels.data(), mapped, result.texels.size() * 2);
  readback->Unmap(0, nullptr);

  int failures = 0;
  const auto check = [&](uint32_t x, float expected, const char* what) {
    const float got = result.Get(x, kHeight / 2, 0);
    const bool ok = std::fabs(got - expected) < 0.01f;
    if (!ok) ++failures;
    std::printf("%-28s x=%3u expected %.3f got %.3f %s\n", what, x, expected, got, ok ? "" : "  <-- FAIL");
  };
  check(50, kBackground, "background");
  check(110, kCore, "core");
  for (uint32_t x = 120; x < 140; x += 4) {
    const float t = (x - 120 + 0.5f) / 20.0f;
    check(x, kFringe * (1.0f - t) + kBackground * t, "fringe beside the core");
  }
  check(165, kBackground, "fringe of another draw");
  check(205, kBackground, "fringe without a near core");

  if (info != nullptr) {
    const UINT64 count = info->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
      SIZE_T size = 0;
      info->GetMessage(i, nullptr, &size);
      std::vector<char> buffer(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
      info->GetMessage(i, message, &size);
      // The test's own per-column clears use other values than the coverage target's clear value.
      if (message->ID == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE) continue;
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
        std::printf("D3D12 %s: %s\n", message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING", message->pDescription);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++failures;
      }
    }
    std::printf("%llu debug layer messages\n", static_cast<unsigned long long>(count));
  } else {
    std::printf("debug layer not available\n");
  }
  std::printf(failures ? "FAILED (%d)\n" : "passed\n", failures);
  return failures ? 1 : 0;
}
