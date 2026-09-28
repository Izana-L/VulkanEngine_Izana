#pragma once

#include <vulkan/vulkan.h>

#include <Gpu_Layouts.hpp>
#include <Mesh_Registry.hpp>
#include <Pipeline_Registry.hpp>
#include <Render_Debug_Settings.hpp>
#include <RenderPacket.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Draw_List_Builder: the CPU side of one frame's draws. It turns the
    // draw items of a RenderPacket into what the passes consume:
    //   - the entries of the per-frame object buffer (Object_GPU), one per
    //     draw, whose index is the draw's firstInstance;
    //   - the lists of drawn objects, opaque and transparent, with the mesh
    //     and pipeline of each;
    //   - on the CPU-indirect path, the indirect commands and the runs of
    //     consecutive draws that share a pipeline.
    // It also decides which opaque path can draw the frame, and keeps the
    // culling frustum, which can be frozen for debugging.
    //
    // The lists and the warning flags persist between frames so their
    // capacity is reused; a frame rebuilds them from scratch. Every
    // reference of a draw item (pipeline, mesh, transform, material) is
    // validated in every build, before its entry is written.
    class Draw_List_Builder
    {
    public:

        // One drawn object of the frame: its entry in the object buffer, its
        // mesh and its pipeline.
        struct Draw_Record
        {
            uint32_t object_index = 0;
            uint32_t mesh_id = 0;
            uint8_t  pipeline_id = 0;
        };

        // A run of consecutive opaque draws that share a pipeline: the
        // records [first, first + count) of the opaque list.
        struct Pipeline_Run
        {
            uint8_t  pipeline_id = 0;
            uint32_t first = 0;
            uint32_t count = 0;
        };

        // Reserves the lists at their maximum size once, instead of growing
        // them during the first frames.
        Draw_List_Builder();

        // =====================================================
        // Culling frustum
        // =====================================================

        // The next Update_culling_frustum call stores its frustum as the
        // frozen one. Called when the freeze is enabled, so the frustum of
        // the next frame is the one that stays.
        void Request_frustum_capture();

        // Chooses the frustum of this frame: _view_frustum, or the frozen
        // one while _freeze is set. The first frame after a capture
        // request provides the frozen planes; later frames keep testing
        // against them while the view moves on.
        void Update_culling_frustum(const CoreTypes::Frustum& _view_frustum, bool _freeze);

        // Frustum used for culling this frame, on the CPU (transparent
        // items) and, through the frame uniforms, on the GPU.
        const CoreTypes::Frustum& Get_culling_frustum() const { return culling_frustum; }

        // =====================================================
        // Path
        // =====================================================

        // Opaque path that can actually draw _packet: _requested, or
        // Cpu_Indirect when a GPU path cannot represent the frame. The GPU
        // paths write every command into one bucket, drawn with one
        // pipeline by one vkCmdDrawIndexedIndirectCount, so they need every
        // opaque item on one pipeline and at most
        // _max_draw_indirect_count of them (the device limit). The first
        // fallback is reported once.
        Opaque_Draw_Path Resolve_opaque_path(Opaque_Draw_Path _requested, const CoreTypes::RenderPacket& _packet,
                                             uint32_t _max_draw_indirect_count);

        // =====================================================
        // Lists
        // =====================================================

        // Writes the object buffer _objects (MAX_OBJECTS entries, rewritten
        // from entry 0) and fills the opaque and transparent lists. Items
        // whose pass_mask lacks the bit of their list are skipped, and so
        // are items that reference a pipeline, mesh, transform or material
        // that does not exist, a released mesh, or a pipeline built for
        // another subpass than the one their list is drawn in (opaque
        // items: Render_Subpass::Opaque, transparent items:
        // Render_Subpass::Transparent). The first invalid item is reported
        // once.
        //
        // Every kept item gets the next entry of the object buffer. Opaque
        // items take indices [0, opaque) and transparent items continue
        // from there, which is what lets the culling pass process
        // [0, opaque) only. Items beyond MAX_OBJECTS are skipped, with a
        // single warning.
        //
        // _cull_transparents: transparent items whose bounding ellipsoid
        // (mesh bounding sphere placed by the model matrix) lies outside
        // the culling frustum get no entry (CPU culling, milestone 4.3).
        // Same test as cull_objects.comp: CoreTypes::Frustum::
        // Intersects_ellipsoid.
        void Build(const CoreTypes::RenderPacket& _packet, Object_GPU* _objects,
                   const Mesh_Registry& _meshes, const Pipeline_Registry& _pipelines,
                   uint32_t _material_count, bool _cull_transparents);

        // Writes one VkDrawIndexedIndirectCommand per opaque draw into
        // _commands (MAX_OBJECTS entries), in opaque list order.
        void Write_cpu_draw_commands(VkDrawIndexedIndirectCommand* _commands, const Mesh_Registry& _meshes) const;

        // Splits the opaque list into runs that share a pipeline. The list
        // arrives sorted by pipeline through the sort key, so each pipeline
        // is normally one run.
        void Group_opaque_by_pipeline();

        const std::vector<Draw_Record>& Get_opaque_draws() const { return opaque_draws; }
        const std::vector<Draw_Record>& Get_transparent_draws() const { return transparent_draws; }
        const std::vector<Pipeline_Run>& Get_opaque_runs() const { return opaque_runs; }

        // Transparent items of the frame before the CPU frustum test, for
        // the statistics.
        uint32_t Get_transparent_candidates() const { return transparent_candidates; }

    private:

        std::vector<Draw_Record>   opaque_draws;
        std::vector<Draw_Record>   transparent_draws;
        std::vector<Pipeline_Run>  opaque_runs;

        uint32_t                   transparent_candidates = 0;

        // Frustum used for culling this frame: the packet's, or the frozen
        // one.
        CoreTypes::Frustum         culling_frustum;
        CoreTypes::Frustum         frozen_frustum;
        bool                       capture_frozen_frustum = false;

        // Each warning is reported once.
        bool                       warned_invalid_item = false;
        bool                       warned_object_overflow = false;
        bool                       warned_gpu_path_fallback = false;
    };

} // namespace Renderer_System
