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
// Visibility: both arrays are visible to the fragment and compute stages
// (Bindless_Reader_Stages, Shader_Stages.hpp). Reading them from any
// other stage requires adding that stage there first.
//
// The including shader must declare, BEFORE this include:
//   #extension GL_EXT_nonuniform_qualifier : require
// and, to read exact texels with texelFetch / textureSize directly on
// textures[i] (no sampler involved), also:
//   #extension GL_EXT_samplerless_texture_functions : require
// A direct access to textures[] does not go through the helpers below,
// so the index needs its own nonuniformEXT at the call site.
//
// Level of detail outside the fragment stage: there are no derivatives,
// so the implicit level of detail of texture() (and of Sample_bindless)
// is 0 and the base mip level is always read. glslang compiles it as an
// explicit Lod 0 in those stages. Sample_bindless_lod selects the level
// explicitly and is the one to use from compute.
//
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

// Same as Sample_bindless with an explicit level of detail: `lod` selects
// the mip level, and a fractional value blends two levels when the
// sampler uses linear mipmap filtering. Valid in every stage; required
// outside the fragment stage to read any level other than the base one.
// nonuniformEXT is applied on the same three places, for the same reason.
vec4 Sample_bindless_lod(uint texture_index, uint sampler_index, vec2 uv, float lod)
{
    return textureLod(nonuniformEXT(sampler2D(textures[nonuniformEXT(texture_index)], samplers[nonuniformEXT(sampler_index)])), uv, lod);
}



#endif