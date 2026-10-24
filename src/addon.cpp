// FF7 Rebirth Vegetation and Hair Fix
// ReShade add-on that removes the dithered (screen-door) opacity on foliage, hair and other masked
// materials by patching the game's pixel shaders as their pipelines are created. Characters and
// scenery have separate settings; characters get soft edges, the thin parts of hair and fur blended
// over the scene (soft_edges.*). Called FF7 Rebirth Hair Fix before 0.2.0 and FF7 Rebirth Vegetation Fix before
// 0.3.0.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "crc32.hpp"
#include "dxil_patcher.hpp"
#include "pipeline_recipe.hpp"
#include "soft_edges.hpp"

extern "C" __declspec(dllexport) const char* NAME = "FF7 Rebirth Vegetation and Hair Fix";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Removes the dithered (screen-door) transparency of trees, grass and other foliage in FINAL FANTASY VII "
    "REBIRTH so DLSS, DLAA and FSR resolve it cleanly, and gives hair and fur soft, blended edges.";

using namespace reshade::api;

namespace {

namespace fs = std::filesystem;
using ShaderCode = std::shared_ptr<const std::vector<uint8_t>>;

// Bump whenever the patcher's output changes so stale cache entries are ignored.
constexpr int kPatcherVersion = 2;

// Characters (Unreal's GPU-skinned meshes: hair, fur, clothing) and scenery (everything else) have
// separate settings.
enum Group { kCharacters, kScenery, kGroupCount };
constexpr const char* kGroupNames[kGroupCount] = {"character", "scenery"};
constexpr const char* kThresholdKeys[kGroupCount] = {"Threshold", "SceneryThreshold"};
constexpr const char* kDitherKeys[kGroupCount] = {"DitherStrength", "SceneryDitherStrength"};
// By default characters keep the game's own dithering, which DLSS preset L resolved well on hair in
// our tests, and scenery gets a clean alpha test.
constexpr vegfix::PatchOptions kDefaults[kGroupCount] = {{0.5f, 1.0f}, {0.33f, 0.0f}};

constexpr const wchar_t* kIniSection = L"VegetationAndHairFix";

struct PipelineInfo {
  uint64_t serial = 0;  // distinguishes a pipeline from a later one that reuses the same handle
  device* dev = nullptr;
  uint32_t ps_hash = 0;
  uint32_t rt_count = 0;
  format rt_formats[8] = {};
  format ds_format = format::unknown;
  bool blend0 = false;
  bool depth_write = false;
  bool depth_test = false;
};

struct Config {
  bool enabled = true;
  vegfix::PatchOptions groups[kGroupCount] = {kDefaults[kCharacters], kDefaults[kScenery]};
  // Soft edges for characters: the threshold above draws the solid core, and the fainter parts are blended over the
  // scene after lighting instead of being cut away (see soft_edges.hlsl).
  bool soft_edges = true;
  float soft_softness = 0.7f;  // how far below the threshold the blended fringe reaches (0..1 of the threshold)
  int soft_key = 0;            // virtual-key code that toggles soft edges (with Shift: cycles the debug views), 0 = none
  bool dump_shaders = false;
  int toggle_key = 0;  // virtual-key code, 0 = none
  int trace_key = 0;
};

Config g_config;  // edited by the overlay (render thread only)
fs::path g_data_dir;  // <game>\End\Binaries\Win64\FF7RebirthVegetationAndHairFix
fs::path g_ini_path;

std::atomic<bool> g_active{true};
const wchar_t* g_old_version_loaded = nullptr;  // the add-on file of a former name that is installed too

// Patch options in effect, per group. Changing a group's options re-patches its dithered pipelines
// on the worker.
std::mutex g_options_mutex;
vegfix::PatchOptions g_options[kGroupCount];
uint64_t g_options_generation[kGroupCount] = {};
std::atomic<int64_t> g_options_apply_at_ms{0};  // debounced slider changes

// Patching.
vegfix::DxilPatcher* g_patcher = nullptr;  // intentionally leaked: never FreeLibrary from DllMain
std::once_flag g_patcher_once;
std::string g_patcher_error;
std::atomic<bool> g_patcher_failed{false};  // set after g_patcher_error is written

std::mutex g_patch_mutex;
std::unordered_map<uint32_t, ShaderCode> g_overrides;  // loose shaders\0xHASH.cso files
std::unordered_map<uint32_t, ShaderCode> g_patched[kGroupCount];  // for each group's current options
std::unordered_set<uint32_t> g_dithered;               // shader hashes known to need patching
std::unordered_set<uint32_t> g_unpatchable;            // not dithered, or failed to patch
std::ofstream g_cache_index;
std::atomic<size_t> g_patch_failures{0};
std::atomic<size_t> g_shaders_checked{0};

// Pipelines.
struct Source {
  device* dev;
  uint64_t serial;
  uint32_t ps_hash;
  Group group;
  std::shared_ptr<vegfix::PipelineRecipe> recipe;
  const void* original_code;  // the game's pixel shader, owned by the recipe
  size_t original_size;
};
std::shared_mutex g_pipeline_mutex;
std::unordered_map<uint64_t, PipelineInfo> g_pipelines;  // every graphics pipeline with a pixel shader
std::unordered_map<uint64_t, Source> g_sources;          // pipelines with a dithered pixel shader, for (re)building clones
std::unordered_map<uint64_t, pipeline> g_clones;         // original pipeline -> patched clone
uint64_t g_next_serial = 1;
std::atomic<size_t> g_source_count[kGroupCount];
std::atomic<size_t> g_clone_count{0};
std::atomic<size_t> g_clone_failures{0};

// Replaced clones may still be referenced by command lists in flight, so they are destroyed a few
// frames later rather than immediately.
struct RetiredPipeline {
  device* dev;
  pipeline handle;
  uint64_t frame;
};
constexpr uint64_t kRetireFrames = 8;
std::mutex g_retired_mutex;
std::vector<RetiredPipeline> g_retired;
std::atomic<uint64_t> g_frame{0};

// Soft edges. Character pipelines of the G-buffer pass get a fringe pipeline: the same draw with the fringe variant
// of the pixel shader, into the coverage target. The fringe is blended in right after the fog pass.
constexpr const char* kFogResourceName = "FogStruct_IntegratedScatteringVolumeATexture";
std::atomic<bool> g_soft_active{true};  // the setting, flipped by the hotkey
std::mutex g_fringe_mutex;              // the next four
vegfix::FringeOptions g_fringe_options;
uint64_t g_fringe_generation = 0;
std::unordered_map<uint32_t, ShaderCode> g_fringe_shaders;  // for the current fringe options
std::unordered_set<uint32_t> g_fringe_unusable;             // shaders that have no fringe variant
std::unordered_map<uint64_t, pipeline> g_fringes;           // original pipeline -> fringe pipeline (g_pipeline_mutex)
std::unordered_set<uint64_t> g_fog_pipelines;               // the fog pass (g_pipeline_mutex)
std::atomic<size_t> g_fog_count{0};
std::atomic<size_t> g_fringe_count{0};
std::atomic<size_t> g_fringe_failures{0};
vegfix::SoftEdges* g_soft = nullptr;  // created with the device
std::string g_soft_error;
std::atomic<bool> g_coverage_dirty{false};  // fringe drawn since the coverage was last cleared
std::atomic<uint64_t> g_composite_frame{0};  // the frame of the last composite (or clear)
std::atomic<uint32_t> g_fringe_draws{0}, g_fringe_draws_shown{0};  // per frame, for the overlay
std::atomic<uint32_t> g_composites{0}, g_composites_shown{0};
std::atomic<uint32_t> g_soft_debug{0};
std::atomic<float> g_depth_tolerance{0.03f};
// The largest G-buffer pass with fringe draws in this frame: the coverage target follows the main view's size.
std::atomic<uint64_t> g_frame_largest{0};  // width << 32 | height
uint32_t g_frames_smaller = 0;

// Background worker. Analysing and patching shaders and compiling the patched pipelines happens
// here, never on the game's own threads, so the game's pipeline creation is not slowed down.
struct Job {
  device* dev;
  uint64_t pipeline;
  uint64_t serial;
  uint32_t ps_hash;
  std::shared_ptr<vegfix::PipelineRecipe> recipe;
  const void* original_code;
  size_t original_size;
};
vegfix::BlobPool g_blob_pool;
std::mutex g_job_mutex;
std::condition_variable g_job_cv;
std::deque<Job> g_jobs;
std::thread* g_worker = nullptr;
bool g_worker_stop = false;
device* g_job_device = nullptr;  // the device of the job the worker is on, if any
std::atomic<size_t> g_queue_size{0};
std::atomic<int> g_device_count{0};

// The worker waits until the game has been presenting for a while, to stay out of the way of the
// game's own shader-cache setup during boot.
constexpr int64_t kWorkerStartDelayMs = 5000;
std::atomic<int64_t> g_first_present_ms{0};

// Shader dumping (developer).
std::mutex g_dump_mutex;
std::unordered_set<uint32_t> g_dumped;
std::atomic<size_t> g_dump_count{0};

// One-frame draw trace (developer), used to find out which shaders draw what.
enum class TraceState { kIdle, kArmed, kCapturing };
std::atomic<TraceState> g_trace_state{TraceState::kIdle};
std::mutex g_trace_mutex;
struct TraceEntry {
  uint32_t draws = 0;
  uint32_t first_seq = 0;
};
std::unordered_map<command_list*, uint64_t> g_trace_current;
std::unordered_map<uint64_t, TraceEntry> g_trace_entries;
uint32_t g_trace_seq = 0;
std::string g_last_trace_file;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[VegetationAndHairFix] " + message).c_str());
}

