#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "common/frame_set.glsl"
#include "common/push_constants.glsl"
#include "common/bindless.glsl"

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

// Flat provisional ambient: replaced by IBL once PBR exists.
const vec3 AMBIENT = vec3(0.08, 0.08, 0.10);

// Provisional specular term (Blinn-Phong), the consumer of
// frame.camera_position until the PBR pass replaces it.
const float SPECULAR_STRENGTH = 0.15;
const float SPECULAR_POWER = 32.0;

void main()
{
    vec3 N = normalize(frag_world_normal);
    vec3 V = normalize(frame.camera_position - frag_world_pos);

    // Base color: vertex tint * per-draw tint * albedo texture, read with
    // the sampler preset the material selected. Every draw carries a
    // written texture slot: an untextured draw samples the white default
    // texture, which leaves the tint unchanged.
    vec4 base = frag_color * push.base_color * Sample_bindless(push.albedo_texture_index, push.albedo_sampler_index, frag_uv);

    vec3 diffuse  = vec3(0.0);
    vec3 specular = vec3(0.0);

    for (int i = 0; i < frame.light_count; ++i)
    {
        Light light = light_buffer.lights[i];

        vec3  L           = vec3(0.0);
        float attenuation = 1.0;

        if (light.type == 0)
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

            if (light.type == 2)
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

    vec3 color = base.rgb * (AMBIENT + diffuse) + specular;

    out_color = vec4(color, base.a);
}
