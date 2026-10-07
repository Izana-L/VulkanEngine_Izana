#ifndef SCENE_DATA_GLSL
#define SCENE_DATA_GLSL

// Set and binding numbers, flag bits and pass bits shared with C++.
#include "gpu_shared.h"

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
// Gpu_Layouts.hpp (Object_GPU, Material_GPU), std430. A field added on one
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

// Bits of Object::flags: Renderer_System::Object_Flag, built from the same
// macros (gpu_shared.h).
const uint OBJECT_FLAG_PASS_MASK    = GPU_OBJECT_FLAG_PASS_MASK;      // bits 0-7: RENDER_PASS_* mask of the draw
const uint OBJECT_FLAG_ACTIVE       = GPU_OBJECT_FLAG_ACTIVE;         // bit 8: drawn this frame; the culling pass skips entries without it
const uint OBJECT_FLAG_MIRRORED     = GPU_OBJECT_FLAG_MIRRORED;       // bit 9: the model matrix inverts the winding (negative determinant of its 3x3);
                                                                      //        a tangent frame built from normal and tangent must flip its bitangent
const uint OBJECT_FLAG_BUCKET_SHIFT = GPU_OBJECT_FLAG_BUCKET_SHIFT;   // bits 10-17: draw bucket of an opaque object on the GPU paths
const uint OBJECT_FLAG_BUCKET_MASK  = GPU_OBJECT_FLAG_BUCKET_MASK;    //             (Draw_Bucket in frame_set.glsl)

// Renderer_System::Render_Pass_Bit: both are built from the same macros
// (gpu_shared.h).
const uint RENDER_PASS_OPAQUE      = GPU_RENDER_PASS_OPAQUE;
const uint RENDER_PASS_TRANSPARENT = GPU_RENDER_PASS_TRANSPARENT;

// Values of Material::alpha_mode: Renderer_System::Alpha_Mode, built from
// the same macros (gpu_shared.h). The mode, not the value of the alpha,
// decides how the alpha of the base color is used and which pass draws the
// object (the Extractor routes BLEND materials to the transparent pass).
const uint ALPHA_MODE_OPAQUE = GPU_ALPHA_MODE_OPAQUE;   // alpha ignored
const uint ALPHA_MODE_MASK   = GPU_ALPHA_MODE_MASK;     // fragments below alpha_cutoff are discarded
const uint ALPHA_MODE_BLEND  = GPU_ALPHA_MODE_BLEND;    // weighted blended transparency

// EXACT mirror of Renderer_System::Material_GPU, std430, 32 bytes.
struct Material
{
    vec4  base_color;           // offset 0:  tint multiplied with the vertex color and the albedo sample
    uint  albedo_texture_index; // offset 16: bindless texture slot, always a written slot
    uint  albedo_sampler_index; // offset 20: slot in samplers[] (a CoreTypes::Sampler_Preset value)
    uint  alpha_mode;           // offset 24: ALPHA_MODE_*
    float alpha_cutoff;         // offset 28: ALPHA_MODE_MASK threshold, in [0, 1]
};

// Unsized arrays: raising MAX_OBJECTS or MAX_MATERIALS (Renderer_Limits.hpp)
// does not touch this file. readonly: no graphics shader writes them.
layout(set = GPU_SET_PER_FRAME, binding = GPU_BINDING_OBJECTS, std430) readonly buffer Object_Buffer
{
    Object objects[];
} object_buffer;

layout(set = GPU_SET_PER_MATERIAL, binding = GPU_BINDING_MATERIALS, std430) readonly buffer Material_Buffer
{
    Material materials[];
} material_buffer;

#endif