std::string HashName(uint32_t hash) {
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%08X", hash);
  return buf;
}

// ---------------------------------------------------------------------------------------------
// Configuration

std::wstring Widen(const char* s) { return std::wstring(s, s + strlen(s)); }

std::wstring ReadIniString(const char* key, const wchar_t* fallback) {
  wchar_t buf[64];
  GetPrivateProfileStringW(kIniSection, Widen(key).c_str(), fallback, buf, 64, g_ini_path.c_str());
  return buf;
}

int ReadIniInt(const char* key, int fallback) {
  try {
    return std::stoi(ReadIniString(key, std::to_wstring(fallback).c_str()));
  } catch (...) {
    return fallback;
  }
}

float ReadIniFloat(const char* key, float fallback) {
  try {
    return std::stof(ReadIniString(key, std::to_wstring(fallback).c_str()));
  } catch (...) {
    return fallback;
  }
}

void WriteIni(const char* key, const std::wstring& value) {
  WritePrivateProfileStringW(kIniSection, Widen(key).c_str(), value.c_str(), g_ini_path.c_str());
}

bool HasIniKey(const char* key) { return !ReadIniString(key, L"").empty(); }

// The add-on was called FF7 Rebirth Hair Fix before 0.2.0 and FF7 Rebirth Vegetation Fix before 0.3.0. The settings
// of the newer former name found are taken over once.
void ImportOldSettings(const fs::path& dir) {
  if (HasIniKey("ImportedOldSettings")) return;
  const fs::path vegetation_ini = dir / "FF7RebirthVegetationFix.ini";
  const fs::path hair_ini = dir / "FF7RebirthHairFix.ini";
  const bool from_vegetation = fs::exists(vegetation_ini);
  if (!from_vegetation && !fs::exists(hair_ini)) return;
  const fs::path& old_ini = from_vegetation ? vegetation_ini : hair_ini;
  const wchar_t* old_section = from_vegetation ? L"VegetationFix" : L"HairFix";
  const auto read_old = [&](const char* key) {
    wchar_t value[64];
    GetPrivateProfileStringW(old_section, Widen(key).c_str(), L"", value, 64, old_ini.c_str());
    return std::wstring(value);
  };
  // Hair Fix 0.1.0 had no scenery settings; its one setting applied to everything.
  const bool split = !read_old("SceneryThreshold").empty();
  const std::pair<const char*, const char*> keys[] = {  // {new key, old key}
      {"Enabled", "Enabled"},
      {"Threshold", "Threshold"},
      {"DitherStrength", "DitherStrength"},
      {"SceneryThreshold", split ? "SceneryThreshold" : "Threshold"},
      {"SceneryDitherStrength", split ? "SceneryDitherStrength" : "DitherStrength"},
      {"SoftEdges", "SoftEdges"},
      {"SoftEdgeSoftness", "SoftEdgeSoftness"},
      {"SoftEdgesKey", "SoftEdgesKey"},
      {"ToggleKey", "ToggleKey"},
      {"TraceKey", "TraceKey"},
      {"DumpShaders", "DumpShaders"}};
  for (const auto& [key, old_key] : keys)
    if (const std::wstring value = read_old(old_key); !value.empty()) WriteIni(key, value);
  WriteIni("ImportedOldSettings", L"1");
  Log(reshade::log::level::info, "Took over the settings in " + old_ini.filename().string() + " (this add-on's former name).");
}

void WriteIniFloat(const char* key, float value) {
  wchar_t buf[32];
  swprintf_s(buf, L"%.2f", value);
  WriteIni(key, buf);
}

// Options are stored rounded to 0.01 so the cache does not fill up with near-identical variants.
float Round2(float value) { return std::round(value * 100.0f) / 100.0f; }

// The patch a group gets. With soft edges, characters get a plain alpha test at their threshold: the fringe pass
// adds back what it cuts away.
vegfix::PatchOptions EffectiveOptions(int group) {
  vegfix::PatchOptions options = g_config.groups[group];
  if (group == kCharacters && g_config.soft_edges) options.dither_strength = 0.0f;
  return options;
}

// The fringe fades in from threshold * (1 - softness) to the threshold.
vegfix::FringeOptions EffectiveFringeOptions() {
  const float threshold = g_config.groups[kCharacters].threshold;
  return {threshold, Round2(threshold * (1.0f - g_config.soft_softness))};
}

void LoadConfig() {
  g_config.enabled = ReadIniInt("Enabled", 1) != 0;
  for (int group = 0; group < kGroupCount; ++group) {
    const vegfix::PatchOptions& fallback = kDefaults[group];
    auto& options = g_config.groups[group];
    options.threshold = Round2(std::clamp(ReadIniFloat(kThresholdKeys[group], fallback.threshold), 0.05f, 0.95f));
    options.dither_strength = Round2(std::clamp(ReadIniFloat(kDitherKeys[group], fallback.dither_strength), 0.0f, 1.0f));
  }
  g_config.soft_edges = ReadIniInt("SoftEdges", 1) != 0;
  g_config.soft_softness = Round2(std::clamp(ReadIniFloat("SoftEdgeSoftness", 0.7f), 0.05f, 1.0f));
  g_config.soft_key = ReadIniInt("SoftEdgesKey", 0);
  for (int group = 0; group < kGroupCount; ++group) g_options[group] = EffectiveOptions(group);
  g_fringe_options = EffectiveFringeOptions();
  g_soft_active = g_config.soft_edges;
  g_config.dump_shaders = ReadIniInt("DumpShaders", 0) != 0;
  g_config.toggle_key = ReadIniInt("ToggleKey", 0);
  g_config.trace_key = ReadIniInt("TraceKey", 0);
  g_active = g_config.enabled;
}

// ---------------------------------------------------------------------------------------------
// Patched shader lookup

ShaderCode ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return nullptr;
  auto code = std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (code->empty()) return nullptr;
  return code;
}

void LoadOverrides() {
  std::error_code ec;
  const fs::path dir = g_data_dir / "shaders";
  if (!fs::is_directory(dir, ec)) return;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    uint32_t hash = 0;
    if (entry.path().extension() != ".cso" || sscanf_s(entry.path().stem().string().c_str(), "0x%8X", &hash) != 1) continue;
    if (auto code = ReadFile(entry.path())) g_overrides[hash] = std::move(code);
  }
}

// cache\v<N>\index.txt          shaders that need no patch (or could not be patched)
// cache\v<N>\t<T>_s<S>\0x*.cso  patched shaders, per option set
fs::path CacheRoot() { return g_data_dir / "cache" / ("v" + std::to_string(kPatcherVersion)); }

fs::path OptionsCacheDir(const vegfix::PatchOptions& options) {
  char key[48];
  snprintf(key, sizeof(key), "t%.2f_s%.2f", options.threshold, options.dither_strength);
  return CacheRoot() / key;
}

void OpenCache() {
  std::error_code ec;
  fs::create_directories(CacheRoot(), ec);
  const fs::path index_path = CacheRoot() / "index.txt";
  std::ifstream index(index_path);
  std::string line;
  while (std::getline(index, line)) {
    uint32_t hash = 0;
    if (sscanf_s(line.c_str(), "0x%8X", &hash) == 1) g_unpatchable.insert(hash);
  }
  g_cache_index.open(index_path, std::ios::app);
}

void InitPatcher() {
  g_patcher = new vegfix::DxilPatcher();
  if (!g_patcher->Initialize(g_data_dir, &g_patcher_error)) {
    Log(reshade::log::level::error, "Shader patching disabled: " + g_patcher_error);
    g_patcher_failed = true;
    return;
  }
  Log(reshade::log::level::info, g_patcher->validating() ? "Patcher ready (validating with dxil.dll)." : "Patcher ready.");
}

// Returns the patched replacement for a pixel shader, or null if it should be left alone.
// Worker thread only.
ShaderCode GetPatchedShader(uint32_t hash, const void* code, size_t size, Group group, const vegfix::PatchOptions& options,
                            uint64_t generation) {
  {
    std::lock_guard lock(g_patch_mutex);
    if (auto it = g_overrides.find(hash); it != g_overrides.end()) return it->second;
    if (g_unpatchable.count(hash)) return nullptr;
    if (auto it = g_patched[group].find(hash); it != g_patched[group].end()) return it->second;
  }

  const fs::path cache_dir = OptionsCacheDir(options);
  const fs::path cache_file = cache_dir / (HashName(hash) + ".cso");
  ShaderCode patched = ReadFile(cache_file);
  const bool from_cache = patched != nullptr;
  if (!from_cache) {
    std::call_once(g_patcher_once, InitPatcher);
    if (!g_patcher->initialized()) return nullptr;

    vegfix::PatchStats stats;
    std::vector<uint8_t> out;
    std::string error;
    ++g_shaders_checked;
    switch (g_patcher->Patch(code, size, options, &out, &stats, &error)) {
      case vegfix::PatchResult::kPatched:
        patched = std::make_shared<std::vector<uint8_t>>(std::move(out));
        break;
      case vegfix::PatchResult::kNotApplicable:
        break;
      case vegfix::PatchResult::kFailed:
        ++g_patch_failures;
        Log(reshade::log::level::warning, "Could not patch pixel shader " + HashName(hash) + ": " + error);
        break;
    }
  }

  if (patched != nullptr && !from_cache) {
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    std::ofstream(cache_file, std::ios::binary).write(reinterpret_cast<const char*>(patched->data()), patched->size());
  }

  std::lock_guard lock(g_patch_mutex);
  if (patched == nullptr) {
    if (g_unpatchable.insert(hash).second) g_cache_index << HashName(hash) << "\n" << std::flush;
    return nullptr;
  }
  g_dithered.insert(hash);
  bool current = false;
  {
    std::lock_guard options_lock(g_options_mutex);
    current = generation == g_options_generation[group];
  }
  if (current) g_patched[group].emplace(hash, patched);
  return patched;
}

