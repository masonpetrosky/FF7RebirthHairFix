// SPDX-License-Identifier: MIT

#include "soft_edges.hpp"

#include <algorithm>

#include "kSoftComposite.h"
#include "kSoftCoreColors.h"
#include "kSoftDownsample.h"

namespace vegfix {

namespace {

// Table layout (soft_edges.hlsl): t0 coverage, t1 scene, t2 core albedo, t3 fringe albedo, t4 fringe ID,
// t5.. lighting levels, t11.. ID levels, u0 and u1.
constexpr uint32_t kLightingSlot = 5;
constexpr uint32_t kIdSlot = kLightingSlot + SoftEdges::kLevels;
constexpr uint32_t kSrvSlots = kIdSlot + SoftEdges::kLevels;
constexpr uint32_t kUavSlots = 2;
constexpr uint32_t kTableSize = kSrvSlots + kUavSlots;
constexpr uint32_t kTables = 128;  // per frame: 1 + (kLevels - 1) + 1 tables; enough for many frames in flight
constexpr DXGI_FORMAT kLevelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kIdFormat = DXGI_FORMAT_R16_FLOAT;
constexpr float kClearColors[SoftEdges::kTargets][4] = {};
constexpr const wchar_t* kTargetNames[SoftEdges::kTargets] = {
    L"FF7RebirthVegetationAndHairFix coverage", L"FF7RebirthVegetationAndHairFix core albedo",
    L"FF7RebirthVegetationAndHairFix fringe albedo", L"FF7RebirthVegetationAndHairFix fringe ID"};

struct Params {
  uint32_t size[2];
  uint32_t source_size[2];
  float depth_tolerance;
  uint32_t debug;
  uint32_t pad[2];
};
static_assert(sizeof(Params) == 32);

template <typename T>
void Release(T*& p) {
  if (p != nullptr) p->Release();
  p = nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

ID3D12Resource* CreateTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ID3D12Resource* resource = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&resource))))
    return nullptr;
  return resource;
}

ID3D12PipelineState* CreateCompute(ID3D12Device* device, ID3D12RootSignature* root, const void* code, size_t size) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root;
  desc.CS = {code, size};
  ID3D12PipelineState* pso = nullptr;
  if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)))) return nullptr;
  return pso;
}

uint32_t LevelSize(uint32_t size, uint32_t level) { return std::max(size >> level, 1u); }

}  // namespace

const float* SoftEdges::ClearColor(uint32_t i) { return kClearColors[i]; }

SoftEdges::~SoftEdges() {
  ReleaseRetired(0, 0);
  ReleaseTextures();
  Release(composite_);
  Release(downsample_);
  Release(core_colors_);
  Release(root_signature_);
  Release(view_heap_);
  Release(rtv_heap_);
}

bool SoftEdges::Initialize(ID3D12Device* device, std::string* error) {
  device_ = device;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = kSrvSlots;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = kUavSlots;
  ranges[1].OffsetInDescriptorsFromTableStart = kSrvSlots;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = sizeof(Params) / 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root = {};
  root.NumParameters = 2;
  root.pParameters = params;
  ID3DBlob* blob = nullptr;
  ID3DBlob* errors = nullptr;
  if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors)) ||
      FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)))) {
    if (error) *error = "could not create the root signature";
    Release(blob);
    Release(errors);
    return false;
  }
  Release(blob);
  Release(errors);

  core_colors_ = CreateCompute(device, root_signature_, kSoftCoreColors, sizeof(kSoftCoreColors));
  downsample_ = CreateCompute(device, root_signature_, kSoftDownsample, sizeof(kSoftDownsample));
  composite_ = CreateCompute(device, root_signature_, kSoftComposite, sizeof(kSoftComposite));
  if (core_colors_ == nullptr || downsample_ == nullptr || composite_ == nullptr) {
    if (error) *error = "could not create the compute pipelines";
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC views = {};
  views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  views.NumDescriptors = kTables * kTableSize;
  views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  D3D12_DESCRIPTOR_HEAP_DESC rtvs = {};
  rtvs.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvs.NumDescriptors = kTargets;
  if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&view_heap_))) ||
      FAILED(device->CreateDescriptorHeap(&rtvs, IID_PPV_ARGS(&rtv_heap_)))) {
    if (error) *error = "could not create the descriptor heaps";
    return false;
  }
  view_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  rtvs_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  return true;
}

