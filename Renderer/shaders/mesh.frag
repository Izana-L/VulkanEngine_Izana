#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "common/frame_set.glsl"
#include "common/scene_data.glsl"
#include "common/bindless.glsl"
#include "common/cluster_data.glsl"

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in vec4 frag_color;
layout(location = 4) flat in uint frag_material_index;

layout(location = 0) out vec4 out_color;

// Flat provisional ambient: replaced by IBL once PBR exists.
const vec3 AMBIENT = vec3(0.08, 0.08, 0.10);

// Provisional specular term (Blinn-Phong), the consumer of
// frame.camera_position until the PBR pass replaces it.
const float SPECULAR_STRENGTH = 0.15;
const float SPECULAR_POWER = 32.0;

// Weight of a debug view over the lit color: the geometry stays readable
// under the overlay.
const float DEBUG_VIEW_OPACITY = 0.8;

// Adds the diffuse and specular contribution of one light to the
// accumulators. Shared by both light selection paths, so the clustered
// path and the brute force reference add the same terms in the same order:
// a light left out of a cluster lies entirely beyond its range for every
// point of the cluster, where its attenuation is exactly zero.
void Accumulate_light(Light light, vec3 N, vec3 V, inout vec3 diffuse, inout vec3 specular)
{
    vec3  L           = vec3(0.0);
    float attenuation = 1.0;

    if (light.type == LIGHT_TYPE_DIRECTIONAL)
    {
        // Directional: the packet stores the direction the light POINTS
        // TO, and the vector TOWARDS the light is needed here.
        L = normalize(-light.position_or_direction);
    }
    else
    {
        // Point and spot: position_or_direction is a position.
        vec3  to_light = light.position_or_direction - frag_world_pos;
        float dist     = length(to_light);

        L = to_light / max(dist, 1e-4);

        // Smooth falloff that reaches 0 exactly at range, so a light
        // does not cut off abruptly at the edge of its radius.
        float falloff = clamp(1.0 - dist / max(light.range, 1e-4), 0.0, 1.0);
        attenuation   = falloff * falloff;

        if (light.type == LIGHT_TYPE_SPOT)
        {
            // Spot cone: angle between the cone axis and the fragment.
            // inner = full intensity, outer = 0.
            float cos_angle = dot(normalize(light.spot_direction), -L);
            float cos_inner = cos(light.inner_angle);
            float cos_outer = cos(light.outer_angle);

            attenuation *= smoothstep(cos_outer, cos_inner, cos_angle);
        }
    }

    float n_dot_l = max(dot(N, L), 0.0);

    vec3 radiance = light.color * light.intensity * attenuation;

    diffuse += radiance * n_dot_l;

    if (n_dot_l > 0.0)
    {
        vec3  H = normalize(L + V);
        float n_dot_h = max(dot(N, H), 0.0);
        specular += radiance * SPECULAR_STRENGTH * pow(n_dot_h, SPECULAR_POWER);
    }
}

// Heatmap gradient for t in [0, 1]: black, blue, cyan, green, yellow, red.
vec3 Heatmap_color(float t)
{
    const vec3 stops[6] = vec3[6](vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0),
                                  vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0));

    const float position = clamp(t, 0.0, 1.0) * 5.0;
    const int   stop     = min(int(position), 4);

    return mix(stops[stop], stops[stop + 1], position - float(stop));
}

// Distinct, stable color for an integer id (integer hash).
vec3 Id_color(uint id)
{
    uint h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;

    return vec3(float(h & 0xFFu), float((h >> 8u) & 0xFFu), float((h >> 16u) & 0xFFu)) / 255.0 * 0.8 + 0.2;
}

void main()
{
    vec3 N = normalize(frag_world_normal);
    vec3 V = normalize(frame.camera_position - frag_world_pos);

    // Base color: vertex tint * material tint * albedo texture, read with
    // the sampler preset the material selected. Every material carries a
    // written texture slot: an untextured one samples the white default
    // texture, which leaves the tint unchanged. Sample_bindless applies
    // nonuniformEXT: the material, and so the texture index, may differ
    // between invocations of one subgroup once draws are merged.
    Material material = material_buffer.materials[frag_material_index];

    vec4 base = frag_color * material.base_color * Sample_bindless(material.albedo_texture_index, material.albedo_sampler_index, frag_uv);

    // Cluster of the fragment: screen tile from gl_FragCoord, depth slice
    // from the view space distance. The distance comes from the world
    // position the vertex shader already interpolates, not from
    // gl_FragCoord.z, which under the infinite reverse-Z projection holds
    // near / distance.
    const float view_depth    = -(frame.view * vec4(frag_world_pos, 1.0)).z;
    const uvec3 cluster       = Cluster_coordinates(gl_FragCoord.xy, view_depth);
    const uvec2 cluster_range = cluster_grid.ranges[Cluster_index(cluster)];
    const uint  cluster_count = cluster_range.y & CLUSTER_COUNT_MASK;

    vec3 diffuse  = vec3(0.0);
    vec3 specular = vec3(0.0);

    if (frame.debug_params.x == LIGHT_CULLING_BRUTE_FORCE)
    {
        // Reference path: every light of the buffer.
        for (int i = 0; i < frame.light_count; ++i)
            Accumulate_light(light_buffer.lights[i], N, V, diffuse, specular);
    }
    else
    {
        // Directional lights reach every fragment and are never clustered:
        // they occupy the start of the buffer.
        for (uint i = 0u; i < frame.cluster_grid.w; ++i)
            Accumulate_light(light_buffer.lights[i], N, V, diffuse, specular);

        // Point and spot lights whose range reaches the cluster, in
        // increasing buffer order.
        for (uint i = 0u; i < cluster_count; ++i)
        {
            const uint light_index = cluster_light_indices.indices[cluster_range.x + i];
            Accumulate_light(light_buffer.lights[light_index], N, V, diffuse, specular);
        }
    }

    vec3 color = base.rgb * (AMBIENT + diffuse) + specular;

    // ── Debug views of the cluster grid ───────────────────────
    const uint debug_view = frame.debug_params.y;

    if (debug_view == CLUSTER_VIEW_LIGHT_HEATMAP)
    {
        // Lights of the cluster, blue (few) to red (debug_params.z or
        // more). Magenta: the cluster lost lights to a full index list.
        const float max_lights = float(max(frame.debug_params.z, 1u));
        const vec3  heat = ((cluster_range.y & CLUSTER_OVERFLOW_BIT) != 0u)
                         ? vec3(1.0, 0.0, 1.0)
                         : Heatmap_color(float(cluster_count) / max_lights);

        color = mix(color, heat, DEBUG_VIEW_OPACITY);
    }
    else if (debug_view == CLUSTER_VIEW_DEPTH_SLICES)
    {
        // One color per slice: bands that get thinner towards the camera.
        color = mix(color, Id_color(cluster.z), DEBUG_VIEW_OPACITY);
    }
    else if (debug_view == CLUSTER_VIEW_CLUSTERS)
    {
        // One color per cluster: the tile grid, fixed on screen while the
        // camera moves, combined with the slices.
        color = mix(color, Id_color(Cluster_index(cluster)), DEBUG_VIEW_OPACITY);
    }

    out_color = vec4(color, base.a);
}