// The fringe variant of a dithered G-buffer pixel shader, or null. Worker thread only.
// cache\v<N>\fringe<F>_c<T>_f<A>\0x*.cso, per fringe option set.
constexpr int kFringeVersion = 4;  // bump whenever the fringe variant changes

ShaderCode GetFringeShader(uint32_t hash, const void* code, size_t size, const vegfix::FringeOptions& options, uint64_t generation) {
  {
    std::lock_guard lock(g_fringe_mutex);
    if (g_fringe_unusable.count(hash)) return nullptr;
    if (auto it = g_fringe_shaders.find(hash); it != g_fringe_shaders.end()) return it->second;
  }
  char key[64];
  snprintf(key, sizeof(key), "fringe%d_c%.2f_f%.2f", kFringeVersion, options.core_threshold, options.fade_from);
  const fs::path cache_file = CacheRoot() / key / (HashName(hash) + ".cso");
  ShaderCode fringe = ReadFile(cache_file);
  if (fringe == nullptr) {
    std::call_once(g_patcher_once, InitPatcher);
    if (!g_patcher->initialized()) return nullptr;
    std::vector<uint8_t> out;
    std::string error;
    switch (g_patcher->MakeFringe(code, size, options, &out, &error)) {
      case vegfix::PatchResult::kPatched: {
        fringe = std::make_shared<std::vector<uint8_t>>(std::move(out));
        std::error_code ec;
        fs::create_directories(cache_file.parent_path(), ec);
        std::ofstream(cache_file, std::ios::binary).write(reinterpret_cast<const char*>(fringe->data()), fringe->size());
        break;
      }
      case vegfix::PatchResult::kNotApplicable:
        Log(reshade::log::level::info, "No soft edges for pixel shader " + HashName(hash) + ": " + error);
        break;
      case vegfix::PatchResult::kFailed:
        ++g_fringe_failures;
        Log(reshade::log::level::warning, "Could not make the soft-edge variant of pixel shader " + HashName(hash) + ": " + error);
        break;
    }
  }
  std::lock_guard lock(g_fringe_mutex);
  if (fringe == nullptr) {
    g_fringe_unusable.insert(hash);
    return nullptr;
  }
  if (generation == g_fringe_generation) g_fringe_shaders.emplace(hash, fringe);
  return fringe;
}

// Characters are drawn with Unreal's GPU-skinned vertex factories, whose vertex shaders read the
// BoneMatrices buffer. Everything else is scenery: Square Enix's mesh-shader environment renderer
// (trees, rocks, buildings), static props and particles.
Group Classify(const vegfix::PipelineRecipe& recipe) {
  const shader_desc* vs = recipe.vertex_shader();
  if (vs != nullptr && vs->code != nullptr && vegfix::ContainsName(vs->code, vs->code_size, "BoneMatrices")) return kCharacters;
  return kScenery;
}

// ---------------------------------------------------------------------------------------------
// Developer tools: shader dump and draw trace

const char* FormatName(format f) {
  switch (f) {
    case format::unknown: return "-";
    case format::r8g8b8a8_unorm: return "rgba8";
    case format::r8g8b8a8_unorm_srgb: return "rgba8_srgb";
    case format::b8g8r8a8_unorm: return "bgra8";
    case format::b8g8r8a8_unorm_srgb: return "bgra8_srgb";
    case format::r10g10b10a2_unorm: return "rgb10a2";
    case format::r11g11b10_float: return "r11g11b10f";
    case format::r16g16b16a16_float: return "rgba16f";
    case format::r16g16_float: return "rg16f";
    case format::r16g16_unorm: return "rg16";
    case format::r16_float: return "r16f";
    case format::r32_float: return "r32f";
    case format::r8_unorm: return "r8";
    case format::r8g8_unorm: return "rg8";
    case format::r32g32_float: return "rg32f";
    case format::d32_float: return "d32";
    case format::d32_float_s8_uint: return "d32s8";
    case format::d24_unorm_s8_uint: return "d24s8";
    case format::d16_unorm: return "d16";
    default: return nullptr;
  }
}

std::string FormatString(format f) {
  if (const char* name = FormatName(f)) return name;
  return "fmt" + std::to_string(static_cast<uint32_t>(f));
}

std::string DescribePipeline(const PipelineInfo& info) {
  std::string rts;
  for (uint32_t i = 0; i < info.rt_count; ++i) rts += (i ? "|" : "") + FormatString(info.rt_formats[i]);
  if (rts.empty()) rts = "none";
  return rts + "," + FormatString(info.ds_format) + "," + (info.depth_test ? "1" : "0") + "," + (info.depth_write ? "1" : "0") + "," +
         (info.blend0 ? "1" : "0");
}

void DumpShader(uint32_t hash, const shader_desc& desc, const PipelineInfo& info) {
  std::lock_guard lock(g_dump_mutex);
  if (!g_dumped.insert(hash).second) return;

  std::error_code ec;
  const fs::path dir = g_data_dir / "dump";
  fs::create_directories(dir, ec);
  const fs::path file = dir / (HashName(hash) + ".ps.cso");
  if (!fs::exists(file, ec)) {
    std::ofstream out(file, std::ios::binary);
    out.write(static_cast<const char*>(desc.code), static_cast<std::streamsize>(desc.code_size));
  }
  std::ofstream csv(dir / "pipelines.csv", std::ios::app);
  csv << HashName(hash) << "," << DescribePipeline(info) << "\n";
  ++g_dump_count;
}

void CountTraceDraw(command_list* cmd_list) {
  if (g_trace_state.load(std::memory_order_relaxed) != TraceState::kCapturing) return;
  std::lock_guard lock(g_trace_mutex);
  auto current = g_trace_current.find(cmd_list);
  if (current == g_trace_current.end()) return;
  auto& entry = g_trace_entries[current->second];
  if (entry.draws++ == 0) entry.first_seq = g_trace_seq++;
}

void WriteTrace() {
  struct Row {
    uint32_t seq;
    uint32_t draws;
    PipelineInfo info;
    bool known;
    bool cloned;
    const char* group;
  };
  std::vector<Row> rows;
  {
    std::lock_guard trace_lock(g_trace_mutex);
    std::shared_lock pipeline_lock(g_pipeline_mutex);
    for (const auto& [handle, entry] : g_trace_entries) {
      Row row{entry.first_seq, entry.draws, {}, false, g_clones.count(handle) != 0, "-"};
      if (auto it = g_pipelines.find(handle); it != g_pipelines.end()) {
        row.info = it->second;
        row.known = true;
      }
      if (auto it = g_sources.find(handle); it != g_sources.end()) row.group = kGroupNames[it->second.group];
      rows.push_back(row);
    }
    g_trace_entries.clear();
    g_trace_current.clear();
    g_trace_seq = 0;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.seq < b.seq; });

  std::error_code ec;
  const fs::path dir = g_data_dir / "traces";
  fs::create_directories(dir, ec);
  SYSTEMTIME t;
  GetLocalTime(&t);
  char name[64];
  snprintf(name, sizeof(name), "trace_%04d%02d%02d_%02d%02d%02d.csv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  std::ofstream out(dir / name);
  out << "order,ps_hash,draws,rts,ds_format,depth_test,depth_write,blend0,patched,group\n";
  for (const auto& row : rows) {
    out << row.seq << "," << (row.known ? HashName(row.info.ps_hash) : std::string("none")) << "," << row.draws << ","
        << (row.known ? DescribePipeline(row.info) : std::string("?,?,?,?,?")) << "," << (row.cloned ? 1 : 0) << "," << row.group
        << "\n";
  }
  g_last_trace_file = name;
  Log(reshade::log::level::info, "Wrote draw trace " + (dir / name).string());
}

// ---------------------------------------------------------------------------------------------
// Worker

void RetirePipeline(device* dev, pipeline handle) {
  std::lock_guard lock(g_retired_mutex);
  g_retired.push_back({dev, handle, g_frame.load()});
}

// Destroys the retired clones old enough to be out of use, or all of them; of every device, or only of `only`.
void DestroyRetiredPipelines(bool all, device* only = nullptr) {
  std::vector<RetiredPipeline> expired;
  {
    std::lock_guard lock(g_retired_mutex);
    const uint64_t frame = g_frame.load();
    auto keep = std::partition(g_retired.begin(), g_retired.end(), [&](const RetiredPipeline& r) {
      return (only != nullptr && r.dev != only) || (!all && frame < r.frame + kRetireFrames);
    });
    expired.assign(keep, g_retired.end());
    g_retired.erase(keep, g_retired.end());
  }
  for (const auto& r : expired) r.dev->destroy_pipeline(r.handle);
}

void EnqueueJob(Job job) {
  {
    std::lock_guard lock(g_job_mutex);
    g_jobs.push_back(std::move(job));
    g_queue_size = g_jobs.size();
  }
  g_job_cv.notify_one();
}

