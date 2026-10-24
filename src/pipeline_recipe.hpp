// SPDX-License-Identifier: MIT
#pragma once

#include <reshade.hpp>

#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vegfix {

using Blob = std::shared_ptr<const std::vector<uint8_t>>;

// De-duplicates shader bytecode so queued pipelines that share shaders share one copy.
class BlobPool {
 public:
  Blob Get(const void* data, size_t size);

 private:
  std::mutex mutex_;
  std::unordered_map<uint64_t, std::weak_ptr<const std::vector<uint8_t>>> blobs_;
};

// A self-contained copy of a pipeline description. ReShade only guarantees the subobjects passed
// to init_pipeline for the duration of the event, so this owns everything the pointers refer to.
class PipelineRecipe {
 public:
  // Returns false if the pipeline uses subobjects that cannot be copied (e.g. ray tracing).
  bool CopyFrom(reshade::api::pipeline_layout layout, uint32_t count, const reshade::api::pipeline_subobject* subobjects, BlobPool& pool);

  reshade::api::pipeline_layout layout() const { return layout_; }
  uint32_t count() const { return static_cast<uint32_t>(subobjects_.size()); }
  const reshade::api::pipeline_subobject* data() const { return subobjects_.data(); }
  reshade::api::shader_desc* pixel_shader() { return pixel_shader_; }
  const reshade::api::shader_desc* vertex_shader() const { return vertex_shader_; }  // null for mesh shaders

  // Replaces (or adds) a fixed-size subobject with a copy of `count` elements of T.
  template <typename T>
  void Set(reshade::api::pipeline_subobject_type type, const T* data, uint32_t count) {
    auto& bytes = pods_.emplace_back(sizeof(T) * count);
    std::memcpy(bytes.data(), data, bytes.size());
    for (auto& sub : subobjects_) {
      if (sub.type != type) continue;
      sub.count = count;
      sub.data = bytes.data();
      return;
    }
    subobjects_.push_back({type, count, bytes.data()});
  }
  // The first subobject of a type, or null.
  template <typename T>
  const T* Get(reshade::api::pipeline_subobject_type type) const {
    for (const auto& sub : subobjects_)
      if (sub.type == type && sub.data != nullptr && sub.count != 0) return static_cast<const T*>(sub.data);
    return nullptr;
  }

 private:
  reshade::api::pipeline_layout layout_ = {0};
  std::vector<reshade::api::pipeline_subobject> subobjects_;
  std::deque<reshade::api::shader_desc> shaders_;
  std::deque<std::vector<uint8_t>> pods_;
  std::deque<std::string> strings_;
  std::vector<Blob> blobs_;
  reshade::api::shader_desc* pixel_shader_ = nullptr;
  reshade::api::shader_desc* vertex_shader_ = nullptr;
};

}  // namespace vegfix
