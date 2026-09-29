#pragma once

#include <cstdint>

namespace Renderer_System
{

    // Runtime switches and debug views of the Renderer. Plain values only,
    // with no dependency on Vulkan: EngineCore reads and writes them
    // through Renderer::Get_debug_settings / Set_debug_settings.

    // How mesh.frag selects the lights of a fragment. Mirrored by the
    // LIGHT_CULLING_* constants of frame_set.glsl.
    //   Clustered   - directional lights, then the list of the fragment's
    //                 cluster.
    //   Brute_Force - every light of the buffer: the reference the
    //                 clustered path must match.
    enum class Light_Culling_Mode : uint32_t
    {
        Clustered = 0,
        Brute_Force = 1,
        Count
    };

    // Debug views of the cluster grid, drawn by mesh.frag instead of the
    // lit color. Mirrored by the CLUSTER_VIEW_* constants of frame_set.glsl.
    //   None          - normal shading;
    //   Light_Heatmap - number of lights of the fragment's cluster
    //                   (blue = few, red = Frame_UBO::heatmap_max_lights or
    //                   more, magenta = cluster that lost lights to a full
    //                   index list);
    //   Depth_Slices  - one color per depth slice: bands that get thinner
    //                   towards the camera;
    //   Clusters      - one color per cluster: the tile grid on screen
    //                   combined with the slices.
    enum class Cluster_Debug_View : uint32_t
    {
        None = 0,
        Light_Heatmap = 1,
        Depth_Slices = 2,
        Clusters = 3,
        Count
    };

    // Path of the opaque pass. Every path draws the same objects from the
    // same Geometry_Pool with the same shaders; they differ in who builds
    // the draw commands, which is what makes them comparable at runtime.
    //   Direct       - one vkCmdDrawIndexed per object, recorded by the CPU
    //                  (roadmap milestone 3.1).
    //   Cpu_Indirect - the CPU writes the commands into a host-visible
    //                  buffer; one vkCmdDrawIndexedIndirect per pipeline
    //                  (milestone 3.4).
    //   Gpu_Indirect - cull_objects.comp writes one command per active
    //                  opaque object, without frustum test; one
    //                  vkCmdDrawIndexedIndirectCount (milestone 4.1).
    //   Gpu_Culled   - the same with the frustum test: objects outside the
    //                  culling frustum get no command (milestone 4.2), and
    //                  the transparent items are culled on the CPU against
    //                  the same planes (milestone 4.3).
    // The GPU paths split the opaque objects into buckets, one per pipeline
    // and per triangle winding (objects with a negative determinant are
    // drawn with the opposite front face), and issue one indirect draw per
    // bucket. A frame that needs more than MAX_DRAW_BUCKETS buckets, or a
    // bucket with more objects than maxDrawIndirectCount, uses Cpu_Indirect
    // instead. Only Gpu_Culled asks for culling, wherever it is done: if
    // that frame falls back to Cpu_Indirect, the opaque and the transparent
    // items are both culled on the CPU.
    enum class Opaque_Draw_Path : uint32_t
    {
        Direct = 0,
        Cpu_Indirect = 1,
        Gpu_Indirect = 2,
        Gpu_Culled = 3,
        Count
    };

    // True for the paths whose draw commands are written by the GPU.
    constexpr bool Is_gpu_draw_path(Opaque_Draw_Path _path)
    {
        return _path == Opaque_Draw_Path::Gpu_Indirect || _path == Opaque_Draw_Path::Gpu_Culled;
    }

    // Runtime switches between the old and the new path of each roadmap
    // step, and the debug views that validate them. Read and written
    // between frames (Renderer::Get_debug_settings / Set_debug_settings);
    // a change applies from the next Render call.
    struct Render_Debug_Settings
    {
        // Light selection of mesh.frag (clustered, or every light).
        Light_Culling_Mode light_culling = Light_Culling_Mode::Clustered;

        // Overlay of the cluster grid.
        Cluster_Debug_View cluster_view = Cluster_Debug_View::None;

        // Light count the heatmap shows as full red.
        uint32_t           heatmap_max_lights = 32;

        Opaque_Draw_Path   opaque_path = Opaque_Draw_Path::Gpu_Culled;

        // Culling camera frozen: the frustum of the frame the freeze was
        // enabled on keeps being used for culling while the view moves,
        // so what is discarded becomes visible.
        bool               freeze_culling = false;

        // Wireframe of the bounding volume the culling tests for every
        // object: its mesh bounding sphere placed by the model matrix, an
        // ellipsoid under non-uniform scale or shear (bounds.vert).
        bool               show_bounds = false;

        // Print GPU timings and counters to the console once per second.
        bool               print_stats = false;

        // Isolated GPU timing (Gpu_Timer): a full barrier before every
        // top-level timed scope, so each one measures its pass alone,
        // without overlap. Changes the performance of the frame: the
        // printed totals are not frame times. Meant to compare one pass
        // before and after a change.
        bool               isolate_gpu_timings = false;
    };

    // Readable names, for logs.
    inline const char* To_string(Light_Culling_Mode _mode)
    {
        switch (_mode)
        {
        case Light_Culling_Mode::Clustered:   return "clustered";
        case Light_Culling_Mode::Brute_Force: return "brute force (every light)";
        default:                              return "unknown";
        }
    }

    inline const char* To_string(Cluster_Debug_View _view)
    {
        switch (_view)
        {
        case Cluster_Debug_View::None:          return "none";
        case Cluster_Debug_View::Light_Heatmap: return "light count heatmap";
        case Cluster_Debug_View::Depth_Slices:  return "depth slices";
        case Cluster_Debug_View::Clusters:      return "clusters (tiles x slices)";
        default:                                return "unknown";
        }
    }

    inline const char* To_string(Opaque_Draw_Path _path)
    {
        switch (_path)
        {
        case Opaque_Draw_Path::Direct:       return "direct draws";
        case Opaque_Draw_Path::Cpu_Indirect: return "indirect, commands written by the CPU";
        case Opaque_Draw_Path::Gpu_Indirect: return "indirect count, commands written by the GPU (no culling)";
        case Opaque_Draw_Path::Gpu_Culled:   return "indirect count, GPU frustum culling";
        default:                             return "unknown";
        }
    }

} // namespace Renderer_System