// The fringe pipeline of a character pipeline in the G-buffer pass: the same draw with the fringe pixel shader into the
// soft-edge targets, depth-tested against the finished depth (reversed Z) without writing it:
//   coverage       MAX on all channels (alpha: the strongest fringe layer, as the game's dithering covers)
//   core albedo    written where the fragment's alpha (1 for core fragments, else 0) says so; alpha = the draw's ID
//   fringe albedo  summed
//   fringe ID      the draw's ID where a fringe fragment is
// The draw's ID comes in as the blend factor (SoftEdges::DrawId), so the composite lights a fringe only from the
// solid core of the same draw: a layer without a core of its own, such as eyebrow hairs, keeps the cutout look.
// Null if the shader has no fringe variant.
pipeline CreateFringePipeline(const Job& job) {
  vegfix::FringeOptions options;
  uint64_t generation;
  {
    std::lock_guard lock(g_fringe_mutex);
    options = g_fringe_options;
    generation = g_fringe_generation;
  }
  ShaderCode code = GetFringeShader(job.ps_hash, job.original_code, job.original_size, options, generation);
  if (code == nullptr) return {0};

  vegfix::PipelineRecipe recipe;
  if (!recipe.CopyFrom(job.recipe->layout(), job.recipe->count(), job.recipe->data(), g_blob_pool)) return {0};
  recipe.pixel_shader()->code = code->data();
  recipe.pixel_shader()->code_size = code->size();
  const format targets[vegfix::SoftEdges::kTargets] = {format::r16g16b16a16_float, format::r16g16b16a16_float,
                                                       format::r16g16b16a16_float, format::r16_float};
  recipe.Set(pipeline_subobject_type::render_target_formats, targets, vegfix::SoftEdges::kTargets);
  blend_desc blend;
  for (uint32_t i = 0; i < vegfix::SoftEdges::kTargets; ++i) blend.blend_enable[i] = true;
  blend.source_color_blend_factor[0] = blend_factor::one;
  blend.dest_color_blend_factor[0] = blend_factor::one;
  blend.color_blend_op[0] = blend_op::max;
  blend.source_alpha_blend_factor[0] = blend_factor::one;
  blend.dest_alpha_blend_factor[0] = blend_factor::one;
  blend.alpha_blend_op[0] = blend_op::max;
  blend.source_color_blend_factor[1] = blend_factor::source_alpha;
  blend.dest_color_blend_factor[1] = blend_factor::one_minus_source_alpha;
  blend.source_alpha_blend_factor[1] = blend_factor::constant_color;
  blend.dest_alpha_blend_factor[1] = blend_factor::one_minus_source_alpha;
  blend.source_color_blend_factor[2] = blend_factor::one;
  blend.dest_color_blend_factor[2] = blend_factor::one;
  blend.source_alpha_blend_factor[2] = blend_factor::one;
  blend.dest_alpha_blend_factor[2] = blend_factor::one;
  blend.source_color_blend_factor[3] = blend_factor::constant_color;
  blend.dest_color_blend_factor[3] = blend_factor::one_minus_source_color;
  blend.source_alpha_blend_factor[3] = blend_factor::constant_color;
  blend.dest_alpha_blend_factor[3] = blend_factor::one_minus_source_alpha;
  recipe.Set(pipeline_subobject_type::blend_state, &blend, 1);
  depth_stencil_desc depth;
  depth.depth_enable = true;
  depth.depth_write_mask = false;
  depth.depth_func = compare_op::greater_equal;
  depth.stencil_enable = false;
  recipe.Set(pipeline_subobject_type::depth_stencil_state, &depth, 1);

  pipeline fringe = {0};
  if (!job.dev->create_pipeline(recipe.layout(), recipe.count(), recipe.data(), &fringe)) {
    ++g_fringe_failures;
    Log(reshade::log::level::warning, "Failed to create the soft-edge pipeline for pixel shader " + HashName(job.ps_hash));
    return {0};
  }
  return fringe;
}

void ProcessJob(Job& job) {
  uint32_t rt_count = 0;
  {
    std::shared_lock lock(g_pipeline_mutex);
    auto it = g_pipelines.find(job.pipeline);
    if (it == g_pipelines.end() || it->second.serial != job.serial) return;
    rt_count = it->second.rt_count;
  }

  const Group group = Classify(*job.recipe);
  vegfix::PatchOptions options;
  uint64_t generation;
  {
    std::lock_guard lock(g_options_mutex);
    options = g_options[group];
    generation = g_options_generation[group];
  }
  ShaderCode replacement = GetPatchedShader(job.ps_hash, job.original_code, job.original_size, group, options, generation);
  if (replacement == nullptr) return;

  // With the game's own test the original pipeline stays bound; the source is kept for other settings.
  pipeline clone = {0};
  if (!options.original()) {
    shader_desc* ps = job.recipe->pixel_shader();
    ps->code = replacement->data();
    ps->code_size = replacement->size();
    if (!job.dev->create_pipeline(job.recipe->layout(), job.recipe->count(), job.recipe->data(), &clone)) {
      ++g_clone_failures;
      Log(reshade::log::level::warning, "Failed to create patched pipeline for pixel shader " + HashName(job.ps_hash));
      return;
    }
  }

  pipeline previous = {0};
  {
    std::unique_lock lock(g_pipeline_mutex);
    auto it = g_pipelines.find(job.pipeline);
    if (it == g_pipelines.end() || it->second.serial != job.serial) {
      lock.unlock();
      if (clone.handle != 0) job.dev->destroy_pipeline(clone);  // the original went away while we were compiling; never bound
      return;
    }
    if (auto existing = g_clones.find(job.pipeline); existing != g_clones.end()) previous = existing->second;
    if (clone.handle != 0)
      g_clones[job.pipeline] = clone;
    else
      g_clones.erase(job.pipeline);
    g_clone_count = g_clones.size();
    const Source source = {job.dev, job.serial, job.ps_hash, group, job.recipe, job.original_code, job.original_size};
    if (g_sources.insert_or_assign(job.pipeline, source).second) ++g_source_count[group];
  }
  if (previous.handle != 0) RetirePipeline(job.dev, previous);

  // Characters in the G-buffer pass (several render targets) also get their fringe pipeline.
  uint64_t fringe_generation = 0;
  if (group == kCharacters && rt_count >= 4) {
    {
      std::lock_guard lock(g_fringe_mutex);
      fringe_generation = g_fringe_generation;
    }
    const pipeline fringe = CreateFringePipeline(job);
    pipeline old_fringe = {0};
    {
      std::unique_lock lock(g_pipeline_mutex);
      auto it = g_pipelines.find(job.pipeline);
      if (it == g_pipelines.end() || it->second.serial != job.serial) {
        lock.unlock();
        if (fringe.handle != 0) job.dev->destroy_pipeline(fringe);
        return;
      }
      if (auto existing = g_fringes.find(job.pipeline); existing != g_fringes.end()) old_fringe = existing->second;
      if (fringe.handle != 0)
        g_fringes[job.pipeline] = fringe;
      else
        g_fringes.erase(job.pipeline);
      g_fringe_count = g_fringes.size();
    }
    if (old_fringe.handle != 0) RetirePipeline(job.dev, old_fringe);
  }

  // A settings change while this job ran may have queued its rebuilds before this pipeline was a source.
  bool stale = false;
  {
    std::lock_guard lock(g_options_mutex);
    stale = generation != g_options_generation[group];
  }
  if (group == kCharacters && rt_count >= 4) {
    std::lock_guard lock(g_fringe_mutex);
    stale |= fringe_generation != g_fringe_generation;
  }
  if (stale) EnqueueJob(job);
}

void WorkerMain() {
  // Hold a reference to this module so it cannot be unloaded while the worker runs; released on exit.
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&WorkerMain), &self);
  Log(reshade::log::level::info, "Worker thread started.");
  size_t processed = 0;
  size_t reported = 0;
  int64_t reported_ms = 0;
  for (;;) {
    // Once the queue has run dry, at most every 30 seconds.
    if (processed != reported && g_queue_size == 0 && NowMs() - reported_ms >= 30000) {
      reported = processed;
      reported_ms = NowMs();
      Log(reshade::log::level::info, std::to_string(processed) + " pipelines processed: " +
                                         std::to_string(g_source_count[kCharacters].load()) + " character and " +
                                         std::to_string(g_source_count[kScenery].load()) + " scenery pipelines dithered, " +
                                         std::to_string(g_clone_count.load()) + " patched pipelines live.");
    }
    Job job;
    {
      std::unique_lock lock(g_job_mutex);
      g_job_cv.wait_for(lock, std::chrono::milliseconds(250), [] { return g_worker_stop || !g_jobs.empty(); });
      if (g_worker_stop) break;
      const int64_t first_present = g_first_present_ms.load();
      if (g_jobs.empty() || first_present == 0 || NowMs() - first_present < kWorkerStartDelayMs) continue;
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
      g_queue_size = g_jobs.size();
      g_job_device = job.dev;
    }
    ProcessJob(job);
    {
      std::lock_guard lock(g_job_mutex);
      g_job_device = nullptr;
    }
    g_job_cv.notify_all();  // ForgetDevice may be waiting for this job
    ++processed;
  }
  Log(reshade::log::level::info, "Worker thread stopped.");
  FreeLibraryAndExitThread(self, 0);
}

void StartWorker() {
  std::lock_guard lock(g_job_mutex);
  if (g_worker != nullptr) return;
  g_worker_stop = false;
  g_worker = new std::thread(WorkerMain);
}

void StopWorker() {
  std::thread* worker = nullptr;
  {
    std::lock_guard lock(g_job_mutex);
    worker = g_worker;
    g_worker = nullptr;
    g_worker_stop = true;
    g_jobs.clear();
    g_queue_size = 0;
  }
  g_job_cv.notify_all();
  if (worker != nullptr) {
    worker->join();
    delete worker;
  }
}

// Switches a group to new patch options and rebuilds its dithered pipelines with them.
void ApplyOptions(Group group, const vegfix::PatchOptions& options) {
  {
    std::lock_guard lock(g_options_mutex);
    if (options == g_options[group]) return;
    g_options[group] = options;
    ++g_options_generation[group];
  }
  {
    std::lock_guard lock(g_patch_mutex);
    g_patched[group].clear();
  }
  std::vector<Job> jobs;
  {
    std::shared_lock lock(g_pipeline_mutex);
    for (const auto& [handle, source] : g_sources) {
      if (source.group != group) continue;
      jobs.push_back({source.dev, handle, source.serial, source.ps_hash, source.recipe, source.original_code, source.original_size});
    }
  }
  for (auto& job : jobs) EnqueueJob(std::move(job));
  Log(reshade::log::level::info,
      "Rebuilding " + std::to_string(jobs.size()) + " " + kGroupNames[group] + " pipelines with new settings.");
}

