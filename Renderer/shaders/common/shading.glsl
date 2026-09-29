#ifndef SHADING_GLSL
#define SHADING_GLSL

// Shading of a mesh fragment: material base color, clustered lighting and
// the cluster debug views. Shared by the opaque pass (mesh.frag) and the
// transparent pass (mesh_oit.frag), so opaque and transparent surfaces
// compute exactly the same color and only differ in where it goes.
//
// Requires, included before this file:
//   frame_set.glsl    - Frame_UBO, light buffer;
//   scene_data.glsl   - material table;
//   bindless.glsl     - Sample_bindless, with GL_EXT_nonuniform_qualifier
//                       enabled by the including shader;
//   cluster_data.glsl - cluster grid and light index list.
// Fragment stage only: the cluster of a fragment comes from gl_FragCoord.

// Flat provisional ambient: replaced by IBL once PBR exists.
const vec3 AMBIENT = vec3(0.08, 0.08, 0.10);

// Provisional specular term (Blinn-Phong), the consumer of
// frame.camera_position until the PBR pass replaces it.
const float SPECULAR_STRENGTH = 0.15;
const float SPECULAR_POWER = 32.0;

// Weight of a debug view over the lit color: the geometry stays readable
// under the overlay.
const float DEBUG_VIEW_OPACITY = 0.8;

// Base color of a fragment: vertex tint * material tint * albedo texture,
// read with the sampler preset the material selected. Every material
// carries a written texture slot: an untextured one samples the white
// default texture, which leaves the tint unchanged. Sample_bindless
// applies nonuniformEXT: the material, and so the texture index, may
// differ between invocations of one subgroup once draws are merged.
// The alpha is returned as computed; its meaning depends on the material's
// alpha mode (Material_opaque_alpha, and the transparent pass for Blend).
vec4 Material_base_color(uint _material_index, vec4 _vertex_color, vec2 _uv)
{
    const Material material = material_buffer.materials[_material_index];

    return _vertex_color * material.base_color * Sample_bindless(material.albedo_texture_index, material.albedo_sampler_index, _uv);
}

// Alpha handling of the opaque pass. Returns false when the fragment must
// be discarded (MATERIAL_ALPHA_MODE_MASK below its cutoff). A surviving
// fragment is fully opaque in both opaque-pass modes: Mask keeps no
// partial coverage and Opaque ignores alpha. A Blend material never
// reaches the opaque pass; if it did, it would be drawn opaque.
bool Material_passes_alpha_test(uint _material_index, float _alpha)
{
    const Material material = material_buffer.materials[_material_index];

    return material.alpha_mode != MATERIAL_ALPHA_MODE_MASK || _alpha >= material.alpha_cutoff;
}

// Distance of a world space point along the view direction (-z in view
// space). Taken from the world position the vertex shader interpolates,
// not from gl_FragCoord.z, which under the infinite reverse-Z projection
// holds near / distance.
float View_depth(vec3 _world_position)
{
    return -(frame.view * vec4(_world_position, 1.0)).z;
}

// Adds the diffuse and specular contribution of one light to the
// accumulators. Shared by both light selection paths, so the clustered
// path and the brute force reference add the same terms in the same order:
// a light left out of a cluster lies entirely beyond its range for every
// point of the cluster, where its attenuation is exactly zero.
void Accumulate_light(Light light, vec3 N, vec3 V, vec3 _world_position, inout vec3 diffuse, inout vec3 specular)
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
        vec3  to_light = light.position_or_direction - _world_position;
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

// Lit color of a surface point, with the active cluster debug view drawn
// over it.
//   _base_color     - Material_base_color(...).rgb;
//   _world_position - interpolated world position of the fragment;
//   _world_normal   - interpolated world normal (normalized here);
//   _frag_coord     - gl_FragCoord.xy, selects the screen tile;
//   _view_depth     - View_depth(_world_position), selects the slice.
vec3 Shade_surface(vec3 _base_color, vec3 _world_position, vec3 _world_normal, vec2 _frag_coord, float _view_depth)
{
    const vec3 N = normalize(_world_normal);
    const vec3 V = normalize(frame.camera_position - _world_position);

    // Cluster of the fragment: screen tile from the fragment coordinates,
    // depth slice from the view space distance.
    const uvec3 cluster       = Cluster_coordinates(_frag_coord, _view_depth);
    const uvec2 cluster_range = cluster_grid.ranges[Cluster_index(cluster)];
    const uint  cluster_count = cluster_range.y & CLUSTER_COUNT_MASK;

    vec3 diffuse  = vec3(0.0);
    vec3 specular = vec3(0.0);

    if (frame.debug_params.x == LIGHT_CULLING_BRUTE_FORCE)
    {
        // Reference path: every light of the buffer.
        for (int i = 0; i < frame.light_count; ++i)
            Accumulate_light(light_buffer.lights[i], N, V, _world_position, diffuse, specular);
    }
    else
    {
        // Directional lights reach every fragment and are never clustered:
        // they occupy the start of the buffer.
        for (uint i = 0u; i < frame.cluster_grid.w; ++i)
            Accumulate_light(light_buffer.lights[i], N, V, _world_position, diffuse, specular);

        // Point and spot lights whose range reaches the cluster, in
        // increasing buffer order.
        for (uint i = 0u; i < cluster_count; ++i)
        {
            const uint light_index = cluster_light_indices.indices[cluster_range.x + i];
            Accumulate_light(light_buffer.lights[light_index], N, V, _world_position, diffuse, specular);
        }
    }

    vec3 color = _base_color * (AMBIENT + diffuse) + specular;

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

    return color;
}

#endif
