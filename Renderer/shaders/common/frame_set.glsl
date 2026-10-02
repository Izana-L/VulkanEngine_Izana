#ifndef FRAME_SET_GLSL
#define FRAME_SET_GLSL

// Set and binding numbers, array sizes and enum values shared with C++.
#include "gpu_shared.h"

// EXACT mirror of Renderer_System::Frame_UBO (Gpu_Layouts.hpp), std140.
// A field added here and not there (or the other way round) shifts every
// following offset without any warning. Both are edited in the same
// commit, always, and the C++ side pins the offsets with static_asserts.
//
// Fields and their consumers:
//   view               - mesh.frag (view depth of the fragment),
//                        cluster_lights.comp (lights to view space)
//   projection         - none today; kept for the passes that need it
//                        apart (position reconstruction from depth)
//   view_projection    - mesh.vert, bounds.vert
//   camera_position    - view-dependent lighting terms (mesh.frag)
//   light_count        - mesh.frag, cluster_lights.comp (through its push
//                        constants)
//   cluster_grid       - mesh.frag: tiles X, tiles Y, slices, and in w the
//                        number of directional lights, stored first in the
//                        light buffer and never clustered
//   cluster_params     - mesh.frag: render width and height in pixels, and
//                        the scale and bias of the depth slice mapping
//                        slice = floor(log(view_depth) * scale + bias)
//   debug_params       - mesh.frag: x = LIGHT_CULLING_*, y = CLUSTER_VIEW_*,
//                        z = light count shown as full red by the heatmap
//   frustum_planes     - cull_objects.comp: world space planes, normals
//                        pointing inside; a point p is inside a plane when
//                        dot(plane.xyz, p) + plane.w >= 0. Left, right,
//                        bottom, top and near: the projection has no far
//                        plane.
//   draw_buckets       - cull_objects.comp: where each draw bucket starts in
//                        the draw command buffer and how many commands it
//                        holds. The bucket of an object is in its flags
//                        (scene_data.glsl).
const int FRUSTUM_PLANE_COUNT = GPU_FRUSTUM_PLANE_COUNT;

// Renderer_System::MAX_DRAW_BUCKETS (Renderer_Limits.hpp): the same macro.
const int MAX_DRAW_BUCKETS = GPU_MAX_DRAW_BUCKETS;

// EXACT mirror of Renderer_System::Draw_Bucket_GPU, std140, 16 bytes.
struct Draw_Bucket
{
    uint first_command;   // index of the bucket's first command in draw_commands
    uint capacity;        // commands reserved for the bucket
    uint _pad0;
    uint _pad1;
};

layout(set = GPU_SET_PER_FRAME, binding = GPU_BINDING_FRAME_UBO, std140) uniform Frame_UBO
{
    mat4  view;
    mat4  projection;
    mat4  view_projection;
    vec3  camera_position;
    int   light_count;
    uvec4 cluster_grid;
    vec4  cluster_params;
    uvec4 debug_params;
    vec4  frustum_planes[FRUSTUM_PLANE_COUNT];
    Draw_Bucket draw_buckets[MAX_DRAW_BUCKETS];
} frame;

// Values of debug_params.x: Renderer_System::Light_Culling_Mode, built from
// the same macros (gpu_shared.h).
const uint LIGHT_CULLING_CLUSTERED   = GPU_LIGHT_CULLING_CLUSTERED;     // directional lights + the list of the fragment's cluster
const uint LIGHT_CULLING_BRUTE_FORCE = GPU_LIGHT_CULLING_BRUTE_FORCE;   // every light of the buffer (reference)

// Values of debug_params.y: Renderer_System::Cluster_Debug_View, built from
// the same macros (gpu_shared.h).
const uint CLUSTER_VIEW_NONE          = GPU_CLUSTER_VIEW_NONE;
const uint CLUSTER_VIEW_LIGHT_HEATMAP = GPU_CLUSTER_VIEW_LIGHT_HEATMAP;
const uint CLUSTER_VIEW_DEPTH_SLICES  = GPU_CLUSTER_VIEW_DEPTH_SLICES;
const uint CLUSTER_VIEW_CLUSTERS      = GPU_CLUSTER_VIEW_CLUSTERS;

// EXACT mirror of CoreTypes::GPU_Light (RenderPacket.hpp), std430.
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

const int LIGHT_TYPE_DIRECTIONAL = 0;
const int LIGHT_TYPE_POINT       = 1;
const int LIGHT_TYPE_SPOT        = 2;

// Unsized array: what an SSBO allows and a UBO does not. Raising the
// buffer capacity (MAX_LIGHTS in Renderer_Limits.hpp) does not touch this file.
// readonly: the shader never writes, and saying so lets the driver optimize.
//
// Order: the frame.cluster_grid.w directional lights first, then the
// point and spot lights (Renderer::Write_frame_uniforms).
layout(set = GPU_SET_PER_FRAME, binding = GPU_BINDING_LIGHTS, std430) readonly buffer Light_Buffer
{
    Light lights[];
} light_buffer;

#endif
