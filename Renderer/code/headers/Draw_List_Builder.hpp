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
    //   - the lists of drawn objects, opaque and transparent, with the mesh,
    //     pipeline and triangle winding of each;
    //   - on the CPU-indirect path, the indirect commands and the runs of
    //     consecutive draws that share a pipeline and a winding;
    //   - on the GPU paths, the draw buckets: the groups of opaque objects
    //     that one indirect draw each will render.
    // It also decides which opaque path can draw the frame, and keeps the
    // culling frustum, which can be frozen for debugging.
    //
    // The lists and the warning flags persist between frames so their
    // capacity is reused; a frame rebuilds them from scratch. Every
    // reference of a draw item (pipeline, mesh, transform, material) is
    // validated in every build, before its entry is written.
    //
    // Triangle winding: a model matrix whose upper 3x3 has a negative
    // determinant (a negative scale on an odd number of axes, a reflection)
    // inverts the winding of the mesh, so the front and back faces swap.
    // The builder detects it per object (Draw_Record::mirrored,
    // Object_Flag::Mirrored) and every draw path draws such objects with the
    // opposite front face. The unit of grouping of the opaque draws is
    // therefore the pair (pipeline, winding), not the pipeline alone.
    class Draw_List_Builder
    {
    public:

        // One drawn object of the frame: its entry in the object buffer, its
        // mesh, its pipeline and whether its transform inverts the winding.
        struct Draw_Record
        {
            uint32_t object_index = 0;
            uint32_t mesh_id = 0;
            uint8_t  pipeline_id = 0;
            bool     mirrored = false;
        };

        // A run of consecutive opaque draws that share a pipeline and a
        // winding: the records [first, first + count) of the opaque list.
        struct Pipeline_Run
        {
            uint8_t  pipeline_id = 0;
            bool     mirrored = false;
            uint32_t first = 0;
            uint32_t count = 0;
        };

        // A group of opaque objects drawn by one vkCmdDrawIndexedIndirectCount
        // on the GPU paths: the objects of one pipeline and one winding. The
        // culling pass writes their commands into
        // [first_command, first_command + capacity) of the draw command
        // buffer and counts them in the counter of the bucket, whose index
        // is the position in Get_opaque_buckets(). The capacity is the
        // number of objects assigned to the bucket, so the region can hold
        // all of them whatever the culling keeps.
        struct Draw_Bucket
        {
            uint8_t  pipeline_id = 0;
            bool     mirrored = false;
            uint32_t first_command = 0;
            uint32_t capacity = 0;
        };

        // What a frame asks of the builder.
        struct Build_Settings
        {
            // Path requested for the opaque objects. A GPU path is used only
            // when the frame fits its limits (see Build).
            Opaque_Draw_Path requested_path = Opaque_Draw_Path::Direct;

            // Discard the objects outside the culling frustum. Says WHAT is
            // wanted, not where it is done: with a GPU path the opaque
            // objects are culled by the culling pass and the transparent
            // ones on the CPU; when the frame falls back to a CPU path both
            // lists are culled on the CPU, so a fallback never draws objects
            // that culling was asked to remove.
            bool             frustum_culling = false;

            // VkPhysicalDeviceLimits::maxDrawIndirectCount: the most commands
            // one indirect draw may take.
            uint32_t         max_draw_indirect_count = 0;
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
        void Update_culling_frustum(const Frustum& _view_frustum, bool _freeze);

        // Frustum used for culling this frame, on the CPU (transparent
        // items, and opaque ones when the frame falls back to a CPU path)
        // and, through the frame uniforms, on the GPU.
        const Frustum& Get_culling_frustum() const { return culling_frustum; }

        // =====================================================
        // Lists
        // =====================================================

        // Writes the object buffer _objects (MAX_OBJECTS entries, rewritten
        // from entry 0), fills the opaque and transparent lists and decides
        // the opaque path. Items whose pass_mask lacks the bit of their list
        // are skipped, and so are items that reference a pipeline, mesh,
        // transform or material that does not exist, a released mesh, or a
        // pipeline built for another subpass than the one their list is
        // drawn in (opaque items: Render_Subpass::Opaque, transparent items:
        // Render_Subpass::Transparent). The first invalid item is reported
        // once.
        //
        // The opaque list is built from the validated items only, and the
        // path is decided from that list: an invalid item cannot influence
        // it. The requested GPU path is kept when the opaque objects fit
        // in at most MAX_DRAW_BUCKETS buckets (one per pipeline and
        // winding) and no bucket holds more than
        // _settings.max_draw_indirect_count objects; otherwise the frame
        // uses Cpu_Indirect, and the first fallback is reported once.
        // Get_opaque_path() returns the outcome.
        //
        // Every kept item gets the next entry of the object buffer. Opaque
        // items take indices [0, opaque) and transparent items continue
        // from there, which is what lets the culling pass process
        // [0, opaque) only. Items beyond MAX_OBJECTS are skipped, with a
        // single warning.
        //
        // _settings.frustum_culling: items whose bounding ellipsoid (mesh
        // bounding sphere placed by the model matrix) lies outside the
        // culling frustum get no entry when the CPU culls them (CPU
        // culling, milestone 4.3): always the transparent items, and the
        // opaque ones when no GPU path draws them. Same test as
        // cull_objects.comp: ::Frustum::Intersects_ellipsoid.
        void Build(const RenderPacket& _packet, Object_GPU* _objects,
                   const Mesh_Registry& _meshes, const Pipeline_Registry& _pipelines,
                   uint32_t _material_count, const Build_Settings& _settings);

        // Writes one VkDrawIndexedIndirectCommand per opaque draw into
        // _commands (MAX_OBJECTS entries), in opaque list order.
        void Write_cpu_draw_commands(VkDrawIndexedIndirectCommand* _commands, const Mesh_Registry& _meshes) const;

        // Splits the opaque list into runs that share a pipeline and a
        // winding. The list arrives sorted by pipeline through the sort
        // key, so each pipeline is normally one run, or two when it has
        // mirrored objects that the key did not group.
        void Group_opaque_by_pipeline();

        // Path the opaque objects of the last Build are drawn with: the
        // requested one, or Cpu_Indirect after a fallback.
        Opaque_Draw_Path Get_opaque_path() const { return opaque_path; }

        // True when the culling pass of the last Build has to test the
        // frustum: a GPU path was kept and culling was requested.
        bool Is_opaque_culled_on_gpu() const { return opaque_culled_on_gpu; }

        const std::vector<Draw_Record>& Get_opaque_draws() const { return opaque_draws; }
        const std::vector<Draw_Record>& Get_transparent_draws() const { return transparent_draws; }
        const std::vector<Pipeline_Run>& Get_opaque_runs() const { return opaque_runs; }

        // The buckets of the last Build; empty unless a GPU path was kept.
        const std::vector<Draw_Bucket>& Get_opaque_buckets() const { return opaque_buckets; }

        // Valid opaque items of the frame before any frustum test or the
        // object buffer capacity, for the statistics.
        uint32_t Get_opaque_candidates() const { return opaque_candidates; }

        // Transparent items of the frame before the CPU frustum test, for
        // the statistics.
        uint32_t Get_transparent_candidates() const { return transparent_candidates; }

    private:

        // A validated opaque item, waiting for the path decision.
        struct Valid_Opaque_Item
        {
            const Draw_Item* item = nullptr;
            uint8_t                     pipeline_id = 0;
            bool                        mirrored = false;
            uint32_t                    bucket = 0;   // index in opaque_buckets, on the GPU paths
        };

        // Assigns the first _count validated items to buckets and reserves
        // their regions of the command buffer. False, leaving no buckets,
        // when the frame does not fit the GPU limits.
        bool Build_buckets(size_t _count, uint32_t _max_draw_indirect_count);

        std::vector<Valid_Opaque_Item> valid_opaque;

        std::vector<Draw_Record>   opaque_draws;
        std::vector<Draw_Record>   transparent_draws;
        std::vector<Pipeline_Run>  opaque_runs;
        std::vector<Draw_Bucket>   opaque_buckets;

        Opaque_Draw_Path           opaque_path = Opaque_Draw_Path::Direct;
        bool                       opaque_culled_on_gpu = false;

        uint32_t                   opaque_candidates = 0;
        uint32_t                   transparent_candidates = 0;

        // Frustum used for culling this frame: the packet's, or the frozen
        // one.
        Frustum         culling_frustum;
        Frustum         frozen_frustum;
        bool                       capture_frozen_frustum = false;

        // Each warning is reported once.
        bool                       warned_invalid_item = false;
        bool                       warned_object_overflow = false;
        bool                       warned_gpu_path_fallback = false;
    };

} // namespace Renderer_System