// Switches to new fringe options and rebuilds the character pipelines' fringe pipelines with them.
void ApplyFringeOptions(const vegfix::FringeOptions& options) {
  {
    std::lock_guard lock(g_fringe_mutex);
    if (options == g_fringe_options) return;
    g_fringe_options = options;
    ++g_fringe_generation;
    g_fringe_shaders.clear();
  }
  std::vector<Job> jobs;
  {
    std::shared_lock lock(g_pipeline_mutex);
    for (const auto& [handle, source] : g_sources) {
      if (source.group != kCharacters) continue;
      jobs.push_back({source.dev, handle, source.serial, source.ps_hash, source.recipe, source.original_code, source.original_size});
    }
  }
  for (auto& job : jobs) EnqueueJob(std::move(job));
}

// ---------------------------------------------------------------------------------------------
// ReShade events

// What the game has bound on a command list: the fringe draws and the soft-edge composite record native commands in
// the middle of the game's, and put this state back afterwards.
struct __declspec(uuid("3b8f1f2e-5d0c-4a7e-9c61-2f7e8d4b6a15")) CmdState {
  uint64_t original = 0;  // the pipeline the game bound
  uint64_t bound = 0;     // the pipeline actually bound: a patched clone or the original
  uint64_t fringe = 0;    // its fringe pipeline, if any
  bool fog = false;       // it is the fog pass
  uint32_t rtv_count = 0;
  uint64_t rtvs[8] = {};
  uint64_t dsv = 0;
  float blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // D3D12's default; ReShade reports it with 8 bits per channel

  // Compute root signature and arguments, and the graphics descriptor tables.
  struct RootArg {
    enum Kind : uint8_t { kNone, kTable, kCbv, kSrv, kUav, kConstants } kind = kNone;
    uint64_t value = 0;  // GPU descriptor handle or GPU virtual address
    std::vector<uint32_t> constants;
    std::vector<uint8_t> constants_set;
  };
  uint64_t compute_layout = 0;
  std::vector<RootArg> compute;
  uint64_t graphics_layout = 0;
  std::vector<uint64_t> graphics_tables;

  // Set at the fog draw: the composite runs before the next command on this list.
  bool pending = false;
  uint64_t pending_scene = 0;
  uint32_t pending_state = 0;  // D3D12_RESOURCE_STATES

  RootArg& ComputeArg(uint64_t layout, uint32_t index) {
    if (layout != compute_layout) {  // a new root signature: earlier arguments are gone
      compute_layout = layout;
      compute.clear();
    }
    if (compute.size() <= index) compute.resize(index + 1);
    return compute[index];
  }
};

CmdState* State(command_list* cmd_list) { return cmd_list->get_private_data<CmdState>(); }

bool SoftEdgesOn() {
  return g_soft != nullptr && g_config.soft_edges && g_soft_active.load(std::memory_order_relaxed) &&
         g_active.load(std::memory_order_relaxed);
}

// Puts the game's state back after the add-on's compute passes. Binding no tables for the game's compute root
// signature makes ReShade set the game's descriptor heaps and that root signature again; then the compute arguments,
// the graphics descriptor tables (the heaps were switched) and the pipeline.
void RestoreGameState(command_list* cmd_list, const CmdState& st) {
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  cmd_list->bind_descriptor_tables(shader_stage::all_compute, {st.compute_layout}, 0, 0, nullptr);
  for (uint32_t i = 0; i < st.compute.size(); ++i) {
    const auto& arg = st.compute[i];
    switch (arg.kind) {
      case CmdState::RootArg::kTable:
        native->SetComputeRootDescriptorTable(i, {arg.value});
        break;
      case CmdState::RootArg::kCbv:
        native->SetComputeRootConstantBufferView(i, arg.value);
        break;
      case CmdState::RootArg::kSrv:
        native->SetComputeRootShaderResourceView(i, arg.value);
        break;
      case CmdState::RootArg::kUav:
        native->SetComputeRootUnorderedAccessView(i, arg.value);
        break;
      case CmdState::RootArg::kConstants:
        for (uint32_t k = 0; k < arg.constants.size(); ++k)
          if (arg.constants_set[k]) native->SetComputeRoot32BitConstant(i, arg.constants[k], k);
        break;
      default:
        break;
    }
  }
  for (uint32_t i = 0; i < st.graphics_tables.size(); ++i)
    if (st.graphics_tables[i] != 0) native->SetGraphicsRootDescriptorTable(i, {st.graphics_tables[i]});
  if (st.bound != 0) native->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(st.bound));
}

// After the fog draw: blend the fringe over the lit scene, or at least clear the coverage.
void RunPendingComposite(command_list* cmd_list, CmdState* st) {
  if (st == nullptr || !st->pending) return;
  st->pending = false;
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (st->compute_layout == 0 || st->pending_state != D3D12_RESOURCE_STATE_RENDER_TARGET || !SoftEdgesOn()) {
    g_soft->Clear(native);
  } else {
    vegfix::SoftEdges::Settings settings;
    settings.depth_tolerance = g_depth_tolerance.load();
    settings.debug = g_soft_debug.load();
    g_soft->Composite(native, reinterpret_cast<ID3D12Resource*>(st->pending_scene), D3D12_RESOURCE_STATE_RENDER_TARGET,
                      settings);
    RestoreGameState(cmd_list, *st);
    ++g_composites;
  }
  g_coverage_dirty = false;
  g_composite_frame = g_frame.load();
}

uint64_t PackSize(uint32_t width, uint32_t height) { return static_cast<uint64_t>(width) << 32 | height; }
uint64_t Pixels(uint64_t packed) { return (packed >> 32) * (packed & 0xFFFFFFFF); }

// The size of a command list's first render target, or 0.
uint64_t TargetSize(command_list* cmd_list, const CmdState& st, resource* target) {
  if (st.rtv_count == 0 || st.rtvs[0] == 0) return 0;
  device* const dev = cmd_list->get_device();
  const resource res = dev->get_resource_from_view({st.rtvs[0]});
  if (res == 0) return 0;
  if (target != nullptr) *target = res;
  const resource_desc desc = dev->get_resource_desc(res);
  return PackSize(desc.texture.width, desc.texture.height);
}

// A character draw in the G-buffer pass: draw its fringe into the coverage target first. `draw` repeats the game's
// draw call on the native command list.
template <typename Draw>
void DrawFringe(command_list* cmd_list, CmdState* st, Draw&& draw) {
  if (st == nullptr || st->fringe == 0 || st->dsv == 0 || !SoftEdgesOn()) return;
  const uint64_t size = TargetSize(cmd_list, *st, nullptr);
  if (size == 0) return;
  // The coverage follows the largest G-buffer pass, the main view; smaller views (scene captures) get no fringe.
  for (uint64_t largest = g_frame_largest.load(); Pixels(size) > Pixels(largest) && !g_frame_largest.compare_exchange_weak(largest, size);) {
  }
  const uint32_t width = static_cast<uint32_t>(size >> 32), height = static_cast<uint32_t>(size);
  if (!g_soft->Matches(width, height)) {
    // Grow to a larger view at once. A smaller one is a scene capture, or the render resolution went down, which the
    // per-frame housekeeping follows.
    if (g_soft->width() != 0 && Pixels(size) <= Pixels(PackSize(g_soft->width(), g_soft->height()))) return;
    if (!g_soft->EnsureSize(width, height, g_frame.load())) return;
  }

  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  const D3D12_CPU_DESCRIPTOR_HANDLE targets = g_soft->target_rtvs();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = {st->dsv};
  const float id = vegfix::SoftEdges::DrawId(g_fringe_draws.fetch_add(1));
  const float blend_factor[4] = {id, id, id, id};
  native->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(st->fringe));
  native->OMSetRenderTargets(vegfix::SoftEdges::kTargets, &targets, TRUE, &dsv);
  native->OMSetBlendFactor(blend_factor);
  draw(native);
  native->OMSetBlendFactor(st->blend_factor);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
  for (uint32_t i = 0; i < st->rtv_count; ++i) rtvs[i] = {st->rtvs[i]};
  native->OMSetRenderTargets(st->rtv_count, rtvs, FALSE, &dsv);
  native->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(st->bound));
  g_coverage_dirty = true;
}

