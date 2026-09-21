#ifndef BINDLESS_GLSL
#define BINDLESS_GLSL

// Set 3: global, se enlaza una vez por fotograma y no cambia nunca.
// Sin tamano declarado -> requiere runtimeDescriptorArray, que
// Vulkan_Device ya activa.
layout(set = 3, binding = 0) uniform sampler2D textures[];

// Uso:
//   vec4 albedo = texture(textures[nonuniformEXT(material.albedo_index)], uv);
// nonuniformEXT es OBLIGATORIO cuando el indice puede variar entre
// invocaciones del mismo grupo — sin el, el driver puede asumir un indice
// uniforme y leer el descriptor equivocado en la mitad de los pixeles.

#endif