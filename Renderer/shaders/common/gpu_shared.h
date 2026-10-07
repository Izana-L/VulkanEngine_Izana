#ifndef GPU_SHARED_H
#define GPU_SHARED_H

// Constants shared by C++ and GLSL: ONE definition, included by both sides.
//
//   C++  - Renderer_Limits.hpp, Descriptor_Sets.hpp, Gpu_Layouts.hpp and
//          Render_Debug_Settings.hpp include this file and build their typed
//          constants (constexpr values, enum values) from the macros below.
//   GLSL - frame_set.glsl and the shaders that do not include it
//          (Procedural.comp, oit_composite.frag) include it; every set and
//          binding number, workgroup size and enum value of a shader is one
//          of these macros.
//
// Rules for this file, because two compilers read it:
//   - Only #define of integer literals. A literal valid in both languages:
//     decimal, hexadecimal, and the `u` suffix. No expressions that depend
//     on a C++ or a GLSL feature.
//   - ASCII only.
//   - Macro names carry the GPU_ prefix, so they never collide with the
//     constexpr constants and the GLSL constants that wrap them.
//   - Changing a value here changes both sides at once. The structs of
//     Gpu_Layouts.hpp are NOT here: their layout is still mirrored by hand
//     in the GLSL blocks and pinned by the static_asserts of Gpu_Layouts.hpp.
//
// Editing this file recompiles every shader: Directory.Build.targets lists
// common/*.h among the inputs of the shader target.

// =========================================================
// Descriptor sets (Renderer_System::Descriptor_Set)
// =========================================================
//
// Ordered by frequency of change; frozen, see Descriptor_Sets.hpp.
#define GPU_SET_PER_FRAME       0
#define GPU_SET_PER_PASS        1
#define GPU_SET_PER_MATERIAL    2
#define GPU_SET_BINDLESS        3

// =========================================================
// Bindings
// =========================================================

// Set 0 (Renderer_System::Binding_Per_Frame)
#define GPU_BINDING_FRAME_UBO              0
#define GPU_BINDING_LIGHTS                 1
#define GPU_BINDING_OBJECTS                2
#define GPU_BINDING_CLUSTER_GRID           3
#define GPU_BINDING_CLUSTER_LIGHT_INDICES  4
#define GPU_BINDING_CLUSTER_COUNTERS       5
#define GPU_BINDING_DRAW_COMMANDS          6
#define GPU_BINDING_DRAW_COUNT             7

// Set 1 of the compute pipeline layout (Renderer_System::Binding_Per_Pass)
#define GPU_BINDING_PROCEDURAL_OUTPUT      0
#define GPU_BINDING_CLUSTER_AABBS          1

// Set 1 of the graphics pipeline layout (Renderer_System::Binding_Graphics_Pass)
#define GPU_BINDING_OIT_ACCUMULATION       0
#define GPU_BINDING_OIT_REVEALAGE          1

// Set 2 (Renderer_System::Binding_Per_Material)
#define GPU_BINDING_MATERIALS              0
#define GPU_BINDING_MESHES                 1

// Set 3 (Renderer_System::Binding_Bindless)
#define GPU_BINDING_BINDLESS_TEXTURES      0
#define GPU_BINDING_BINDLESS_SAMPLERS      1

// Set 0 of the shader self-test (Shader_Self_Test.hpp), which has a pipeline
// layout of its own: the test cases it reads and the results it writes
#define GPU_BINDING_SELFTEST_INPUT         0
#define GPU_BINDING_SELFTEST_OUTPUT        1

// =========================================================
// Limits (Renderer_Limits.hpp)
// =========================================================

// Draw buckets of the GPU opaque paths (Renderer_System::MAX_DRAW_BUCKETS).
// The array sizes of Frame_UBO::draw_buckets and Draw_Count::bucket_draw_count.
#define GPU_MAX_DRAW_BUCKETS        32

// Planes of the culling frustum (Renderer_System::FRUSTUM_PLANE_COUNT): left,
// right, bottom, top and near. The array size of Frame_UBO::frustum_planes.
#define GPU_FRUSTUM_PLANE_COUNT     5

// Test cases of each kind the shader self-test runs (selftest.comp,
// Shader_Self_Test.cpp): the size of the arrays of its two buffers.
#define GPU_SELFTEST_MAX_CASES      128

// =========================================================
// Workgroup sizes
// =========================================================
//
// The local size of each compute shader and the divisor of its dispatch on
// the CPU (Renderer_System::Dispatch_group_count): when they differ, a pass
// silently covers only part of its elements.

// cull_objects.comp: one invocation per object.
#define GPU_CULL_GROUP_SIZE         64

