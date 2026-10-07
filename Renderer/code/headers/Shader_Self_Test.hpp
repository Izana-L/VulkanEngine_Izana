#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Upload_Context.hpp>
#include <Vulkan_Device.hpp>

#include <string>

namespace Renderer_System::Shader_Self_Test
{

    // Shader_Self_Test: compares, on the GPU, the arithmetic that exists
    // twice, once in GLSL and once in C++.
    //
    // Three computations are written in both languages and used by both:
    //   - the fragment to cluster mapping (cluster_math.glsl, used by
    //     mesh.frag; its C++ twin is Cluster_Grid::Find_cluster and
    //     Cluster_index, which the boxes of cluster_lights.comp are built
    //     with);
    //   - the bounding ellipsoid against the frustum (bounding_math.glsl,
    //     used by cull_objects.comp; its C++ twin is
    //     Frustum::Intersects_ellipsoid, which culls the transparent items
    //     on the CPU);
    //   - Safe_normalize (safe_math.glsl), which must keep a zero vector
    //     finite.
    // Nothing else connected them: a change to one copy that breaks the
    // other shows as lights missing in a sliver of the screen, or as
    // objects culled by one pass and not by the other, with no error.
    //
    // The test dispatches selftest.comp, which includes those same GLSL
    // files (not copies), over a table of cases, reads the results back and
    // compares them with the C++ twins. The cases are chosen away from the
    // places where the two sides may legitimately differ by rounding (a slice
    // boundary, a plane the ellipsoid just touches), so a mismatch is a real
    // disagreement. It takes well under a millisecond of GPU time and has
    // resources of its own (set layout, pipeline, three small buffers),
    // all released before it returns: it can run once at startup, in every
    // build, before any frame is recorded.

    // Runs the test and returns an empty string when every case agrees, or
    // a report of the disagreements (the first few of each kind). Throws
    // Vulkan_Error or std::runtime_error when the test itself cannot run
    // (the shader is missing, the device fails): that is not a mismatch.
    //
    // _upload_context: its synchronous submit runs and waits for the
    // dispatch, and must not be in the middle of a transfer.
    std::string Run(const Vulkan_Device& _device, VmaAllocator _allocator, VkPipelineCache _pipeline_cache,
                    Upload_Context& _upload_context);

} // namespace Renderer_System::Shader_Self_Test
