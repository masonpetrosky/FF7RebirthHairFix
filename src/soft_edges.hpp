// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vegfix {

// The native D3D12 side of soft edges: the targets the fringe pass draws into, and the compute passes that blend the
// fringe over the lit scene (src/soft_edges.hlsl explains both). The add-on does the bookkeeping of the game's command
// list state around them.
class SoftEdges {
 public:
  // The fringe pass's render targets, in order: coverage, core base colour, fringe base colour, fringe draw ID.
  static constexpr uint32_t kTargets = 4;
  static constexpr DXGI_FORMAT kTargetFormats[kTargets] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                           DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT};
  static constexpr uint32_t kLevels = 6;
  // The ID of the n-th fringe draw of a frame, as the fringe pass's blend factor: n / 2048 for n = 1..2047, exact in
  // 16-bit float.
  static float DrawId(uint32_t n) { return static_cast<float>(n % 2047 + 1) / 2048.0f; }

  struct Settings {
    float depth_tolerance = 0.03f;
    uint32_t debug = 0;  // 1 = fringe opacity, 2 = fringe colour, 3 = core pixels
  };

  SoftEdges() = default;
  SoftEdges(const SoftEdges&) = delete;
  SoftEdges& operator=(const SoftEdges&) = delete;
  ~SoftEdges();

  // Root signature, pipelines and descriptor heaps.
  bool Initialize(ID3D12Device* device, std::string* error);
  bool initialized() const { return root_signature_ != nullptr; }

  // Makes sure the targets and the lighting levels have this size. Textures replaced by a resize stay alive for a few
  // frames. Returns false if they could not be created.
  bool EnsureSize(uint32_t width, uint32_t height, uint64_t frame);
  bool Matches(uint32_t width, uint32_t height) const;
  uint32_t width() const { return width_; }  // 0 before the first EnsureSize
  uint32_t height() const { return height_; }
  // The first of kTargets consecutive RTV descriptors (one descriptor range for OMSetRenderTargets).
  D3D12_CPU_DESCRIPTOR_HANDLE target_rtvs() const { return rtvs_; }
  D3D12_CPU_DESCRIPTOR_HANDLE target_rtv(uint32_t i) const { return {rtvs_.ptr + static_cast<SIZE_T>(i) * rtv_increment_}; }
  static const float* ClearColor(uint32_t i);
  // The targets hold valid data once they have been cleared after their creation.
  bool coverage_valid() const { return coverage_valid_; }

  // Records the passes on `cmd`: reads and writes `scene` (in `scene_state`, which it is left in again) and clears the
  // targets. Leaves the add-on's descriptor heap, compute root signature and pipeline bound: the caller puts the
  // game's back. Without valid targets it only clears.
  void Composite(ID3D12GraphicsCommandList* cmd, ID3D12Resource* scene, D3D12_RESOURCE_STATES scene_state,
                 const Settings& settings);
  // Clears the targets (they are in the RENDER_TARGET state between passes).
  void Clear(ID3D12GraphicsCommandList* cmd);
  // After a clear recorded elsewhere (target_rtv(i) with ClearColor(i)).
  void MarkCleared() {
    std::lock_guard lock(mutex_);
    coverage_valid_ = targets_[0] != nullptr;
  }

  // Releases textures retired at least `frames` frames ago (all with frames = 0).
  void ReleaseRetired(uint64_t frame, uint64_t frames);

 private:
  struct Table {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
  };
  Table NextTable();
  void WriteSrv(const Table& table, uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format);
  void WriteUav(const Table& table, uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format);
  void ClearTargets(ID3D12GraphicsCommandList* cmd);
  void ReleaseTextures();

  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* root_signature_ = nullptr;
  ID3D12PipelineState* core_colors_ = nullptr;
  ID3D12PipelineState* downsample_ = nullptr;
  ID3D12PipelineState* composite_ = nullptr;
  ID3D12DescriptorHeap* view_heap_ = nullptr;  // shader visible, a ring of tables
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  uint32_t view_increment_ = 0;
  uint32_t rtv_increment_ = 0;
  uint32_t next_table_ = 0;

  std::mutex mutex_;
  uint32_t width_ = 0, height_ = 0;
  ID3D12Resource* targets_[kTargets] = {};
  ID3D12Resource* levels_[kLevels] = {};     // lighting and depth
  ID3D12Resource* id_levels_[kLevels] = {};  // draw ID
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs_ = {};
  bool coverage_valid_ = false;
  struct Retired {
    ID3D12Resource* resource;
    uint64_t frame;
  };
  std::vector<Retired> retired_;
};

}  // namespace vegfix
