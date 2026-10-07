#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/frame_set.glsl"
#include "common/scene_data.glsl"
#include "common/safe_math.glsl"

// Vertex inputs - matches CoreTypes::Vertex_Static_Mesh
#include "common/mesh_vertex_input.glsl"

// Outputs to the fragment shaders (mesh.frag, mesh_oit.frag)
#define MESH_VARYINGS_OUTPUT
#include "common/mesh_varyings.glsl"

void main()
{
    // The draw's firstInstance is the index of its entry in the object
    // buffer, and gl_InstanceIndex already includes it.
    Object object = object_buffer.objects[gl_InstanceIndex];

    vec4 world_pos = object.model * vec4(in_position, 1.0);

    gl_Position = frame.view_projection * world_pos;

    // Normal to world space with the normal matrix computed on the CPU
    // (handles non-uniform scale); no per-vertex inverse(). Safe_normalize:
    // a zero normal in the asset, or a collapsed object, gives the zero
    // vector instead of the NaN of normalize(0), which would reach every
    // fragment of the triangle.
    frag_world_normal = Safe_normalize(mat3(object.normal_matrix) * in_normal);

    frag_world_pos      = world_pos.xyz;
    frag_uv             = in_uv;
    frag_color          = in_color;
    frag_material_index = object.material_index;
}
