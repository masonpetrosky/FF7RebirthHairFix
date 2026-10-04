// SPDX-License-Identifier: MIT

#include "pipeline_recipe.hpp"

#include <cstring>

#include "crc32.hpp"

using namespace reshade::api;

namespace hairfix {

Blob BlobPool::Get(const void* data, size_t size) {
  const uint64_t key = (static_cast<uint64_t>(Crc32(data, size)) << 32) ^ size;
  std::lock_guard lock(mutex_);
  if (auto it = blobs_.find(key); it != blobs_.end()) {
    if (Blob blob = it->second.lock()) return blob;
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  Blob blob = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + size);
  blobs_[key] = blob;
  if (blobs_.size() > 4096) std::erase_if(blobs_, [](const auto& entry) { return entry.second.expired(); });
  return blob;
}

bool PipelineRecipe::CopyFrom(pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects, BlobPool& pool) {
  layout_ = layout;
  subobjects_.assign(subobjects, subobjects + count);

  for (auto& sub : subobjects_) {
    if (sub.data == nullptr || sub.count == 0) continue;
    size_t element_size = 0;
    switch (sub.type) {
      case pipeline_subobject_type::vertex_shader:
      case pipeline_subobject_type::hull_shader:
      case pipeline_subobject_type::domain_shader:
      case pipeline_subobject_type::geometry_shader:
      case pipeline_subobject_type::pixel_shader:
      case pipeline_subobject_type::amplification_shader:
      case pipeline_subobject_type::mesh_shader: {
        shader_desc desc = *static_cast<const shader_desc*>(sub.data);
        if (desc.spec_constants != 0) return false;  // SPIR-V only; never used by D3D12
        if (desc.code != nullptr && desc.code_size != 0) {
          blobs_.push_back(pool.Get(desc.code, desc.code_size));
          desc.code = blobs_.back()->data();
        }
        if (desc.entry_point != nullptr) desc.entry_point = strings_.emplace_back(desc.entry_point).c_str();
        sub.data = &shaders_.emplace_back(desc);
        if (sub.type == pipeline_subobject_type::pixel_shader) pixel_shader_ = &shaders_.back();
        continue;
      }
      case pipeline_subobject_type::input_layout: {
        auto& bytes = pods_.emplace_back(sizeof(input_element) * sub.count);
        std::memcpy(bytes.data(), sub.data, bytes.size());
        auto* elements = reinterpret_cast<input_element*>(bytes.data());
        for (uint32_t i = 0; i < sub.count; ++i) {
          if (elements[i].semantic != nullptr) elements[i].semantic = strings_.emplace_back(elements[i].semantic).c_str();
        }
        sub.data = bytes.data();
        continue;
      }
      case pipeline_subobject_type::stream_output_state: element_size = sizeof(stream_output_desc); break;
      case pipeline_subobject_type::blend_state: element_size = sizeof(blend_desc); break;
      case pipeline_subobject_type::rasterizer_state: element_size = sizeof(rasterizer_desc); break;
      case pipeline_subobject_type::depth_stencil_state: element_size = sizeof(depth_stencil_desc); break;
      case pipeline_subobject_type::primitive_topology: element_size = sizeof(primitive_topology); break;
      case pipeline_subobject_type::depth_stencil_format:
      case pipeline_subobject_type::render_target_formats: element_size = sizeof(format); break;
      case pipeline_subobject_type::sample_mask:
      case pipeline_subobject_type::sample_count:
      case pipeline_subobject_type::viewport_count:
      case pipeline_subobject_type::max_vertex_count: element_size = sizeof(uint32_t); break;
      case pipeline_subobject_type::dynamic_pipeline_states: element_size = sizeof(dynamic_state); break;
      case pipeline_subobject_type::flags: element_size = sizeof(pipeline_flags); break;
      default:
        return false;
    }
    auto& bytes = pods_.emplace_back(element_size * sub.count);
    std::memcpy(bytes.data(), sub.data, bytes.size());
    sub.data = bytes.data();
  }
  return pixel_shader_ != nullptr;
}

}  // namespace hairfix
