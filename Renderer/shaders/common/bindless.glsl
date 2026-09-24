#ifndef BINDLESS_GLSL
#define BINDLESS_GLSL

// Set 3: global, bound once per frame and never changed. Two unsized
// arrays, which require runtimeDescriptorArray (Vulkan_Device enables it
// and only selects devices that support it):
//   binding 0 - every uploaded texture, without a sampler
//               (Binding_Bindless::Textures);
//   binding 1 - the samplers, one slot per CoreTypes::Sampler_Preset
//               (Binding_Bindless::Samplers).
// Image and sampler are combined at the point of use, so the material
// chooses the filtering independently of the texture.
//
// The including shader must declare, BEFORE this include:
//   #extension GL_EXT_nonuniform_qualifier : require
//
// On nonuniformEXT: today both indices come from push constants, which
// are dynamically uniform within a draw, so the qualifier is not strictly
// required yet. It is kept because it becomes mandatory as soon as an
// index comes from per-instance data, a material buffer or a GPU-driven
// pass: the index can then differ between invocations of the same
// subgroup, and without the qualifier the driver may read the wrong
// descriptor on part of the pixels. On a uniform value its cost is
// minimal.
layout(set = 3, binding = 0) uniform texture2D textures[];
layout(set = 3, binding = 1) uniform sampler   samplers[];

// Samples texture `texture_index` with sampler `sampler_index`.
// nonuniformEXT is applied here, on the three places that need it: both
// array accesses and the combined sampler2D. The combined value is the
// operand the sampling instruction actually reads, so it must carry the
// NonUniform decoration as well, and glslang does not propagate it from
// the array accesses on its own. A qualifier on the caller's arguments
// does not survive the call, so wrapping them at the call site would not
// be enough.
vec4 Sample_bindless(uint texture_index, uint sampler_index, vec2 uv)
{
    return texture(nonuniformEXT(sampler2D(textures[nonuniformEXT(texture_index)], samplers[nonuniformEXT(sampler_index)])), uv);
}

#endif