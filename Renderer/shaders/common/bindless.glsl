#ifndef FRAME_SET_GLSL
#define FRAME_SET_GLSL

// Espejo EXACTO de Renderer_System::Frame_UBO (Frame_Data.hpp), std140.
// Un campo anadido aqui y no alli (o al reves) desplaza todos los offsets
// posteriores sin que nada avise. Se editan en el mismo commit, siempre.
layout(set = 0, binding = 0, std140) uniform Frame_UBO
{
    mat4  view;
    mat4  projection;
    mat4  view_projection;
    mat4  inv_view;
    mat4  inv_projection;
    vec3  camera_position;
    int   light_count;
    float time;
    float delta_time;
    float _pad0;
    float _pad1;
} frame;

// Espejo EXACTO de Renderer_System::Light_GPU, std430.
struct Light
{
    vec3  position_or_direction;
    float intensity;
    vec3  color;
    float range;
    vec3  spot_direction;
    float inner_angle;
    float outer_angle;
    int   type;                 // 0 = directional, 1 = point, 2 = spot
    float _pad0;
    float _pad1;
};

// Array de tamano NO DECLARADO. Es lo que un SSBO permite y un UBO no:
// se acabo el "#define MAX_LIGHTS 16 que debe coincidir con el C++".
// Subir la capacidad del buffer ya no toca este fichero.
// readonly: el shader no escribe, y decirlo deja optimizar al driver.
layout(set = 0, binding = 1, std430) readonly buffer Light_Buffer
{
    Light lights[];
} light_buffer;

#endif