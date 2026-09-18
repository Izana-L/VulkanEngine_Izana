#version 450

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

// Hardcoded directional light for the Fase 1 test.
// Direction matches the light entity in Setup_scene (pitch -45, yaw 45).
const vec3  LIGHT_DIR       = normalize(vec3(0.707, 0.707, 0.0));
const vec3  LIGHT_COLOR     = vec3(1.0, 0.98, 0.95);
const float LIGHT_INTENSITY = 1.0;
const vec3  AMBIENT         = vec3(0.08, 0.08, 0.10);

void main()
{
    vec3 N = normalize(frag_world_normal);

    // Lambertian diffuse.
    float n_dot_l = max(dot(N, LIGHT_DIR), 0.0);

    vec3 base_color = frag_color.rgb;
    vec3 color = base_color * (AMBIENT + LIGHT_COLOR * LIGHT_INTENSITY * n_dot_l);

    out_color = vec4(color, 1.0);
}