void SoftEdges::ReleaseTextures() {
  for (auto& target : targets_) Release(target);
  for (auto& level : levels_) Release(level);
  for (auto& level : id_levels_) Release(level);
  width_ = height_ = 0;
  coverage_valid_ = false;
}

bool SoftEdges::Matches(uint32_t width, uint32_t height) const {
  return targets_[0] != nullptr && width_ == width && height_ == height;
}

bool SoftEdges::EnsureSize(uint32_t width, uint32_t height, uint64_t frame) {
  std::lock_guard lock(mutex_);
  if (!initialized()) return false;
  if (Matches(width, height)) return true;
  // Textures in use by frames in flight are released later.
  const auto retire = [&](ID3D12Resource*& resource) {
    if (resource != nullptr) retired_.push_back({resource, frame});
    resource = nullptr;
  };
  for (auto& target : targets_) retire(target);
  for (auto& level : levels_) retire(level);
  for (auto& level : id_levels_) retire(level);
  ReleaseTextures();

  bool ok = true;
  for (uint32_t i = 0; ok && i < kTargets; ++i) {
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kTargetFormats[i];
    std::copy_n(kClearColors[i], 4, clear.Color);
    targets_[i] = CreateTexture(device_, width, height, kTargetFormats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    ok = targets_[i] != nullptr;
  }
  for (uint32_t i = 0; ok && i < kLevels; ++i) {
    levels_[i] = CreateTexture(device_, LevelSize(width, i), LevelSize(height, i), kLevelFormat,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    id_levels_[i] = CreateTexture(device_, LevelSize(width, i), LevelSize(height, i), kIdFormat,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    ok = levels_[i] != nullptr && id_levels_[i] != nullptr;
  }
  if (!ok) {
    ReleaseTextures();
    return false;
  }
  for (uint32_t i = 0; i < kTargets; ++i) {
    targets_[i]->SetName(kTargetNames[i]);
    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = kTargetFormats[i];
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(targets_[i], &rtv, target_rtv(i));
  }
  width_ = width;
  height_ = height;
  return true;
}

void SoftEdges::ReleaseRetired(uint64_t frame, uint64_t frames) {
  std::lock_guard lock(mutex_);
  auto keep = std::partition(retired_.begin(), retired_.end(),
                             [&](const Retired& r) { return frames != 0 && frame < r.frame + frames; });
  for (auto it = keep; it != retired_.end(); ++it) it->resource->Release();
  retired_.erase(keep, retired_.end());
}

SoftEdges::Table SoftEdges::NextTable() {
  const uint32_t index = next_table_++ % kTables;
  Table table;
  table.cpu = view_heap_->GetCPUDescriptorHandleForHeapStart();
  table.cpu.ptr += static_cast<SIZE_T>(index) * kTableSize * view_increment_;
  table.gpu = view_heap_->GetGPUDescriptorHandleForHeapStart();
  table.gpu.ptr += static_cast<UINT64>(index) * kTableSize * view_increment_;
  // Every slot gets a valid descriptor; unused ones are null views.
  for (uint32_t slot = 0; slot < kSrvSlots; ++slot) WriteSrv(table, slot, nullptr, kLevelFormat);
  for (uint32_t slot = 0; slot < kUavSlots; ++slot) WriteUav(table, slot, nullptr, kLevelFormat);
  return table;
}

void SoftEdges::WriteSrv(const Table& table, uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = table.cpu;
  cpu.ptr += static_cast<SIZE_T>(slot) * view_increment_;
  device_->CreateShaderResourceView(resource, &srv, cpu);
}

void SoftEdges::WriteUav(const Table& table, uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = table.cpu;
  cpu.ptr += static_cast<SIZE_T>(kSrvSlots + slot) * view_increment_;
  device_->CreateUnorderedAccessView(resource, nullptr, &uav, cpu);
}

void SoftEdges::ClearTargets(ID3D12GraphicsCommandList* cmd) {
  for (uint32_t i = 0; i < kTargets; ++i) cmd->ClearRenderTargetView(target_rtv(i), kClearColors[i], 0, nullptr);
  coverage_valid_ = true;
}

void SoftEdges::Clear(ID3D12GraphicsCommandList* cmd) {
  std::lock_guard lock(mutex_);
  if (targets_[0] != nullptr) ClearTargets(cmd);
}

void SoftEdges::Composite(ID3D12GraphicsCommandList* cmd, ID3D12Resource* scene, D3D12_RESOURCE_STATES scene_state,
                          const Settings& settings) {
  std::lock_guard lock(mutex_);
  if (targets_[0] == nullptr) return;
  if (!coverage_valid_ || scene == nullptr) {
    ClearTargets(cmd);
    return;
  }

  // The scene colour is a typed float texture; views use its own format.
  DXGI_FORMAT scene_format = scene->GetDesc().Format;
  if (scene_format == DXGI_FORMAT_R16G16B16A16_TYPELESS) scene_format = DXGI_FORMAT_R16G16B16A16_FLOAT;

  Params params = {};
  params.depth_tolerance = settings.depth_tolerance;
  params.debug = settings.debug;
  const auto dispatch = [&](ID3D12PipelineState* pso, const Table& table, uint32_t width, uint32_t height) {
    params.size[0] = width;
    params.size[1] = height;
    cmd->SetPipelineState(pso);
    cmd->SetComputeRoot32BitConstants(0, sizeof(Params) / 4, &params, 0);
    cmd->SetComputeRootDescriptorTable(1, table.gpu);
    cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  };
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  ID3D12DescriptorHeap* heaps[] = {view_heap_};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetComputeRootSignature(root_signature_);

  // Level 0: the lighting and draw ID of core pixels.
  {
    D3D12_RESOURCE_BARRIER before[kTargets + 3];
    for (uint32_t i = 0; i < kTargets; ++i) before[i] = Transition(targets_[i], D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    before[kTargets] = Transition(scene, scene_state, kRead);
    before[kTargets + 1] = Transition(levels_[0], kRead, kWrite);
    before[kTargets + 2] = Transition(id_levels_[0], kRead, kWrite);
    cmd->ResourceBarrier(_countof(before), before);
    const Table table = NextTable();
    WriteSrv(table, 0, targets_[0], kTargetFormats[0]);
    WriteSrv(table, 1, scene, scene_format);
    WriteSrv(table, 2, targets_[1], kTargetFormats[1]);
    WriteUav(table, 0, levels_[0], kLevelFormat);
    WriteUav(table, 1, id_levels_[0], kIdFormat);
    dispatch(core_colors_, table, width_, height_);
  }
  // Coarser levels (the source level is read as the first lighting and ID slots).
  for (uint32_t level = 1; level < kLevels; ++level) {
    const D3D12_RESOURCE_BARRIER between[] = {
        Transition(levels_[level - 1], kWrite, kRead), Transition(id_levels_[level - 1], kWrite, kRead),
        Transition(levels_[level], kRead, kWrite), Transition(id_levels_[level], kRead, kWrite)};
    cmd->ResourceBarrier(_countof(between), between);
    const Table table = NextTable();
    WriteSrv(table, kLightingSlot, levels_[level - 1], kLevelFormat);
    WriteSrv(table, kIdSlot, id_levels_[level - 1], kIdFormat);
    WriteUav(table, 0, levels_[level], kLevelFormat);
    WriteUav(table, 1, id_levels_[level], kIdFormat);
    params.source_size[0] = LevelSize(width_, level - 1);
    params.source_size[1] = LevelSize(height_, level - 1);
    dispatch(downsample_, table, LevelSize(width_, level), LevelSize(height_, level));
  }
  // Blend the fringe into the scene colour.
  {
    const D3D12_RESOURCE_BARRIER between[] = {Transition(levels_[kLevels - 1], kWrite, kRead),
                                              Transition(id_levels_[kLevels - 1], kWrite, kRead), Transition(scene, kRead, kWrite)};
    cmd->ResourceBarrier(_countof(between), between);
    const Table table = NextTable();
    WriteSrv(table, 0, targets_[0], kTargetFormats[0]);
    WriteSrv(table, 3, targets_[2], kTargetFormats[2]);
    WriteSrv(table, 4, targets_[3], kTargetFormats[3]);
    for (uint32_t level = 0; level < kLevels; ++level) {
      WriteSrv(table, kLightingSlot + level, levels_[level], kLevelFormat);
      WriteSrv(table, kIdSlot + level, id_levels_[level], kIdFormat);
    }
    WriteUav(table, 0, scene, scene_format);
    dispatch(composite_, table, width_, height_);
  }
  D3D12_RESOURCE_BARRIER after[kTargets + 1];
  after[0] = Transition(scene, kWrite, scene_state);
  for (uint32_t i = 0; i < kTargets; ++i) after[1 + i] = Transition(targets_[i], kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmd->ResourceBarrier(_countof(after), after);
  ClearTargets(cmd);
}

}  // namespace vegfix
