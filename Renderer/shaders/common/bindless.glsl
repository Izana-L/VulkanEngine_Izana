#ifndef BINDLESS_GLSL
#define BINDLESS_GLSL

// Set 3: global, bound once per frame and never changed. Unsized array,
// which requires runtimeDescriptorArray (Vulkan_Device enables it and
// only selects devices that support it).
//
// The including shader must declare, BEFORE this include:
//   #extension GL_EXT_nonuniform_qualifier : require
// because the index is per-draw data and can differ between invocations
// of the same subgroup; without nonuniformEXT the driver may assume a
// uniform index and read the wrong descriptor on part of the pixels.
layout(set = 3, binding = 0) uniform sampler2D textures[];

// Usage:
//   vec4 albedo = texture(textures[nonuniformEXT(push.albedo_texture_index)], uv);

#endif