// The fog draw: the composite runs right after it, so the fringe takes its colour from fogged core pixels.
void CheckFog(command_list* cmd_list, CmdState* st) {
  if (st == nullptr || !st->fog || !SoftEdgesOn()) return;
  resource scene = {0};
  const uint64_t size = TargetSize(cmd_list, *st, &scene);
  if (size == 0 || !g_soft->Matches(static_cast<uint32_t>(size >> 32), static_cast<uint32_t>(size))) return;
  st->pending = true;
  st->pending_scene = scene.handle;
  st->pending_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

void OnInitPipeline(device* device, pipeline_layout layout, uint32_t subobject_count, const pipeline_subobject* subobjects,
                    pipeline pipeline) {
  const shader_desc* ps = nullptr;
  PipelineInfo info;
  for (uint32_t i = 0; i < subobject_count; ++i) {
    const auto& sub = subobjects[i];
    switch (sub.type) {
      case pipeline_subobject_type::pixel_shader:
        ps = static_cast<const shader_desc*>(sub.data);
        break;
      case pipeline_subobject_type::render_target_formats:
        info.rt_count = std::min<uint32_t>(sub.count, 8);
        for (uint32_t k = 0; k < info.rt_count; ++k) info.rt_formats[k] = static_cast<const format*>(sub.data)[k];
        break;
      case pipeline_subobject_type::depth_stencil_format:
        info.ds_format = *static_cast<const format*>(sub.data);
        break;
      case pipeline_subobject_type::blend_state:
        info.blend0 = static_cast<const blend_desc*>(sub.data)->blend_enable[0];
        break;
      case pipeline_subobject_type::depth_stencil_state: {
        const auto* ds = static_cast<const depth_stencil_desc*>(sub.data);
        info.depth_test = ds->depth_enable;
        info.depth_write = ds->depth_write_mask;
        break;
      }
      default:
        break;
    }
  }
  if (ps == nullptr || ps->code == nullptr || ps->code_size == 0) return;

  info.ps_hash = vegfix::Crc32(ps->code, ps->code_size);
  info.dev = device;
  if (g_config.dump_shaders) DumpShader(info.ps_hash, *ps, info);
  // The fog pass draws into the scene colour (one float target) and reads the integrated fog volumes.
  const bool fog = info.rt_count == 1 && info.rt_formats[0] == format::r16g16b16a16_float &&
                   vegfix::ContainsName(ps->code, ps->code_size, kFogResourceName);
  {
    std::unique_lock lock(g_pipeline_mutex);
    info.serial = g_next_serial++;
    g_pipelines[pipeline.handle] = info;
    if (fog) {
      g_fog_pipelines.insert(pipeline.handle);
      g_fog_count = g_fog_pipelines.size();
    }
  }
  {
    std::lock_guard lock(g_patch_mutex);
    if (g_unpatchable.count(info.ps_hash)) return;
  }

  // Pipelines that come out of an ID3D12PipelineLibrary cannot be modified in create_pipeline,
  // so every patched pipeline is a clone, built on the worker and bound in place of the original.
  auto recipe = std::make_shared<vegfix::PipelineRecipe>();
  if (!recipe->CopyFrom(layout, subobject_count, subobjects, g_blob_pool)) return;
  const shader_desc* copied_ps = recipe->pixel_shader();
  EnqueueJob({device, pipeline.handle, info.serial, info.ps_hash, recipe, copied_ps->code, copied_ps->code_size});
  StartWorker();
}

void OnDestroyPipeline(device* device, pipeline pipeline) {
  reshade::api::pipeline clone = {0};
  reshade::api::pipeline fringe = {0};
  {
    std::unique_lock lock(g_pipeline_mutex);
    g_pipelines.erase(pipeline.handle);
    if (g_fog_pipelines.erase(pipeline.handle) != 0) g_fog_count = g_fog_pipelines.size();
    if (auto it = g_sources.find(pipeline.handle); it != g_sources.end()) {
      --g_source_count[it->second.group];
      g_sources.erase(it);
    }
    if (auto it = g_clones.find(pipeline.handle); it != g_clones.end()) {
      clone = it->second;
      g_clones.erase(it);
      g_clone_count = g_clones.size();
    }
    if (auto it = g_fringes.find(pipeline.handle); it != g_fringes.end()) {
      fringe = it->second;
      g_fringes.erase(it);
      g_fringe_count = g_fringes.size();
    }
  }
  // The game only destroys a pipeline once the GPU is done with it, which covers the clone bound in its place and the
  // fringe drawn with it.
  if (clone.handle != 0) device->destroy_pipeline(clone);
  if (fringe.handle != 0) device->destroy_pipeline(fringe);
}

void OnBindPipeline(command_list* cmd_list, pipeline_stage stages, pipeline pipeline) {
  if (g_trace_state.load(std::memory_order_relaxed) == TraceState::kCapturing) {
    std::lock_guard lock(g_trace_mutex);
    g_trace_current[cmd_list] = pipeline.handle;
  }
  CmdState* const st = State(cmd_list);
  if (st != nullptr) {
    st->original = st->bound = pipeline.handle;
    st->fringe = 0;
    st->fog = false;
  }
  if ((stages & pipeline_stage::pixel_shader) == 0) return;
  if (g_clone_count.load(std::memory_order_relaxed) == 0 && g_fringe_count.load(std::memory_order_relaxed) == 0 &&
      g_fog_count.load(std::memory_order_relaxed) == 0)
    return;

  reshade::api::pipeline clone = {0};
  {
    std::shared_lock lock(g_pipeline_mutex);
    if (auto it = g_clones.find(pipeline.handle); it != g_clones.end()) clone = it->second;
    if (st != nullptr) {
      if (auto it = g_fringes.find(pipeline.handle); it != g_fringes.end()) st->fringe = it->second.handle;
      st->fog = g_fog_pipelines.count(pipeline.handle) != 0;
    }
  }
  if (clone.handle != 0 && g_active.load(std::memory_order_relaxed)) {
    cmd_list->bind_pipeline(stages, clone);
    if (st != nullptr) st->bound = clone.handle;
  }
}

void OnInitCommandList(command_list* cmd_list) { cmd_list->create_private_data<CmdState>(); }
void OnDestroyCommandList(command_list* cmd_list) { cmd_list->destroy_private_data<CmdState>(); }

void OnResetCommandList(command_list* cmd_list) {
  if (CmdState* const st = State(cmd_list)) *st = CmdState();
}

void OnCloseCommandList(command_list* cmd_list) { RunPendingComposite(cmd_list, State(cmd_list)); }

void OnBindPipelineStates(command_list* cmd_list, uint32_t count, const dynamic_state* states, const uint32_t* values) {
  CmdState* const st = State(cmd_list);
  if (st == nullptr) return;
  for (uint32_t i = 0; i < count; ++i) {
    if (states[i] != dynamic_state::blend_constant) continue;
    for (int c = 0; c < 4; ++c) st->blend_factor[c] = static_cast<float>((values[i] >> (8 * c)) & 0xFF) / 255.0f;
  }
}

void OnBindRenderTargets(command_list* cmd_list, uint32_t count, const resource_view* rtvs, resource_view dsv) {
  CmdState* const st = State(cmd_list);
  if (st == nullptr) return;
  st->rtv_count = std::min<uint32_t>(count, 8);
  for (uint32_t i = 0; i < st->rtv_count; ++i) st->rtvs[i] = rtvs[i].handle;
  st->dsv = dsv.handle;
}

void OnBindDescriptorTables(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                            const descriptor_table* tables, uint32_t, const uint32_t*) {
  CmdState* const st = State(cmd_list);
  if (st == nullptr) return;
  if ((stages & shader_stage::all_compute) != 0) {
    if (count == 0) st->ComputeArg(layout.handle, 0);  // SetComputeRootSignature
    for (uint32_t i = 0; i < count; ++i) {
      auto& arg = st->ComputeArg(layout.handle, first + i);
      arg = {};
      arg.kind = CmdState::RootArg::kTable;
      arg.value = tables[i].handle;
    }
  }
  if ((stages & shader_stage::all_graphics) != 0) {
    if (layout.handle != st->graphics_layout) {
      st->graphics_layout = layout.handle;
      st->graphics_tables.clear();
    }
    if (count != 0 && st->graphics_tables.size() < first + count) st->graphics_tables.resize(first + count);
    for (uint32_t i = 0; i < count; ++i) st->graphics_tables[first + i] = tables[i].handle;
  }
}

void OnPushDescriptors(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& update) {
  if ((stages & shader_stage::all_compute) == 0 || update.count == 0 || update.descriptors == nullptr) return;
  CmdState* const st = State(cmd_list);
  if (st == nullptr) return;
  auto& arg = st->ComputeArg(layout.handle, param);
  arg = {};
  // Root descriptors, as ReShade 6.8 reports them: constant buffers as a buffer range, shader resources and unordered
  // access views as the GPU virtual address.
  switch (update.type) {
    case descriptor_type::constant_buffer: {
      const buffer_range& range = static_cast<const buffer_range*>(update.descriptors)[0];
      if (auto* const buffer = reinterpret_cast<ID3D12Resource*>(range.buffer.handle)) {
        arg.kind = CmdState::RootArg::kCbv;
        arg.value = buffer->GetGPUVirtualAddress() + range.offset;
      }
      break;
    }
    case descriptor_type::buffer_shader_resource_view:
    case descriptor_type::acceleration_structure:
      arg.kind = CmdState::RootArg::kSrv;
      arg.value = *static_cast<const uint64_t*>(update.descriptors);
      break;
    case descriptor_type::buffer_unordered_access_view:
      arg.kind = CmdState::RootArg::kUav;
      arg.value = *static_cast<const uint64_t*>(update.descriptors);
      break;
    default:
      break;
  }
}

void OnPushConstants(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
  if ((stages & shader_stage::all_compute) == 0 || count == 0) return;
  CmdState* const st = State(cmd_list);
  if (st == nullptr) return;
  auto& arg = st->ComputeArg(layout.handle, param);
  if (arg.kind != CmdState::RootArg::kConstants) arg = {};
  arg.kind = CmdState::RootArg::kConstants;
  if (arg.constants.size() < first + count) {
    arg.constants.resize(first + count);
    arg.constants_set.resize(first + count);
  }
  std::memcpy(arg.constants.data() + first, values, count * sizeof(uint32_t));
  std::fill_n(arg.constants_set.begin() + first, count, uint8_t{1});
}

// Follows the scene colour's state between the fog draw and the composite (the event comes after the barrier).
void OnBarrier(command_list* cmd_list, uint32_t count, const resource* resources, const resource_usage*, const resource_usage* new_states) {
  CmdState* const st = State(cmd_list);
  if (st == nullptr || !st->pending) return;
  for (uint32_t i = 0; i < count; ++i) {
    if (resources[i].handle != st->pending_scene) continue;
    const uint32_t state = static_cast<uint32_t>(new_states[i]);
    st->pending_state = (state & 0x80000000u) != 0 ? 0 : state;  // ReShade's "general" is D3D12's COMMON
  }
}

bool OnDraw(command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
  CountTraceDraw(cmd_list);
  CmdState* const st = State(cmd_list);
  RunPendingComposite(cmd_list, st);
  DrawFringe(cmd_list, st, [&](ID3D12GraphicsCommandList* native) {
    native->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
  });
  CheckFog(cmd_list, st);
  return false;
}

bool OnDrawIndexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
                   uint32_t first_instance) {
  CountTraceDraw(cmd_list);
  CmdState* const st = State(cmd_list);
  RunPendingComposite(cmd_list, st);
  DrawFringe(cmd_list, st, [&](ID3D12GraphicsCommandList* native) {
    native->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
  });
  CheckFog(cmd_list, st);
  return false;
}

