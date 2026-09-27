#ifndef FRAME_SET_GLSL
#define FRAME_SET_GLSL

// EXACT mirror of Renderer_System::Frame_UBO (Frame_Data.hpp), std140.
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
const int FRUSTUM_PLANE_COUNT = 5;

layout(set = 0, binding = 0, std140) uniform Frame_UBO
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
} frame;

// Values of debug_params.x, mirror of Renderer_System::Light_Culling_Mode.
const uint LIGHT_CULLING_CLUSTERED   = 0u;   // directional lights + the list of the fragment's cluster
const uint LIGHT_CULLING_BRUTE_FORCE = 1u;   // every light of the buffer (reference)

// Values of debug_params.y, mirror of Renderer_System::Cluster_Debug_View.
const uint CLUSTER_VIEW_NONE          = 0u;
const uint CLUSTER_VIEW_LIGHT_HEATMAP = 1u;
const uint CLUSTER_VIEW_DEPTH_SLICES  = 2u;
const uint CLUSTER_VIEW_CLUSTERS      = 3u;

// EXACT mirror of Renderer_System::Light_GPU, std430.
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
// buffer capacity (MAX_LIGHTS in Frame_Data.hpp) does not touch this file.
// readonly: the shader never writes, and saying so lets the driver optimize.
//
// Order: the frame.cluster_grid.w directional lights first, then the
// point and spot lights (Renderer::Write_frame_uniforms).
layout(set = 0, binding = 1, std430) readonly buffer Light_Buffer
{
    Light lights[];
} light_buffer;

#endif
