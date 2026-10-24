// Soft edges for dithered character materials (hair, fur, eyelashes...).
// SPDX-License-Identifier: MIT
//
// The game's passes draw such a material's solid core as a plain alpha test, so the G-buffer only ever holds whole,
// lit hair pixels. A fringe pass then draws the same geometry once more (depth test against the full depth buffer, no
// depth write) into four targets:
//   coverage        r  1 where the visible surface is a core fragment (MAX)
//                   g  device depth of the nearest fringe fragment (MAX; reversed Z, so larger = nearer)
//                   b  device depth of the visible core fragment (MAX)
//                   a  opacity of the strongest fringe fragment in front of the visible surface (MAX). Like the
//                      game's dithering, where every layer at a pixel is tested against the same noise value, overlapping
//                      layers cover as much as the strongest of them (not 1 - product(1 - opacity)).
//   core albedo     rgb  base colour of the visible core fragment, a = its draw's ID
//   fringe albedo   rgb  sum of opacity * base colour of the fringe fragments, a = sum of their opacities
//   fringe ID       r  the draw ID of the last fringe fragment drawn here
// After lighting and fog, these passes blend the fringe over the lit scene. A fringe has no lighting of its own: it
// takes its base colour times the lighting (lit colour / base colour) of the nearest core pixels of the same draw at a
// similar depth, found in a small pyramid. A fringe whose draw has no core nearby (eyebrow hairs, for one) is left out.

cbuffer Params : register(b0) {
  uint2 g_size;            // size of the level being written
  uint2 g_source_size;     // size of the level being read (downsample)
  float g_depth_tolerance; // relative depth difference for a core pixel to light a fringe
  uint g_debug;            // 0 = off, 1 = fringe opacity, 2 = fringe colour, 3 = core pixels
  uint2 g_pad;
};

static const uint kLevels = 6;
static const float kMaxLighting = 64.0;

Texture2D<float4> g_coverage : register(t0);
Texture2D<float4> g_scene : register(t1);
Texture2D<float4> g_core_albedo : register(t2);
Texture2D<float4> g_fringe_albedo : register(t3);
Texture2D<float> g_fringe_id : register(t4);
Texture2D<float4> g_core[kLevels] : register(t5);   // rgb = lighting (lit colour / base colour), a = device depth (0 = none)
Texture2D<float> g_core_id[kLevels] : register(t11);  // draw ID of the core texel
RWTexture2D<float4> g_out : register(u0);
RWTexture2D<float> g_out_id : register(u1);

// A core sample at depth `core` can light a fringe at depth `fringe` when the two are within the tolerance. Device
// depth is proportional to 1 / view depth, so the ratio is the ratio of view depths.
bool SimilarDepth(float core, float fringe) {
  return core > 0.0 && abs(core - fringe) <= g_depth_tolerance * max(core, fringe);
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Level 0: the lighting and draw ID of every pixel whose visible surface is a core fragment.
[numthreads(8, 8, 1)]
void CoreColors(uint2 p : SV_DispatchThreadID) {
  if (any(p >= g_size)) return;
  const float4 coverage = g_coverage[p];
  if (coverage.r < 0.5) {
    g_out[p] = 0.0;
    g_out_id[p] = 0.0;
    return;
  }
  // Dark channels of a coloured base colour would make huge ratios: keep each at least a third of its luminance.
  const float4 albedo = g_core_albedo[p];
  const float3 floor_albedo = max(albedo.rgb, max(Luminance(albedo.rgb) * (1.0 / 3.0), 0.01));
  g_out[p] = float4(min(g_scene[p].rgb / floor_albedo, kMaxLighting), coverage.b);
  g_out_id[p] = albedo.a;
}

// Next level: of the four source texels, the nearest core, and the average lighting of those of its draw at a similar
// depth.
[numthreads(8, 8, 1)]
void Downsample(uint2 p : SV_DispatchThreadID) {
  if (any(p >= g_size)) return;
  float4 s[4];
  float id[4];
  float nearest = 0.0, nearest_id = 0.0;
  [unroll] for (uint i = 0; i < 4; ++i) {
    const uint2 q = min(p * 2 + uint2(i & 1, i >> 1), g_source_size - 1);
    s[i] = g_core[0][q];
    id[i] = g_core_id[0][q];
    if (s[i].a > nearest) {
      nearest = s[i].a;
      nearest_id = id[i];
    }
  }
  float3 sum = 0.0;
  float count = 0.0;
  [unroll] for (uint k = 0; k < 4; ++k) {
    if (id[k] == nearest_id && SimilarDepth(s[k].a, nearest)) {
      sum += s[k].rgb;
      count += 1.0;
    }
  }
  g_out[p] = count > 0.0 ? float4(sum / count, nearest) : 0.0;
  g_out_id[p] = count > 0.0 ? nearest_id : 0.0;
}

// Blends the fringe over the scene colour: scene = lerp(scene, fringe colour, fringe opacity).
[numthreads(8, 8, 1)]
void Composite(uint2 p : SV_DispatchThreadID) {
  if (any(p >= g_size)) return;
  const float4 coverage = g_coverage[p];
  const float4 scene = g_out[p];
  if (g_debug == 3) {
    g_out[p] = float4(coverage.r.xxx, scene.a);
    return;
  }
  const float transmittance = 1.0 - coverage.a;
  const float depth = coverage.g;
  if (transmittance >= 0.998 || depth <= 0.0) {
    if (g_debug != 0) g_out[p] = float4(0.0.xxx, scene.a);
    return;
  }

  // From the finest level up, a 3x3 neighbourhood of core texels of the fringe's draw at a similar depth, weighted by
  // distance.
  const float id = g_fringe_id[p];
  float3 lighting = 0.0;
  float weight = 0.0;
  [unroll] for (uint level = 0; level < kLevels; ++level) {
    const int2 size = int2(max(g_size >> level, uint2(1, 1)));
    const float2 center = (float2(p) + 0.5) / float(1u << level) - 0.5;
    const int2 base = int2(floor(center + 0.5));
    [unroll] for (int y = -1; y <= 1; ++y) {
      [unroll] for (int x = -1; x <= 1; ++x) {
        const int2 q = clamp(base + int2(x, y), int2(0, 0), size - 1);
        const float4 core = g_core[level][q];
        if (g_core_id[level][q] != id || !SimilarDepth(core.a, depth)) continue;
        const float2 d = float2(q) - center;
        const float w = 1.0 / (1.0 + dot(d, d));
        lighting += core.rgb * w;
        weight += w;
      }
    }
    if (weight > 0.0) break;
  }

  if (g_debug == 1) {
    g_out[p] = float4((1.0 - transmittance).xxx, scene.a);
    return;
  }
  if (weight <= 0.0) {  // no lit core of its draw nearby: leave the fringe out
    if (g_debug == 2) g_out[p] = float4(0.0.xxx, scene.a);
    return;
  }
  const float4 fringe = g_fringe_albedo[p];
  const float3 color = fringe.rgb / max(fringe.a, 1e-4) * (lighting / weight);
  if (g_debug == 2) {
    g_out[p] = float4(color, scene.a);
    return;
  }
  g_out[p] = float4(lerp(color, scene.rgb, transmittance), scene.a);
}