bool OnDrawIndirect(command_list* cmd_list, indirect_command type, resource, uint64_t, uint32_t, uint32_t) {
  if (type != indirect_command::dispatch) CountTraceDraw(cmd_list);
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}

// Any other command after the fog draw runs the pending composite first.
bool OnDispatch(command_list* cmd_list, uint32_t, uint32_t, uint32_t) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnCopyResource(command_list* cmd_list, resource, resource) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnCopyBufferRegion(command_list* cmd_list, resource, uint64_t, resource, uint64_t, uint64_t) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnCopyBufferToTexture(command_list* cmd_list, resource, uint64_t, uint32_t, uint32_t, resource, uint32_t, const subresource_box*) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnCopyTextureRegion(command_list* cmd_list, resource, uint32_t, const subresource_box*, resource, uint32_t, const subresource_box*,
                         filter_mode) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnCopyTextureToBuffer(command_list* cmd_list, resource, uint32_t, const subresource_box*, resource, uint64_t, uint32_t, uint32_t) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnResolveTextureRegion(command_list* cmd_list, resource, uint32_t, const subresource_box*, resource, uint32_t, uint32_t, uint32_t,
                            uint32_t, format) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnClearDepthStencil(command_list* cmd_list, resource_view, const float*, const uint8_t*, uint32_t, const rect*) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnClearRenderTarget(command_list* cmd_list, resource_view, const float[4], uint32_t, const rect*) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnClearUavUint(command_list* cmd_list, resource_view, const uint32_t[4], uint32_t, const rect*) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}
bool OnClearUavFloat(command_list* cmd_list, resource_view, const float[4], uint32_t, const rect*) {
  RunPendingComposite(cmd_list, State(cmd_list));
  return false;
}

void OnInitDevice(device* device) {
  ++g_device_count;
  // Both add-ons would patch the same shaders.
  for (const wchar_t* old : {L"FF7RebirthVegetationFix.addon64", L"FF7RebirthHairFix.addon64"}) {
    if (GetModuleHandleW(old) == nullptr || g_old_version_loaded != nullptr) continue;
    g_old_version_loaded = old;
    const std::string name = fs::path(old).stem().string();
    Log(reshade::log::level::warning, name + ".addon64, this add-on's former name, is installed too. Delete it, " + name +
                                          ".ini and the " + name + " folder.");
  }
  if (g_soft == nullptr && device->get_api() == device_api::d3d12) {
    auto* soft = new vegfix::SoftEdges();
    if (soft->Initialize(reinterpret_cast<ID3D12Device*>(device->get_native()), &g_soft_error)) {
      g_soft = soft;
    } else {
      Log(reshade::log::level::error, "Soft edges disabled: " + g_soft_error);
      delete soft;
    }
  }
  StartWorker();
}

// Forgets the pipelines of a device that goes away while others stay, and destroys its clones.
void ForgetDevice(device* device) {
  std::vector<reshade::api::pipeline> clones;
  {
    std::unique_lock lock(g_pipeline_mutex);
    std::erase_if(g_pipelines, [device](const auto& entry) { return entry.second.dev == device; });
    for (auto it = g_sources.begin(); it != g_sources.end();) {
      if (it->second.dev != device) {
        ++it;
        continue;
      }
      if (auto clone = g_clones.find(it->first); clone != g_clones.end()) {
        clones.push_back(clone->second);
        g_clones.erase(clone);
      }
      if (auto fringe = g_fringes.find(it->first); fringe != g_fringes.end()) {
        clones.push_back(fringe->second);
        g_fringes.erase(fringe);
      }
      --g_source_count[it->second.group];
      it = g_sources.erase(it);
    }
    g_clone_count = g_clones.size();
    g_fringe_count = g_fringes.size();
  }
  // Its queued jobs would now fail their pipeline check anyway. Drop them, and wait for the job in progress, which
  // may be using the device.
  {
    std::unique_lock lock(g_job_mutex);
    std::erase_if(g_jobs, [device](const Job& job) { return job.dev == device; });
    g_queue_size = g_jobs.size();
    g_job_cv.wait(lock, [device] { return g_job_device != device; });
  }
  for (auto clone : clones) device->destroy_pipeline(clone);
  DestroyRetiredPipelines(true, device);
}

void OnDestroyDevice(device* device) {
  // Shader Injector, for one, opens and closes a device of its own while the game's is alive. With one GPU that is
  // the game's device again, but on a PC with two it can be a separate device, which must not take the game's
  // pipelines with it.
  if (--g_device_count > 0) {
    ForgetDevice(device);
    return;
  }
  StopWorker();
  std::vector<reshade::api::pipeline> clones;
  {
    std::unique_lock lock(g_pipeline_mutex);
    for (const auto& [original, clone] : g_clones) clones.push_back(clone);
    for (const auto& [original, fringe] : g_fringes) clones.push_back(fringe);
    g_clones.clear();
    g_fringes.clear();
    g_fog_pipelines.clear();
    g_fog_count = 0;
    g_sources.clear();
    g_pipelines.clear();
    for (auto& count : g_source_count) count = 0;
    g_clone_count = 0;
    g_fringe_count = 0;
  }
  for (auto clone : clones) device->destroy_pipeline(clone);
  DestroyRetiredPipelines(true);
  delete g_soft;
  g_soft = nullptr;
}

bool KeyPressed(int vk, bool& was_down) {
  if (vk == 0) return false;
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool pressed = down && !was_down;
  was_down = down;
  return pressed;
}

// Once per frame: clears the coverage if no composite did (on the queue, before the next frame's commands), follows
// the main view's size, and releases textures replaced by a resize.
void SoftEdgesHousekeeping(command_queue* queue) {
  static bool logged_blend = false;
  static uint32_t frames_without_blend = 0;
  const uint32_t fringe_draws = g_fringe_draws.exchange(0);
  const uint32_t composites = g_composites.exchange(0);
  g_fringe_draws_shown = fringe_draws;
  g_composites_shown = composites;
  if (composites != 0 && !logged_blend) {
    logged_blend = true;
    Log(reshade::log::level::info, "Soft edges: " + std::to_string(fringe_draws) + " fringe draws, blended after the fog pass (" +
                                       std::to_string(g_soft->width()) + "x" + std::to_string(g_soft->height()) + ").");
  }
  frames_without_blend = fringe_draws != 0 && composites == 0 ? frames_without_blend + 1 : 0;
  if (frames_without_blend == 600)
    Log(reshade::log::level::warning, "Soft edges: fringe drawn for 600 frames without a fog pass to blend it after.");
  const uint64_t largest = g_frame_largest.exchange(0);
  if (g_soft == nullptr) return;
  const uint64_t frame = g_frame.load();
  if (g_coverage_dirty && g_composite_frame.load() != frame) {
    // Through ReShade's API, which then knows its immediate list has something to submit.
    command_list* const immediate = queue->get_immediate_command_list();
    if (immediate != nullptr && g_soft->width() != 0) {
      for (uint32_t i = 0; i < vegfix::SoftEdges::kTargets; ++i)
        immediate->clear_render_target_view({g_soft->target_rtv(i).ptr}, vegfix::SoftEdges::ClearColor(i));
      g_soft->MarkCleared();
      queue->flush_immediate_command_list();
      g_coverage_dirty = false;
    }
  }
  // A smaller main view (a lower render resolution) for two seconds: shrink the coverage to it.
  const uint32_t width = static_cast<uint32_t>(largest >> 32), height = static_cast<uint32_t>(largest);
  if (largest != 0 && !g_soft->Matches(width, height)) {
    if (++g_frames_smaller > 120) {
      g_frames_smaller = 0;
      g_soft->EnsureSize(width, height, frame);
    }
  } else {
    g_frames_smaller = 0;
  }
  g_soft->ReleaseRetired(frame, kRetireFrames);
}

void OnPresent(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
  static bool toggle_down = false;
  static bool trace_down = false;
  static bool soft_down = false;
  SoftEdgesHousekeeping(queue);
  ++g_frame;
  if (g_first_present_ms.load(std::memory_order_relaxed) == 0) g_first_present_ms = NowMs();
  if (KeyPressed(g_config.toggle_key, toggle_down)) g_active = !g_active;
  if (KeyPressed(g_config.soft_key, soft_down)) {
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
      g_soft_debug = (g_soft_debug.load() + 1) % 4;
    else
      g_soft_active = !g_soft_active;
  }
  if (KeyPressed(g_config.trace_key, trace_down) && g_trace_state == TraceState::kIdle) g_trace_state = TraceState::kArmed;

  const int64_t apply_at = g_options_apply_at_ms.load();
  if (apply_at != 0 && NowMs() >= apply_at) {
    g_options_apply_at_ms = 0;
    for (int group = 0; group < kGroupCount; ++group) ApplyOptions(static_cast<Group>(group), EffectiveOptions(group));
    ApplyFringeOptions(EffectiveFringeOptions());
  }
  DestroyRetiredPipelines(false);

  switch (g_trace_state.load()) {
    case TraceState::kArmed:
      g_trace_state = TraceState::kCapturing;
      break;
    case TraceState::kCapturing:
      g_trace_state = TraceState::kIdle;
      WriteTrace();
      break;
    default:
      break;
  }
}

