#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/frame_set.glsl"


layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

// Ambiente plano provisional: sustituir por IBL cuando haya PBR.
const vec3 AMBIENT = vec3(0.08, 0.08, 0.10);

void main()
{
    vec3 N = normalize(frag_world_normal);

    vec3 diffuse = vec3(0.0);

    for (int i = 0; i < frame.light_count; ++i)
    {
        Light light = light_buffer.lights[i];

        vec3  L           = vec3(0.0);
        float attenuation = 1.0;

        if (light.type == 0)
        {
            // Direccional: el paquete guarda hacia DONDE APUNTA la luz,
            // y aqui hace falta el vector HACIA la luz. De ahi el signo.
            L = normalize(-light.position_or_direction);
        }
        else
        {
            // Point y spot: position_or_direction es una posicion.
            vec3  to_light = light.position_or_direction - frag_world_pos;
            float dist     = length(to_light);

            L = to_light / max(dist, 1e-4);

            // Falloff suave que llega a 0 exactamente en range, para que
            // una luz no corte de golpe en el borde de su radio.
            float falloff = clamp(1.0 - dist / max(light.range, 1e-4), 0.0, 1.0);
            attenuation   = falloff * falloff;

            if (light.type == 2)
            {
                // Cono del spot: angulo entre el eje del cono y el
                // fragmento. inner = intensidad plena, outer = 0.
                float cos_angle = dot(normalize(light.spot_direction), -L);
                float cos_inner = cos(light.inner_angle);
                float cos_outer = cos(light.outer_angle);

                attenuation *= smoothstep(cos_outer, cos_inner, cos_angle);
            }
        }

        float n_dot_l = max(dot(N, L), 0.0);

        diffuse += light.color * light.intensity * attenuation * n_dot_l;
    }

    vec3 base_color = frag_color.rgb;
    vec3 color      = base_color * (AMBIENT + diffuse);

    out_color = vec4(color, 1.0);
}
