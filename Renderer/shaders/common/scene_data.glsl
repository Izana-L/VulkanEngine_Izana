#ifndef SCENE_DATA_GLSL
#define SCENE_DATA_GLSL

// Per-object and per-material data, read by index instead of through push
// constants. Two buffers:
//   set 0, binding 2 (Binding_Per_Frame::Objects)     - one Object per draw,
//                                                       rewritten every frame;
//   set 2, binding 0 (Binding_Per_Material::Materials) - one Material per
//                                                       registered material.
//
// A draw passes the index of its object as firstInstance, so the vertex
// shader reads objects[gl_InstanceIndex] (Vulkan includes firstInstance in
// gl_InstanceIndex). The object carries the index of its material, which
// the vertex shader forwards to the fragment shader as a flat varying.
//
// Both structs are EXACT mirrors of their C++ counterparts in
// Frame_Data.hpp (Object_GPU, Material_GPU), std430. A field added on one
// side and not the other shifts every following entry of the array; the
// C++ side pins sizes and offsets with static_asserts.

// EXACT mirror of Renderer_System::Object_GPU, std430, 144 bytes.
struct Object
{
    mat4 model;                 // offset 0:   world matrix
    mat4 normal_matrix;         // offset 64:  transpose(inverse(model)); only the upper 3x3 is used
    uint material_index;        // offset 128: slot in materials[]
    uint mesh_index;            // offset 132: Renderer mesh registry id = slot in the mesh table (mesh_table.glsl)
    uint flags;                 // offset 136: OBJECT_FLAG_* bits
    uint _pad0;                 // offset 140
};

// Bits of Object::flags, mirror of Renderer_System::Object_Flag.
const uint OBJECT_FLAG_PASS_MASK = 0xFFu;    // bits 0-7: RENDER_PASS_* mask of the draw
const uint OBJECT_FLAG_ACTIVE    = 0x100u;   // bit 8: drawn this frame; the culling pass skips entries without it

// Mirror of CoreTypes::Render_Pass_Bit.
const uint RENDER_PASS_OPAQUE      = 1u;
const uint RENDER_PASS_TRANSPARENT = 2u;

// EXACT mirror of Renderer_System::Material_GPU, std430, 32 bytes.
struct Material
{
    vec4 base_color;            // offset 0:  tint, alpha < 1 = transparent pass
    uint albedo_texture_index;  // offset 16: bindless texture slot, always a written slot
    uint albedo_sampler_index;  // offset 20: slot in samplers[] (a CoreTypes::Sampler_Preset value)
    uint _pad0;                 // offset 24
    uint _pad1;                 // offset 28
};

// Unsized arrays: raising MAX_OBJECTS or MAX_MATERIALS (Frame_Data.hpp)
// does not touch this file. readonly: no graphics shader writes them.
layout(set = 0, binding = 2, std430) readonly buffer Object_Buffer
{
    Object objects[];
} object_buffer;

layout(set = 2, binding = 0, std430) readonly buffer Material_Buffer
{
    Material materials[];
} material_buffer;

#endif