void OnSettingsChanged() {
  for (int group = 0; group < kGroupCount; ++group) {
    auto& options = g_config.groups[group];
    options.threshold = Round2(options.threshold);
    options.dither_strength = Round2(options.dither_strength);
    WriteIniFloat(kThresholdKeys[group], options.threshold);
    WriteIniFloat(kDitherKeys[group], options.dither_strength);
  }
  g_config.soft_softness = Round2(g_config.soft_softness);
  WriteIni("SoftEdges", g_config.soft_edges ? L"1" : L"0");
  WriteIniFloat("SoftEdgeSoftness", g_config.soft_softness);
  g_options_apply_at_ms = NowMs() + 300;
}

void OnOverlay(effect_runtime*) {
  if (g_old_version_loaded != nullptr)
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Delete %s (this add-on's former name) and restart.",
                       fs::path(g_old_version_loaded).string().c_str());
  bool active = g_active;
  if (ImGui::Checkbox("Enabled", &active)) {
    g_active = active;
    g_config.enabled = active;
    WriteIni("Enabled", active ? L"1" : L"0");
  }
  ImGui::SetItemTooltip("Switches between these settings and the game's original shaders instantly.");

  static constexpr const char* kHeadings[kGroupCount] = {"Characters: hair, fur, clothing",
                                                         "Scenery: trees, plants, grass and everything else"};
  bool changed = false;
  for (int group : {kScenery, kCharacters}) {
    auto& options = g_config.groups[group];
    ImGui::PushID(group);
    ImGui::SeparatorText(kHeadings[group]);
    const bool soft = group == kCharacters && g_config.soft_edges;
    if (group == kCharacters) {
      if (ImGui::Checkbox("Soft edges", &g_config.soft_edges)) {
        g_soft_active = g_config.soft_edges;
        changed = true;
      }
      ImGui::SetItemTooltip(
          "Draws the solid part of hair and fur cleanly and blends the thin, see-through parts over the scene\n"
          "instead of dithering them or cutting them away.");
    }
    changed |= ImGui::SliderFloat(soft ? "Solid above" : "Opacity threshold", &options.threshold, 0.05f, 0.95f, "%.2f");
    ImGui::SetItemTooltip(soft ? "Parts more opaque than this are drawn solid; the rest is blended in. Default %.2f."
                               : "Pixels more opaque than this are drawn.\nLower = fuller hair and leaves, higher = thinner. Default %.2f.",
                          kDefaults[group].threshold);
    if (soft) {
      changed |= ImGui::SliderFloat("Edge softness", &g_config.soft_softness, 0.05f, 1.0f, "%.2f");
      ImGui::SetItemTooltip(
          "How much of the thin, see-through part is blended in, as a share of the threshold. Higher = softer, wispier\n"
          "edges, lower = closer to a clean cutout. Default 0.70.");
    } else {
      changed |= ImGui::SliderFloat("Dither strength", &options.dither_strength, 0.0f, 1.0f, "%.2f");
      ImGui::SetItemTooltip("0 = no dithering (clean alpha test). 1 = the game's original dithering. Default %.2f.",
                            kDefaults[group].dither_strength);
      ImGui::BeginDisabled(options.original());
      if (ImGui::Button("Game's dithering")) {
        options = {0.5f, 1.0f};
        changed = true;
      }
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("Threshold 0.50 with dither strength 1.00 is the game's own test; these shaders are then left alone.");
      ImGui::SameLine();
    }
    ImGui::TextDisabled("%zu dithered pipelines", g_source_count[group].load());
    if (soft) {
      if (g_soft == nullptr)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Soft edges unavailable: %s",
                           g_soft_error.empty() ? "no D3D12 device" : g_soft_error.c_str());
      else
        ImGui::TextDisabled("%zu soft-edge pipelines; last frame %u fringe draws, %u blends%s", g_fringe_count.load(),
                            g_fringe_draws_shown.load(), g_composites_shown.load(), g_soft_active ? "" : " (off: hotkey)");
    }
    ImGui::PopID();
  }
  ImGui::Spacing();
  if (ImGui::Button("Reset to defaults")) {
    for (int group = 0; group < kGroupCount; ++group) g_config.groups[group] = kDefaults[group];
    g_config.soft_edges = true;
    g_config.soft_softness = 0.7f;
    changed = true;
  }
  if (changed) OnSettingsChanged();

  ImGui::Spacing();
  if (g_patcher_failed) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Patching disabled: %s", g_patcher_error.c_str());
  size_t dithered = 0;
  {
    std::lock_guard lock(g_patch_mutex);
    dithered = g_dithered.size();
  }
  ImGui::Text("Dithered shaders found: %zu   patched pipelines live: %zu", dithered, g_clone_count.load());
  if (g_queue_size != 0) ImGui::Text("Working... %zu pipelines queued", g_queue_size.load());
  if (g_patch_failures != 0 || g_clone_failures != 0 || g_fringe_failures != 0)
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Failures: %zu patch, %zu pipeline, %zu soft edge (see ReShade.log)",
                       g_patch_failures.load(), g_clone_failures.load(), g_fringe_failures.load());

  if (ImGui::CollapsingHeader("Developer")) {
    ImGui::Text("Shaders analysed this session: %zu", g_shaders_checked.load());
    bool dump = g_config.dump_shaders;
    if (ImGui::Checkbox("Dump pixel shaders (applies to new pipelines)", &dump)) {
      g_config.dump_shaders = dump;
      WriteIni("DumpShaders", dump ? L"1" : L"0");
    }
    ImGui::Text("Shaders dumped this session: %zu", g_dump_count.load());
    if (ImGui::Button("Trace next frame") && g_trace_state == TraceState::kIdle) g_trace_state = TraceState::kArmed;
    if (!g_last_trace_file.empty()) ImGui::Text("Last trace: %s", g_last_trace_file.c_str());
    int debug = static_cast<int>(g_soft_debug.load());
    ImGui::TextUnformatted("Soft edges view:");
    ImGui::SameLine();
    bool view_changed = ImGui::RadioButton("Normal", &debug, 0);
    ImGui::SameLine();
    view_changed |= ImGui::RadioButton("Edge opacity", &debug, 1);
    ImGui::SameLine();
    view_changed |= ImGui::RadioButton("Edge colour", &debug, 2);
    ImGui::SameLine();
    view_changed |= ImGui::RadioButton("Solid parts", &debug, 3);
    if (view_changed) g_soft_debug = static_cast<uint32_t>(debug);
    float tolerance = g_depth_tolerance.load();
    if (ImGui::SliderFloat("Edge colour depth tolerance", &tolerance, 0.01f, 0.5f, "%.2f")) g_depth_tolerance = tolerance;
    ImGui::SetItemTooltip("How far a solid pixel may be from a blended edge, relative to the camera distance, to lend it its colour.");
  }
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  const fs::path dir = fs::path(path).parent_path();
  g_data_dir = dir / "FF7RebirthVegetationAndHairFix";
  g_ini_path = dir / "FF7RebirthVegetationAndHairFix.ini";
  ImportOldSettings(dir);
  LoadConfig();
  LoadOverrides();
  OpenCache();
  char summary[256];
  snprintf(summary, sizeof(summary),
           "Scenery: threshold %.2f, dither strength %.2f. Characters: threshold %.2f, dither strength %.2f, soft edges %s "
           "(edge softness %.2f). %zu cached non-dithered shaders, %zu loose overrides.",
           g_options[kScenery].threshold, g_options[kScenery].dither_strength, g_options[kCharacters].threshold,
           g_options[kCharacters].dither_strength, g_config.soft_edges ? "on" : "off", g_config.soft_softness,
           g_unpatchable.size(), g_overrides.size());
  Log(reshade::log::level::info, summary);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::register_event<reshade::addon_event::draw>(OnDraw);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(OnDrawIndirect);
      reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
      reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::register_event<reshade::addon_event::close_command_list>(OnCloseCommandList);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::register_event<reshade::addon_event::bind_pipeline_states>(OnBindPipelineStates);
      reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OnBindDescriptorTables);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::push_constants>(OnPushConstants);
      reshade::register_event<reshade::addon_event::barrier>(OnBarrier);
      reshade::register_event<reshade::addon_event::dispatch>(OnDispatch);
      reshade::register_event<reshade::addon_event::copy_resource>(OnCopyResource);
      reshade::register_event<reshade::addon_event::copy_buffer_region>(OnCopyBufferRegion);
      reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(OnCopyBufferToTexture);
      reshade::register_event<reshade::addon_event::copy_texture_region>(OnCopyTextureRegion);
      reshade::register_event<reshade::addon_event::copy_texture_to_buffer>(OnCopyTextureToBuffer);
      reshade::register_event<reshade::addon_event::resolve_texture_region>(OnResolveTextureRegion);
      reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(OnClearDepthStencil);
      reshade::register_event<reshade::addon_event::clear_render_target_view>(OnClearRenderTarget);
      reshade::register_event<reshade::addon_event::clear_unordered_access_view_uint>(OnClearUavUint);
      reshade::register_event<reshade::addon_event::clear_unordered_access_view_float>(OnClearUavFloat);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay(nullptr, OnOverlay);
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