// cluster_lights.comp: one invocation per cluster.
#define GPU_CLUSTER_GROUP_SIZE      64

// Procedural.comp: one invocation per texel, in square groups of this side.
#define GPU_PROCEDURAL_GROUP_SIZE   8

// selftest.comp: one invocation per test case.
#define GPU_SELFTEST_GROUP_SIZE     64

// =========================================================
// Enums
// =========================================================

// Frame_UBO::light_culling_mode (Renderer_System::Light_Culling_Mode)
#define GPU_LIGHT_CULLING_CLUSTERED     0u
#define GPU_LIGHT_CULLING_BRUTE_FORCE   1u

// Frame_UBO::cluster_debug_view (Renderer_System::Cluster_Debug_View)
#define GPU_CLUSTER_VIEW_NONE           0u
#define GPU_CLUSTER_VIEW_LIGHT_HEATMAP  1u
#define GPU_CLUSTER_VIEW_DEPTH_SLICES   2u
#define GPU_CLUSTER_VIEW_CLUSTERS       3u

// Bits of Object_GPU::flags (Renderer_System::Object_Flag)
#define GPU_OBJECT_FLAG_PASS_MASK       0xFFu     // bits 0-7: render pass mask
#define GPU_OBJECT_FLAG_ACTIVE          0x100u    // bit 8
#define GPU_OBJECT_FLAG_MIRRORED        0x200u    // bit 9
#define GPU_OBJECT_FLAG_BUCKET_SHIFT    10u       // bits 10-17: draw bucket
#define GPU_OBJECT_FLAG_BUCKET_MASK     0xFFu

// Render pass bits (Renderer_System::Render_Pass_Bit, RenderPacket.hpp, which
// builds its constants from these macros).
#define GPU_RENDER_PASS_OPAQUE          1u
#define GPU_RENDER_PASS_TRANSPARENT     2u

// GPU_Light::type / Light::type (Renderer_System::Light_Type)
#define GPU_LIGHT_TYPE_DIRECTIONAL      0
#define GPU_LIGHT_TYPE_POINT            1
#define GPU_LIGHT_TYPE_SPOT             2

// Material_GPU::alpha_mode (Renderer_System::Alpha_Mode). The mode decides
// how the alpha of the base color is used and which pass draws the object:
//   OPAQUE - the alpha is ignored; opaque pass;
//   MASK   - the fragments below Material_GPU::alpha_cutoff are discarded;
//            opaque pass;
//   BLEND  - weighted blended transparency; transparent pass.
#define GPU_ALPHA_MODE_OPAQUE           0u
#define GPU_ALPHA_MODE_MASK             1u
#define GPU_ALPHA_MODE_BLEND            2u

// =========================================================
// Shader interfaces
// =========================================================
//
// Locations and attachment indices of the stage interfaces. Each one is
// declared once here and read by the GLSL that declares the interface
// (mesh_vertex_input.glsl, mesh_varyings.glsl, the fragment outputs) and by
// the C++ that describes the other end (Vulkan_Vertex_Layout.hpp,
// Vulkan_Render_Pass.cpp), so the two ends cannot drift apart.

// Vertex attributes of the lit mesh vertex (CoreTypes::Vertex_Static_Mesh)
#define GPU_VERTEX_LOCATION_POSITION        0
#define GPU_VERTEX_LOCATION_NORMAL          1
#define GPU_VERTEX_LOCATION_TANGENT         2
#define GPU_VERTEX_LOCATION_UV              3
#define GPU_VERTEX_LOCATION_COLOR           4

// Varyings from mesh.vert to mesh.frag and mesh_oit.frag
#define GPU_VARYING_WORLD_NORMAL            0
#define GPU_VARYING_WORLD_POSITION          1
#define GPU_VARYING_UV                      2
#define GPU_VARYING_COLOR                   3
#define GPU_VARYING_MATERIAL_INDEX          4

// Varying from bounds.vert to bounds.frag
#define GPU_VARYING_OBJECT_FLAGS            0

// Fragment outputs of the transparent subpass (mesh_oit.frag): the position
// in pColorAttachments of the subpass (Vulkan_Render_Pass.cpp)
#define GPU_OIT_OUTPUT_ACCUMULATION         0
#define GPU_OIT_OUTPUT_REVEALAGE            1

// Input attachments of the composite subpass (oit_composite.frag): the
// position in pInputAttachments of the subpass (Vulkan_Render_Pass.cpp)
#define GPU_OIT_INPUT_ACCUMULATION          0
#define GPU_OIT_INPUT_REVEALAGE             1

#endif
